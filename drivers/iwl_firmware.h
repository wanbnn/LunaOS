#ifndef LUNA_IWL_FIRMWARE_H
#define LUNA_IWL_FIRMWARE_H
#include <stddef.h>
#include <stdint.h>
#define IWL_FW_SECTIONS 128
/* Bounded structural parser, not proof of firmware/hardware compatibility.
 * All views point into caller-owned bytes; keep them alive until release. */
typedef struct { uint32_t type, offset, size; const uint8_t *data; } IwlFwSection;
typedef struct {
    uint32_t version, build, version_triplet[3], cpu_count, paging_bytes, flags;
    IwlFwSection sections[IWL_FW_SECTIONS];
    unsigned section_count, runtime_count;
    const uint8_t *command_versions;
    uint32_t command_versions_bytes;
} IwlFirmware;
int iwl_firmware_parse(const uint8_t *, size_t, IwlFirmware *);
/* Restrict the initial hardware configuration to SO A + GF A. The raw CSR
 * values are required, not PCI IDs. Unknown steppings must fail closed. */
const char *iwl_firmware_candidate(uint32_t raw_hw_rev, uint32_t raw_rf_id);
#endif
