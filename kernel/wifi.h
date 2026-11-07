#ifndef LUNA_WIFI_H
#define LUNA_WIFI_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define WIFI_MAX_APS 8
#define WIFI_MAX_SSID 32
#define WIFI_MAX_PASSWORD 63
typedef enum {
    WIFI_DOWN = 0, WIFI_SCANNING = 1, WIFI_IDLE = 2,
    WIFI_ASSOCIATING = 3, WIFI_ASSOCIATED = 4, WIFI_ONLINE = 5
} WifiLink;
typedef enum { WIFI_OPEN = 0, WIFI_WPA2_PSK = 1, WIFI_OTHER = 2 } WifiSecurity;
typedef struct {
    char ssid[WIFI_MAX_SSID + 1];
    uint8_t ssid_len, channel;
    int16_t rssi_dbm;
    WifiSecurity security;
} WifiAp;
typedef struct {
    WifiLink link;
    int error;
    /* A driver must report true only with initialized real RF hardware. */
    bool radio_ready;
} WifiState;
typedef struct {
    int (*scan_start)(void *ctx);
    int (*scan_results)(void *ctx, WifiAp *out, unsigned cap, unsigned *count);
    int (*connect)(void *ctx, const WifiAp *ap, const char *password);
    int (*state)(void *ctx, WifiState *out);
    void (*poll)(void *ctx);
} WifiDriverOps;
int wifi_register_driver(const WifiDriverOps *ops, void *context);
void wifi_unregister_driver(const WifiDriverOps *ops);
int wifi_scan_start(void);
int wifi_scan_results(WifiAp *out, unsigned cap, unsigned *count);
int wifi_connect_ap(const WifiAp *ap, const char *password);
int wifi_state(WifiState *out);
void wifi_poll(void);
#endif
