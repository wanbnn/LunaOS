#include "driver.h"
#define CLAIMS 16
#define MAPPINGS 16
#define IRQ_SLOTS 32
static struct {
    bool used;
    uint8_t bus, slot, fn;
    uint16_t command;
    uint32_t cookie;
} claims[CLAIMS];
static struct {
    void *base;
    uint32_t size, cookie, owner;
} mappings[MAPPINGS];
static struct {
    DrvIrqHandler fn;
    void *arg;
    uint32_t cookie, owner;
    uint8_t line;
} irq_slots[IRQ_SLOTS];
static uint32_t next_cookie;
static uint16_t original_elcr, saved_elcr;
static unsigned unhandled[16];
extern void driver_irq_enter(void), driver_irq_leave(void);
static uint32_t cookie(void) {
    if (!++next_cookie)
        ++next_cookie;
    return next_cookie;
}
static bool valid_pci(DrvPci *p) {
    return p && p->slot < 32 && p->function < 8 &&
           pci_read(p->bus, p->slot, p->function, 0) == ((uint32_t)p->device << 16 | p->vendor);
}
static int claim_index(DrvPci *p) {
    if (!p || !p->cookie)
        return -1;
    for (unsigned i = 0; i < CLAIMS; i++)
        if (claims[i].used && claims[i].cookie == p->cookie && claims[i].bus == p->bus &&
            claims[i].slot == p->slot && claims[i].fn == p->function)
            return i;
    return -1;
}
int drv_pci_find(uint16_t vendor, uint16_t device, unsigned index, DrvPci *out) {
    if (!out || vendor == 65535)
        return DRV_EINVAL;
    if (out->cookie)
        return DRV_EBUSY;
    for (unsigned b = 0; b < 256; b++)
        for (unsigned d = 0; d < 32; d++) {
            if ((pci_read(b, d, 0, 0) & 65535) == 65535)
                continue;
            unsigned functions = (pci_read(b, d, 0, 12) & 0x800000) ? 8 : 1;
            for (unsigned f = 0; f < functions; f++) {
                uint32_t id = pci_read(b, d, f, 0);
                if ((id & 65535) != vendor || (id >> 16) != device)
                    continue;
                if (index) {
                    index--;
                    continue;
                }
                uint32_t cl = pci_read(b, d, f, 8), irq = pci_read(b, d, f, 0x3c);
                *out = (DrvPci){.bus = b,
                                .slot = d,
                                .function = f,
                                .revision = cl & 255,
                                .irq = irq & 255,
                                .pin = (irq >> 8) & 255,
                                .vendor = vendor,
                                .device = device,
                                .class_code = cl >> 8,
                                .subsystem = pci_read(b, d, f, 0x2c)};
                return 0;
            }
        }
    return DRV_ENOENT;
}
int drv_pci_claim(DrvPci *p) {
    if (drv_in_irq())
        return DRV_EDEADLK;
    if (!valid_pci(p))
        return DRV_ENOENT;
    uint32_t flags = drv_irq_save();
    for (unsigned i = 0; i < CLAIMS; i++)
        if (claims[i].used && claims[i].bus == p->bus && claims[i].slot == p->slot &&
            claims[i].fn == p->function) {
            drv_irq_restore(flags);
            return DRV_EBUSY;
        }
    uint16_t cmd = pci_read(p->bus, p->slot, p->function, 4);
    /* Never seize an already bus-mastering device from an existing native driver. */
    if (p->cookie || (cmd & 4)) {
        drv_irq_restore(flags);
        return DRV_EBUSY;
    }
    for (unsigned i = 0; i < CLAIMS; i++)
        if (!claims[i].used) {
            claims[i].used = true;
            claims[i].bus = p->bus;
            claims[i].slot = p->slot;
            claims[i].fn = p->function;
            claims[i].command = cmd;
            claims[i].cookie = p->cookie = cookie();
            drv_irq_restore(flags);
            return 0;
        }
    drv_irq_restore(flags);
    return DRV_ENOSPC;
}
int drv_pci_release(DrvPci *p) {
    if (drv_in_irq())
        return DRV_EDEADLK;
    uint32_t flags = drv_irq_save();
    int i = claim_index(p);
    if (i < 0) {
        drv_irq_restore(flags);
        return DRV_EINVAL;
    }
    for (unsigned j = 0; j < MAPPINGS; j++)
        if (mappings[j].cookie && mappings[j].owner == p->cookie) {
            drv_irq_restore(flags);
            return DRV_EBUSY;
        }
    for (unsigned j = 0; j < IRQ_SLOTS; j++)
        if (irq_slots[j].cookie && irq_slots[j].owner == p->cookie) {
            drv_irq_restore(flags);
            return DRV_EBUSY;
        }
    if (pci_read(p->bus, p->slot, p->function, 4) & 4) {
        drv_irq_restore(flags);
        return DRV_EBUSY;
    }
    pci_write(p->bus, p->slot, p->function, 4, claims[i].command);
    claims[i].used = false;
    p->cookie = 0;
    drv_irq_restore(flags);
    return 0;
}
int drv_pci_enable(DrvPci *p, bool io, bool memory) {
    uint32_t flags = drv_irq_save();
    if (claim_index(p) < 0) {
        drv_irq_restore(flags);
        return DRV_EINVAL;
    }
    uint16_t command = pci_read(p->bus, p->slot, p->function, 4);
    command = (command & ~3u) | (io ? 1 : 0) | (memory ? 2 : 0);
    pci_write(p->bus, p->slot, p->function, 4, command);
    bool ok = (pci_read(p->bus, p->slot, p->function, 4) & 3) == (command & 3);
    drv_irq_restore(flags);
    return ok ? 0 : DRV_EIO;
}
int drv_pci_master(DrvPci *p, bool enabled) {
    uint32_t flags = drv_irq_save();
    if (claim_index(p) < 0) {
        drv_irq_restore(flags);
        return DRV_EINVAL;
    }
    uint16_t command = pci_read(p->bus, p->slot, p->function, 4);
    command = enabled ? (command | 4) : (command & ~4u);
    pci_write(p->bus, p->slot, p->function, 4, command);
    bool ok = (pci_read(p->bus, p->slot, p->function, 4) & 4) == (command & 4);
    drv_irq_restore(flags);
    return ok ? 0 : DRV_EIO;
}
int drv_pci_msi_enable(DrvPci *p) {
    return claim_index(p) < 0 ? DRV_EINVAL : DRV_ENOTSUP;
}
int drv_pci_bar(DrvPci *p, unsigned index, DrvBar *out) {
    if (drv_in_irq())
        return DRV_EDEADLK;
    if (!out || index >= 6)
        return DRV_EINVAL;
    uint32_t flags = drv_irq_save();
    if (claim_index(p) < 0) {
        drv_irq_restore(flags);
        return DRV_EINVAL;
    }
    if ((pci_read(p->bus, p->slot, p->function, 12) >> 16) & 127) {
        drv_irq_restore(flags);
        return DRV_ENOTSUP;
    }
    for (unsigned j = 0; j < index; j++) {
        uint32_t prev = pci_read(p->bus, p->slot, p->function, 0x10 + j * 4);
        if (!(prev & 1) && (prev & 6) == 4) {
            if (++j == index) {
                drv_irq_restore(flags);
                return DRV_EINVAL;
            }
        }
    }
    uint16_t cmd = pci_read(p->bus, p->slot, p->function, 4);
    if (cmd & 4) {
        drv_irq_restore(flags);
        return DRV_EBUSY;
    }
    unsigned reg = 0x10 + index * 4;
    uint32_t low = pci_read(p->bus, p->slot, p->function, reg);
    bool io = low & 1, wide = !io && (low & 6) == 4;
    if ((!io && (low & 6) && !wide) || (wide && index == 5)) {
        drv_irq_restore(flags);
        return DRV_ENOTSUP;
    }
    uint32_t high = wide ? pci_read(p->bus, p->slot, p->function, reg + 4) : 0;
    pci_write(p->bus, p->slot, p->function, 4, cmd & ~3u);
    pci_write(p->bus, p->slot, p->function, reg, UINT32_MAX);
    if (wide)
        pci_write(p->bus, p->slot, p->function, reg + 4, UINT32_MAX);
    uint32_t mask = pci_read(p->bus, p->slot, p->function, reg);
    uint32_t upper = wide ? pci_read(p->bus, p->slot, p->function, reg + 4) : UINT32_MAX;
    pci_write(p->bus, p->slot, p->function, reg, low);
    if (wide)
        pci_write(p->bus, p->slot, p->function, reg + 4, high);
    pci_write(p->bus, p->slot, p->function, 4, cmd);
    drv_irq_restore(flags);
    if (high || (wide && upper != UINT32_MAX))
        return DRV_ENOTSUP;
    uint32_t keep = io ? ~3u : ~15u;
    mask &= keep;
    if (!mask || !(low & keep))
        return DRV_ENOENT;
    uint32_t bytes = ~mask + 1;
    if (!bytes || (bytes & (bytes - 1)))
        return DRV_EIO;
    *out = (DrvBar){low & keep, bytes, io, !io && (low & 8)};
    return 0;
}
static bool overlaps_ram(uint32_t address, uint32_t size) {
    uint64_t last = (uint64_t)address + size;
    uint32_t count = *(volatile uint32_t *)0x5000;
    for (unsigned i = 0; i < count && i < 64; i++) {
        const uint8_t *e = (void *)(0x5004 + i * 24);
        uint64_t base, length;
        memcpy(&base, e, 8);
        memcpy(&length, e + 8, 8);
        if (*(const uint32_t *)(e + 16) == 1 && base < last && length <= UINT64_MAX - base &&
            base + length > address)
            return true;
    }
    return false;
}
int drv_pci_map(DrvPci *p, unsigned bar, DrvMmio *out) {
    if (!out || out->cookie || out->base)
        return DRV_EINVAL;
    DrvBar resource;
    int r = drv_pci_bar(p, bar, &resource);
    if (r)
        return r;
    if (resource.io)
        return DRV_ENOTSUP;
    if (resource.address < 0x100000 ||
        (uint64_t)resource.address + resource.size > 0x100000000ull ||
        overlaps_ram(resource.address, resource.size))
        return DRV_EINVAL;
    uint32_t flags = drv_irq_save();
    for (unsigned i = 0; i < MAPPINGS; i++)
        if (!mappings[i].cookie) {
            void *address = 0;
            r = task_mmio_map(resource.address, resource.size, &address);
            if (r) {
                drv_irq_restore(flags);
                return r;
            }
            uint32_t id = cookie();
            mappings[i].base = address;
            mappings[i].size = resource.size;
            mappings[i].cookie = id;
            mappings[i].owner = p->cookie;
            *out = (DrvMmio){address, resource.size, id};
            drv_irq_restore(flags);
            return 0;
        }
    drv_irq_restore(flags);
    return DRV_ENOSPC;
}
/* ACPI resource-backed PIO devices have no PCI function to claim. */
int drv_platform_map(uint32_t physical,uint32_t bytes,DrvMmio *out) {
    if(!out || out->base || out->cookie || physical<0x100000 || !bytes ||
       bytes>0x10000 || (uint64_t)physical+bytes>0x100000000ull ||
       overlaps_ram(physical,bytes))return DRV_EINVAL;
    uint32_t flags=drv_irq_save();
    for(unsigned i=0;i<MAPPINGS;i++)if(!mappings[i].cookie) {
        void *address=0;int rc=task_mmio_map(physical,bytes,&address);
        if(rc){drv_irq_restore(flags);return rc;}
        uint32_t id=cookie();
        mappings[i].base=address;mappings[i].size=bytes;
        mappings[i].cookie=id;mappings[i].owner=0;
        *out=(DrvMmio){address,bytes,id};drv_irq_restore(flags);return 0;
    }
    drv_irq_restore(flags);return DRV_ENOSPC;
}
static int mapping_index(DrvMmio *mem) {
    if (!mem || !mem->cookie)
        return -1;
    for (unsigned i = 0; i < MAPPINGS; i++)
        if (mappings[i].cookie == mem->cookie && mappings[i].base == (void *)mem->base &&
            mappings[i].size == mem->size)
            return i;
    return -1;
}
int drv_mmio_unmap(DrvMmio *mem) {
    if (drv_in_irq())
        return DRV_EDEADLK;
    uint32_t flags = drv_irq_save();
    int i = mapping_index(mem);
    if (i < 0) {
        drv_irq_restore(flags);
        return DRV_EINVAL;
    }
    for (unsigned j = 0; j < CLAIMS; j++)
        if (claims[j].used && claims[j].cookie == mappings[i].owner) {
            if (pci_read(claims[j].bus, claims[j].slot, claims[j].fn, 4) & 4) {
                drv_irq_restore(flags);
                return DRV_EBUSY;
            }
        }
    for (unsigned j = 0; j < IRQ_SLOTS; j++)
        if (irq_slots[j].fn && irq_slots[j].owner == mappings[i].owner) {
            drv_irq_restore(flags);
            return DRV_EBUSY;
        }
    /* Device must also be quiesced by its chipset driver before unmapping. */
    int r = task_mmio_unmap((void *)mem->base, mem->size);
    if (!r) {
        memset(&mappings[i], 0, sizeof(mappings[i]));
        memset(mem, 0, sizeof(*mem));
    }
    drv_irq_restore(flags);
    return r;
}
static int check_mmio(DrvMmio *mem, uint32_t offset, unsigned width) {
    if (mapping_index(mem) < 0 || (width != 1 && width != 2 && width != 4) || mem->size < width ||
        offset > mem->size - width || offset % width)
        return DRV_EINVAL;
    return 0;
}
int drv_mmio_read(DrvMmio *mem, uint32_t off, unsigned width, uint32_t *out) {
    if (!out || check_mmio(mem, off, width))
        return DRV_EINVAL;
    __asm__ volatile("" ::: "memory");
    volatile uint8_t *p = mem->base + off;
    *out = width == 1 ? *p : width == 2 ? *(volatile uint16_t *)p : *(volatile uint32_t *)p;
    drv_memory_barrier();
    return 0;
}
int drv_mmio_write(DrvMmio *mem, uint32_t off, unsigned width, uint32_t value) {
    if (check_mmio(mem, off, width))
        return DRV_EINVAL;
    drv_memory_barrier();
    volatile uint8_t *p = mem->base + off;
    if (width == 1)
        *p = value;
    else if (width == 2)
        *(volatile uint16_t *)p = value;
    else
        *(volatile uint32_t *)p = value;
    __asm__ volatile("" ::: "memory");
    return 0;
}
static bool line_used(unsigned line) {
    for (unsigned i = 0; i < IRQ_SLOTS; i++)
        if (irq_slots[i].fn && irq_slots[i].line == line)
            return true;
    return false;
}
static void pic_line(unsigned line, bool enable) {
    uint16_t port = line < 8 ? 0x21 : 0xa1;
    uint8_t mask = inb(port), bit = 1u << (line % 8);
    outb(port, enable ? (mask & ~bit) : (mask | bit));
    if (line >= 8) {
        bool slave = false;
        for (unsigned i = 8; i < 16; i++)
            slave |= line_used(i);
        mask = inb(0x21);
        outb(0x21, slave ? (mask & ~4u) : (mask | 4));
    }
}
int drv_irq_request(DrvPci *p, DrvIrqHandler fn, void *arg, DrvIrq *out) {
    if (drv_in_irq())
        return DRV_EDEADLK;
    if (!fn || !out || out->cookie)
        return DRV_EINVAL;
    uint32_t flags = drv_irq_save();
    if (claim_index(p) < 0) {
        drv_irq_restore(flags);
        return DRV_EINVAL;
    }
    if (p->irq < 3 || p->irq > 15 || p->irq == 8 || p->irq == 13 || p->irq >= 14 || !p->pin ||
        p->pin > 4) {
        drv_irq_restore(flags);
        return DRV_ENOTSUP;
    }
    for (unsigned i = 0; i < IRQ_SLOTS; i++)
        if (!irq_slots[i].fn) {
            irq_slots[i].fn = fn;
            irq_slots[i].arg = arg;
            irq_slots[i].owner = p->cookie;
            irq_slots[i].line = p->irq;
            irq_slots[i].cookie = out->cookie = cookie();
            /* PCI INTx is level-triggered. ELCR is provided by the supported PC chipset. */
            uint16_t port = p->irq < 8 ? 0x4d0 : 0x4d1;
            uint8_t bit = 1u << (p->irq % 8);
            if (!(saved_elcr & (1u << p->irq))) {
                if (inb(port) & bit)
                    original_elcr |= 1u << p->irq;
                saved_elcr |= 1u << p->irq;
            }
            outb(port, inb(port) | bit);
            unhandled[p->irq] = 0;
            uint16_t cmd = pci_read(p->bus, p->slot, p->function, 4);
            pci_write(p->bus, p->slot, p->function, 4, cmd & ~0x400u);
            pic_line(p->irq, true);
            drv_irq_restore(flags);
            return 0;
        }
    drv_irq_restore(flags);
    return DRV_ENOSPC;
}
int drv_irq_free(DrvIrq *irq) {
    if (!irq || !irq->cookie || drv_in_irq())
        return DRV_EINVAL;
    uint32_t flags = drv_irq_save();
    for (unsigned i = 0; i < IRQ_SLOTS; i++)
        if (irq_slots[i].cookie == irq->cookie) {
            unsigned line = irq_slots[i].line;
            uint32_t owner = irq_slots[i].owner;
            memset(&irq_slots[i], 0, sizeof(irq_slots[i]));
            irq->cookie = 0;
            bool other = false;
            for (unsigned j = 0; j < IRQ_SLOTS; j++)
                other |= irq_slots[j].fn && irq_slots[j].owner == owner;
            if (!other)
                for (unsigned j = 0; j < CLAIMS; j++)
                    if (claims[j].used && claims[j].cookie == owner) {
                        uint16_t cmd = pci_read(claims[j].bus, claims[j].slot, claims[j].fn, 4);
                        pci_write(claims[j].bus, claims[j].slot, claims[j].fn, 4, cmd | 0x400u);
                    }
            if (!line_used(line)) {
                pic_line(line, false);
                uint16_t port = line < 8 ? 0x4d0 : 0x4d1;
                uint8_t bit = 1u << (line % 8);
                outb(port, (inb(port) & ~bit) | ((original_elcr & (1u << line)) ? bit : 0));
                original_elcr &= ~(1u << line);
                saved_elcr &= ~(1u << line);
            }
            drv_irq_restore(flags);
            return 0;
        }
    drv_irq_restore(flags);
    return DRV_EINVAL;
}
void driver_irq_dispatch(unsigned line) {
    if (line >= 16)
        return;
    if (line == 7 || line == 15) {
        uint16_t port = line == 7 ? 0x20 : 0xa0;
        outb(port, 0x0b);
        if (!(inb(port) & 0x80)) {
            if (line == 15)
                outb(0x20, 0x20);
            return;
        }
    }
    driver_irq_enter();
    bool handled = false;
    for (unsigned i = 0; i < IRQ_SLOTS; i++)
        if (irq_slots[i].fn && irq_slots[i].line == line)
            handled |= irq_slots[i].fn(irq_slots[i].arg);
    if (handled)
        unhandled[line] = 0;
    else if (++unhandled[line] >= 32)
        pic_line(line, false);
    if (line >= 8)
        outb(0xa0, 0x20);
    outb(0x20, 0x20);
    driver_irq_leave();
}

int drv_pci_config_read(DrvPci *p, unsigned off, unsigned width, uint32_t *out) {
    if (!out || off >= 256 || (width != 1 && width != 2 && width != 4) || off % width ||
        width > 256 - off)
        return DRV_EINVAL;
    if (!valid_pci(p))
        return DRV_ENOENT;
    uint32_t raw = pci_read(p->bus, p->slot, p->function, off);
    *out = raw >> ((off & 3) * 8);
    if (width < 4)
        *out &= (1u << (width * 8)) - 1;
    return 0;
}
int drv_pci_config_write(DrvPci *p, unsigned off, unsigned width, uint32_t value) {
    if (off >= 256 || (width != 1 && width != 2 && width != 4) || off % width || width > 256 - off)
        return DRV_EINVAL;
    uint32_t flags = drv_irq_save();
    if (claim_index(p) < 0) {
        drv_irq_restore(flags);
        return DRV_EINVAL;
    }
    outl(0xcf8, 0x80000000u | ((uint32_t)p->bus << 16) | (p->slot << 11) | (p->function << 8) |
                    (off & ~3u));
    /* Subword writes must not acknowledge adjacent PCI status W1C bits. */
    if (width == 1)
        outb(0xcfc + (off & 3), value);
    else if (width == 2)
        outw(0xcfc + (off & 3), value);
    else
        outl(0xcfc, value);
    drv_irq_restore(flags);
    return 0;
}
