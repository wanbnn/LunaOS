#include "boot.h"
#include "install.h"
#include "storage.h"
#include "task.h"
static DiskInfo disks[4];
static unsigned boot_index;
static const uint8_t *ram_source;
static uint32_t cd_image_lba;
static int cd_cached_device = -1;
static uint32_t cd_cached_lba;
static uint8_t cd_cache[2048];
static bool legacy_busy[2];
static uint16_t port(unsigned index) {
    return index < 2 ? 0x1f0 : 0x170;
}
static uint8_t read_status(unsigned index) {
    return inb(port(index) + 7);
}
static bool idle(unsigned index, bool data) {
    uint32_t start = drv_now_ms();
    unsigned polls = 0;
    do {
        uint8_t status = read_status(index);
        if (!status || status == 255)
            return false;
        if (!(status & 0x80)) {
            if (status & 0x21)
                return false;
            if (!data || (status & 8))
                return true;
        }
        storage_poll_wait(&polls);
    } while (drv_now_ms() - start < 5000);
    return false;
}
static bool not_busy(unsigned index) {
    uint32_t start = drv_now_ms();
    unsigned polls = 0;
    while (read_status(index) & 0x80) {
        if (drv_now_ms() - start >= 5000)
            return false;
        storage_poll_wait(&polls);
    }
    return true;
}
static bool enter_channel(unsigned index) {
    unsigned channel = index / 2;
    uint32_t start = drv_now_ms();
    unsigned polls = 32;
    while (legacy_busy[channel]) {
        if (!current_task || !drv_irqs_enabled() || drv_in_irq() || drv_now_ms() - start >= 5000)
            return false;
        storage_poll_wait(&polls);
    }
    legacy_busy[channel] = true;
    return true;
}
static void select_drive(unsigned index, uint8_t head) {
    uint16_t base = port(index);
    outb(base + 6, head | ((index & 1) << 4));
    for (unsigned i = 0; i < 4; i++)
        inb(base + 0x206);
}
static void read_io(unsigned index, void *data, unsigned count) {
    __asm__ volatile("cld; rep insw" : "+D"(data), "+c"(count) : "d"(port(index)) : "memory");
}
static void write_io(unsigned index, const void *data, unsigned count) {
    __asm__ volatile("cld; rep outsw" : "+S"(data), "+c"(count) : "d"(port(index)) : "memory");
}
static bool command(unsigned index, uint32_t lba, unsigned count, uint8_t cmd) {
    if (index >= 4 || !disks[index].present || !disks[index].supported || disks[index].optical ||
        !count || count > 128 || lba >= 0x10000000u ||
        (uint64_t)lba + count > disks[index].sectors || (uint64_t)lba + count > 0x10000000ull)
        return false;
    uint16_t base = port(index);
    select_drive(index, 0xe0 | ((lba >> 24) & 15));
    if (!not_busy(index))
        return false;
    /* Polling backend: do not leave an unhandled IDE IRQ pending on shared PIC lines. */
    outb(base + 0x206, 2);
    outb(base + 2, count);
    outb(base + 3, lba);
    outb(base + 4, lba >> 8);
    outb(base + 5, lba >> 16);
    outb(base + 7, cmd);
    return idle(index, true);
}
/* ATAPI PACKET READ(12), PIO data-in phases. No optical write command exists here. */
static bool atapi_read(unsigned index, uint32_t lba, unsigned blocks, void *data) {
    if (index >= 4 || !disks[index].optical || !blocks || blocks > 16 || !data)
        return false;
    uint16_t base = port(index);
    select_drive(index, 0xa0);
    if (!not_busy(index))
        return false;
    outb(base + 0x206, 2);
    outb(base + 1, 0);
    outb(base + 2, 0);
    outb(base + 3, 0);
    outb(base + 4, 0);
    outb(base + 5, 8);
    outb(base + 7, 0xa0);
    if (!idle(index, true) || (inb(base + 2) & 3) != 1)
        return false;
    uint8_t packet[12] = {0xa8, 0};
    packet[2] = lba >> 24;
    packet[3] = lba >> 16;
    packet[4] = lba >> 8;
    packet[5] = lba;
    packet[6] = blocks >> 24;
    packet[7] = blocks >> 16;
    packet[8] = blocks >> 8;
    packet[9] = blocks;
    write_io(index, packet, 6);
    unsigned remaining = blocks * 2048;
    uint8_t *out = data;
    while (remaining) {
        if (!idle(index, true) || (inb(base + 2) & 3) != 2)
            return false;
        unsigned bytes = inb(base + 4) | ((unsigned)inb(base + 5) << 8);
        if (!bytes || bytes > remaining || (bytes & 1))
            return false;
        read_io(index, out, bytes / 2);
        out += bytes;
        remaining -= bytes;
    }
    return idle(index, false);
}
static bool cd_read_image(unsigned index, uint32_t lba, unsigned count, void *data) {
    if (index != boot_index || !disks[index].supported || !count || count > 128 ||
        (uint64_t)lba + count > LUNA_IMAGE_BYTES / 512)
        return false;
    uint8_t *out = data;
    while (count) {
        uint32_t sector = cd_image_lba + lba / 4;
        if (!(lba & 3) && count >= 4) {
            unsigned blocks = count / 4;
            if (blocks > 16)
                blocks = 16;
            if (!atapi_read(index, sector, blocks, out))
                return false;
            lba += blocks * 4;
            count -= blocks * 4;
            out += blocks * 2048;
        } else {
            if (cd_cached_device != (int)index || cd_cached_lba != sector) {
                if (!atapi_read(index, sector, 1, cd_cache))
                    return false;
                cd_cached_device = index;
                cd_cached_lba = sector;
            }
            unsigned n = 4 - (lba & 3);
            if (n > count)
                n = count;
            memcpy(out, cd_cache + (lba & 3) * 512, n * 512);
            lba += n;
            count -= n;
            out += n * 512;
        }
    }
    return true;
}
static bool native_transfer(unsigned index, uint32_t lba, unsigned count, void *data, bool write) {
    uint32_t start = drv_now_ms();
    unsigned polls = 32;
    int result;
    do {
        result = storage_transfer(index, lba, count, data, write);
        if (result != DRV_EBUSY || !current_task || !drv_irqs_enabled() || drv_in_irq())
            return result == 0;
        storage_poll_wait(&polls);
    } while (drv_now_ms() - start < 5000);
    return false;
}
static bool read_blocks(unsigned index, uint32_t lba, unsigned count, void *data) {
    if(index>=STORAGE_NATIVE_BASE)
        return native_transfer(index, lba, count, data, false);
    if (index == 4 && ram_source) {
        if (!data || !count || count > 128 || (uint64_t)lba + count > LUNA_IMAGE_BYTES / 512)
            return false;
        memcpy(data, ram_source + (size_t)lba * 512, count * 512);
        return true;
    }
    if (index < 4 && disks[index].optical)
        return data && cd_read_image(index, lba, count, data);
    unsigned group = index < 4 && count >= disks[index].multiple && disks[index].multiple > 1
                         ? disks[index].multiple
                         : 1;
    if (!data || !command(index, lba, count, group > 1 ? 0xc4 : 0x20))
        return false;
    uint8_t *p = data;
    for (unsigned i = 0; i < count; i += group) {
        if (!idle(index, true))
            return false;
        unsigned n = count - i;
        if (n > group)
            n = group;
        read_io(index, p + i * 512, n * 256);
    }
    return true;
}
static bool write_blocks(unsigned index, uint32_t lba, unsigned count, const void *data) {
    if(index>=STORAGE_NATIVE_BASE)
        return native_transfer(index, lba, count, (void *)data, true);
    unsigned group = index < 4 && count >= disks[index].multiple && disks[index].multiple > 1
                         ? disks[index].multiple
                         : 1;
    if (!data || !command(index, lba, count, group > 1 ? 0xc5 : 0x30))
        return false;
    const uint8_t *p = data;
    for (unsigned i = 0; i < count; i += group) {
        if (!idle(index, true))
            return false;
        unsigned n = count - i;
        if (n > group)
            n = group;
        write_io(index, p + i * 512, n * 256);
    }
    return idle(index, false);
}
bool disk_read_blocks(unsigned index, uint32_t lba, unsigned count, void *data) {
    bool legacy = index < 4;
    if (legacy && !enter_channel(index))
        return false;
    PerfArea previous = perf_enter(PERF_STORAGE);
    bool ok = read_blocks(index, lba, count, data);
    perf_leave(previous);
    if (legacy)
        legacy_busy[index / 2] = false;
    return ok;
}
bool disk_write_blocks(unsigned index, uint32_t lba, unsigned count, const void *data) {
    bool legacy = index < 4;
    if (legacy && !enter_channel(index))
        return false;
    PerfArea previous = perf_enter(PERF_STORAGE);
    bool ok = write_blocks(index, lba, count, data);
    perf_leave(previous);
    if (legacy)
        legacy_busy[index / 2] = false;
    return ok;
}
bool disk_read_on(unsigned index, uint32_t lba, void *data) {
    return disk_read_blocks(index, lba, 1, data);
}
bool disk_write_on(unsigned index, uint32_t lba, const void *data) {
    return disk_write_blocks(index, lba, 1, data);
}
static bool flush_disk(unsigned index) {
    if(index>=STORAGE_NATIVE_BASE)
        return storage_flush(index)==0;
    if (index >= 4 || !disks[index].present || !disks[index].supported || disks[index].optical)
        return false;
    select_drive(index, 0xe0);
    outb(port(index) + 7, 0xe7);
    return idle(index, false);
}
bool disk_flush(unsigned index) {
    bool legacy = index < 4;
    if (legacy && !enter_channel(index))
        return false;
    PerfArea previous = perf_enter(PERF_STORAGE);
    bool ok = flush_disk(index);
    perf_leave(previous);
    if (legacy)
        legacy_busy[index / 2] = false;
    return ok;
}
bool disk_read(uint32_t lba, void *data) {
    return disk_read_on(boot_index, lba, data);
}
bool disk_write(uint32_t lba, const void *data) {
    return disk_write_on(boot_index, lba, data) && disk_flush(boot_index);
}
const DiskInfo *disk_info(unsigned index) {
    return index < 4 ? &disks[index] : storage_disk_info(index);
}
unsigned disk_boot_index(void) {
    return boot_index;
}
/* Reconcile BIOS or UEFI boot MBR identity with native block devices after
 * PCI DMA controllers are operational, but BEFORE mounting LunaFS.
 * No match: keep the firmware-provided RAM/ATA fallback. Cloned IDs:
 * reject ambiguous native selection. Never guess the first NVMe drive. */
void disk_resolve_native_boot(void) {
    uint32_t identity=*(volatile uint32_t *)0x6f04;
    if(boot_is_uefi() && ram_source)
        memcpy(&identity,ram_source+440,4);
    if(!identity)return;
    unsigned count=0,candidate=boot_index;
    uint8_t sector[512];
    for(unsigned i=0;i<STORAGE_DEVICE_COUNT;i++) {
        if(i==4)continue;
        const DiskInfo *d=disk_info(i);
        if(!d || !d->present || !d->supported || d->optical || !d->sectors ||
           !disk_read_on(i,0,sector) || sector[510]!=0x55 || sector[511]!=0xaa)continue;
        uint32_t found=0;
        memcpy(&found,sector+440,4);
        if(found!=identity)continue;
        count++;candidate=i;
    }
    if(count==1) {
        boot_index=candidate;
        serial_write("STORAGE: native boot volume matched MBR ID on disk ");
        char n[16];number(n,candidate);
        serial_write(n);serial_write("\n");
    } else if(count>1)
        serial_write("STORAGE: ambiguous boot disk identity; retained safe fallback\n");
}
void disk_init(void) {
    storage_reset();
    for (unsigned i = 0; i < 4; i++) {
        DiskInfo *d = &disks[i];
        d->index = i;
        d->multiple = 1;
        uint16_t base = port(i), id[256];
        select_drive(i, 0xa0);
        outb(base + 0x206, 2);
        uint8_t status = read_status(i);
        if (!status || status == 255)
            continue;
        outb(base + 2, 0);
        outb(base + 3, 0);
        outb(base + 4, 0);
        outb(base + 5, 0);
        outb(base + 7, 0xec);
        if (!idle(i, true)) {
            if (inb(base + 4) != 0x14 || inb(base + 5) != 0xeb)
                continue;
            outb(base + 7, 0xa1);
            if (!idle(i, true))
                continue;
            d->optical = true;
        } else if (inb(base + 4) || inb(base + 5))
            continue;
        read_io(i, id, 256);
        d->present = true;
        d->sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
        if (id[83] & (1u << 10))
            d->sectors = (uint64_t)id[100] | ((uint64_t)id[101] << 16) | ((uint64_t)id[102] << 32) |
                         ((uint64_t)id[103] << 48);
        d->supported =
            !d->optical && (id[49] & (1u << 9)) && d->sectors && d->sectors <= 0x10000000ull;
        if ((id[106] & 0xd000) == 0x5000 && ((uint32_t)id[117] | ((uint32_t)id[118] << 16)) != 256)
            d->supported = false;
        for (unsigned j = 0; j < 20; j++) {
            d->model[j * 2] = id[27 + j] >> 8;
            d->model[j * 2 + 1] = id[27 + j] & 255;
        }
        unsigned maximum = id[47] & 255;
        unsigned multiple = 1;
        while (multiple < 16 && multiple * 2 <= maximum)
            multiple *= 2;
        if (d->supported && multiple > 1) {
            outb(base + 2, multiple);
            outb(base + 7, 0xc6);
            if (idle(i, false))
                d->multiple = multiple;
        }
        d->model[40] = 0;
        for (int j = 39; j >= 0 && d->model[j] == ' '; j--)
            d->model[j] = 0;
    }
    if (boot_is_uefi()) {
        const UefiBootInfo *info = LUNA_UEFI_INFO;
        if (info->ram_image < 0x4000000 || info->ram_bytes != LUNA_IMAGE_BYTES ||
            (uint64_t)info->ram_image + info->ram_bytes > 0x100000000ull) {
            boot_index = 4;
            serial_write("UEFI: invalid storage handoff\n");
            return;
        }
        ram_source = (void *)(uintptr_t)info->ram_image;
        uint32_t source_id;
        memcpy(&source_id, ram_source + 440, 4);
        uint8_t sector[512];
        for (unsigned i = 0; i < 4; i++)
            if (disks[i].present && disks[i].supported && !disks[i].optical &&
                disk_read_on(i, 0, sector)) {
                uint32_t id;
                memcpy(&id, sector + 440, 4);
                if (id == source_id && sector[510] == 0x55 && sector[511] == 0xaa) {
                    boot_index = i;
                    serial_write("UEFI: native writable ATA boot volume\n");
                    return;
                }
            }
        boot_index = 4;
        serial_write("UEFI: firmware-loaded RAM source (USB/virtual/CD), read-only\n");
        return;
    }
    if (*(volatile uint16_t *)0x6f08 == 0xcd01) {
        cd_image_lba = *(volatile uint32_t *)0x6f0c;
        boot_index = 4; /* Fail closed if the boot CD cannot be identified. */
        if (cd_image_lba >= 32 && cd_image_lba < 4096)
            for (unsigned i = 0; i < 4; i++) {
                if (!disks[i].optical || !atapi_read(i, cd_image_lba, 1, cd_cache))
                    continue;
                uint32_t id;
                uint16_t marker;
                memcpy(&id, cd_cache + 440, 4);
                memcpy(&marker, cd_cache + 444, 2);
                if (id == *(volatile uint32_t *)0x6f04 && marker == 0xcd01 &&
                    cd_cache[510] == 0x55 && cd_cache[511] == 0xaa) {
                    boot_index = i;
                    disks[i].supported = true;
                    disks[i].sectors = LUNA_IMAGE_BYTES / 512;
                    cd_cached_device = i;
                    cd_cached_lba = cd_image_lba;
                    break;
                }
            }
        serial_write(boot_index < 4 ? "ATAPI: read-only LunaOS CD installation source\n"
                                    : "ATAPI: boot CD unavailable\n");
        return;
    }
    uint8_t bios = *(volatile uint8_t *)0x6f00;
    unsigned ordinal = bios >= 0x80 ? bios - 0x80 : 0, seen = 0;
    for (unsigned i = 0; i < 4; i++)
        if (disks[i].present) {
            if (seen++ == ordinal)
                boot_index = i;
        }
    uint32_t boot_id = *(volatile uint32_t *)0x6f04;
    unsigned matches = 0, match = boot_index;
    uint8_t sector[512];
    for (unsigned i = 0; i < 4; i++)
        if (disk_read_on(i, 0, sector)) {
            uint32_t id;
            memcpy(&id, sector + 440, 4);
            if (boot_id && id == boot_id && sector[510] == 0x55 && sector[511] == 0xaa) {
                matches++;
                match = i;
            }
        }
    if (matches == 1)
        boot_index = match;
    serial_write("ATA: enumerated IDE targets / BIOS boot identity\n");
}
