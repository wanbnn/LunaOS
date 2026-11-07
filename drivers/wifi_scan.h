#ifndef LUNA_WIFI_SCAN_H
#define LUNA_WIFI_SCAN_H
#include "wifi.h"
/* Native passive scanner. The hardware adapter owns RF initialization, channel
 * policy and RX DMA. Simulated adapters belong to host tests only. */
#define WIFI_SCAN_FRAME_CAP 11520
typedef struct {
    void *context;
    bool (*ready)(void *);
    uint32_t (*now_ms)(void *);
    int (*set_channel)(void *, uint8_t);
    /* 1=frame (FCS removed), 0=no frame, negative=error. */
    int (*receive)(void *, uint8_t *, unsigned, unsigned *, int16_t *);
    int (*stop)(void *);
} WifiScanRadio;
typedef struct {
    WifiScanRadio radio;
    WifiAp aps[WIFI_MAX_APS];
    uint8_t bssid[WIFI_MAX_APS][6];
    uint8_t channels[64], channel_index, channel_count;
    uint32_t deadline;
    unsigned count;
    int error;
    bool scanning, attached;
    uint8_t frame[WIFI_SCAN_FRAME_CAP];
} WifiScanner;
/* Caller supplies channels already allowed by EEPROM + country policy.
 * No active probes, association or fake networks are generated. */
int wifi_scan_attach(WifiScanner *, const WifiScanRadio *, const uint8_t *, unsigned);
void wifi_scan_detach(WifiScanner *);
#endif
