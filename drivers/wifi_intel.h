#ifndef LUNA_WIFI_INTEL_H
#define LUNA_WIFI_INTEL_H
#include "driver.h"
/* Read-only inventory. A PCI match is not an initialized radio. */
typedef struct {
    DrvPci pci;
    bool ax211_machine_match;
} WifiIntelProbe;
int wifi_intel_probe(unsigned index, WifiIntelProbe *out);
void wifi_intel_status(char *out, size_t cap);
#endif
