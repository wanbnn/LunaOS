#ifndef LUNA_PMM_H
#define LUNA_PMM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define PMM_PAGE 4096u
#define PMM_MAP_MAX 128u
#define PMM_PHYSICAL_END UINT64_C(0x100000000)
/* BIOS E820 / translated UEFI descriptor. Disabled entries are ignored. */
typedef struct { uint64_t base, length; uint32_t type, attributes; } __attribute__((packed)) PmmRange;
typedef struct { uint64_t base, length; } PmmReservation;
typedef struct { uint32_t address, bytes, cookie; } PmmAllocation;
typedef struct {
    uint8_t *free_bits;
    uint32_t *sizes, *cookies;
    uint32_t page_count, free_pages, generation, allocation_count;
    uint64_t firmware_usable, addressable_usable, above_4g_usable;
    bool ready;
} Pmm;
/* Metadata scales with the physical address span, bounded by the i386
 * non-PAE architectural address space, never by a selected RAM pool size. */
size_t pmm_metadata_bytes(const PmmRange *, unsigned);
int pmm_metadata_region(const PmmRange *, unsigned, const PmmReservation *, unsigned,
                         size_t bytes, uint32_t *address);
int pmm_init(Pmm *, const PmmRange *, unsigned, const PmmReservation *, unsigned,
               void *metadata, size_t metadata_bytes);
int pmm_allocate(Pmm *, uint32_t bytes, uint32_t align, uint32_t mask,
                  uint32_t boundary, bool prefer_high, PmmAllocation *);
int pmm_release(Pmm *, const PmmAllocation *);
int pmm_lookup(const Pmm *, uint32_t address, PmmAllocation *);
#endif
