#ifndef LUNA_IWL_PCIE_H
#define LUNA_IWL_PCIE_H
#include "wifi_intel.h"
/* Transport preparation diagnostics. Hardware boot must never be inferred
 * from a structurally valid file. Radio remains unregistered until ALIVE,
 * NVM, regulatory and data queues have been implemented and verified. */
typedef struct {
    uint32_t hw_rev, rf_id, gp_control;
    uint32_t firmware_bytes, paging_bytes;
    unsigned runtime_sections;
    const char *firmware_name, *stage;
    int result;
    bool present, firmware_valid;
} IwlPcieState;
int iwl_pcie_prepare(const WifiIntelProbe *, IwlPcieState *);
void wifi_intel_init(void);
int wifi_intel_diag(char *, size_t);
#endif
