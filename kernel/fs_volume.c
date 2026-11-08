#include "fs_volume.h"

#define LEGACY_START 8192u
#define LEGACY_SLOT 4096u
#define SYSTEM_END (LUNA_IMAGE_BYTES / 512u)

static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void write32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; i++)
        p[i] = value >> (8 * i);
}

size_t fs_volume_bytes(const FsVolume *volume) {
    uint64_t bytes = ((uint64_t)volume->slot_sectors - 1u) * 512u;
    return bytes > SIZE_MAX ? SIZE_MAX : (size_t)bytes;
}

bool fs_volume(unsigned disk, bool create, FsVolume *out) {
    if (!out)
        return false;
    *out = (FsVolume){LEGACY_START, LEGACY_SLOT, false, true};
    const DiskInfo *info = disk_info(disk);
    if (!info || !info->present || !info->supported || info->optical ||
        info->sectors <= SYSTEM_END || info->sectors > UINT32_MAX)
        return true;

    uint8_t mbr[512], image[512];
    if (!disk_read_on(disk, 0, mbr) || !disk_read_on(disk, LUNA_IMAGE_INFO_SECTOR, image))
        return false;
    /* Only claim free space on an identified LunaOS system disk. */
    if (mbr[510] != 0x55 || mbr[511] != 0xaa || memcmp(image, "LUNAIMG1", 8) ||
        read32(image + 8) != 1 || read32(image + 12) != LUNA_IMAGE_BYTES || mbr[450] != 0xda ||
        read32(mbr + 454) != LEGACY_START)
        return true;

    uint32_t starts[4], ends[4];
    unsigned occupied = 0;
    int empty = -1;
    FsVolume existing = {0};
    for (unsigned i = 0; i < 4; i++) {
        uint8_t *entry = mbr + 446 + i * 16;
        uint32_t start = read32(entry + 8), count = read32(entry + 12);
        if (!entry[4] && !count) {
            if (empty < 0)
                empty = (int)i;
            continue;
        }
        uint64_t end = (uint64_t)start + count;
        if (!count || end > info->sectors || entry[4] == 0xee)
            return true;
        if (entry[4] == 0xda && !memcmp(entry + 1, "LFS", 3) && start >= SYSTEM_END && count >= 4) {
            if (existing.exists)
                return true;
            existing = (FsVolume){start, count / 2u, true, true};
        }
        unsigned pos = occupied++;
        while (pos && starts[pos - 1] > start) {
            starts[pos] = starts[pos - 1];
            ends[pos] = ends[pos - 1];
            pos--;
        }
        starts[pos] = start;
        ends[pos] = (uint32_t)end;
    }
    for (unsigned i = 1; i < occupied; i++)
        if (starts[i] < ends[i - 1])
            return true;
    if (existing.exists) {
        *out = existing;
        return true;
    }
    if (empty < 0)
        return true;

    uint32_t cursor = SYSTEM_END, best_start = 0, best_size = 0;
    for (unsigned i = 0; i <= occupied; i++) {
        uint32_t end = i == occupied ? (uint32_t)info->sectors : starts[i];
        if (end > cursor && end - cursor > best_size) {
            best_start = cursor;
            best_size = end - cursor;
        }
        if (i < occupied && ends[i] > cursor)
            cursor = ends[i];
    }
    if (best_size < LEGACY_SLOT * 2u)
        return true;
    *out = (FsVolume){best_start, best_size / 2u, true, false};
    if (!create)
        return true;

    uint8_t zero[512] = {0};
    if (!disk_write_on(disk, best_start, zero) ||
        !disk_write_on(disk, best_start + out->slot_sectors, zero) || !disk_flush(disk))
        return false;
    uint8_t *entry = mbr + 446 + (unsigned)empty * 16;
    memset(entry, 0, 16);
    memcpy(entry + 1, "LFS", 3);
    entry[4] = 0xda;
    write32(entry + 8, best_start);
    write32(entry + 12, out->slot_sectors * 2u);
    if (!disk_write_on(disk, 0, mbr) || !disk_flush(disk))
        return false;
    out->exists = true;
    return true;
}
