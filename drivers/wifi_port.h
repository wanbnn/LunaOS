#ifndef LUNA_WIFI_PORT_H
#define LUNA_WIFI_PORT_H
#include "driver.h"
/* Candidate PCIe ports. This API does NOT claim a functional radio driver. */
typedef enum { WIFI_RTLWIFI, WIFI_RTW88, WIFI_MT76, WIFI_ATH10K } WifiFamily;
typedef struct {
    uint16_t vendor, device;
    const char *name, *family_name;
    WifiFamily family;
    const char *firmware, *firmware_aux;
} WifiPortTarget;
typedef struct {
    const WifiPortTarget *target;
    DrvPci pci;
    unsigned mmio_bar;
    uint32_t mmio_size;
    int pci_result, mmio_result, firmware_result, firmware_aux_result;
    bool pci_present, firmware_ready, mmio_ready, driver_loaded;
} WifiPortProbe;
unsigned wifi_port_target_count(void);
const WifiPortTarget *wifi_port_target(unsigned index);
const WifiPortTarget *wifi_port_match(uint16_t vendor, uint16_t device);
/* Passive preflight, not firmware upload or hardware initialization.
 * Returns DRV_OK on successful *preflight* only. Claims/buffers are released. */
int wifi_port_preflight(const WifiPortTarget *, unsigned instance, WifiPortProbe *);
#endif
