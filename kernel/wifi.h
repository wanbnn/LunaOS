#ifndef LUNA_WIFI_H
#define LUNA_WIFI_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define WIFI_MAX_APS 32
#define WIFI_MAX_SSID 32
#define WIFI_MAX_PASSWORD 63
#define WIFI_PSK_HEX_LEN 64
typedef enum {
    WIFI_DOWN = 0, WIFI_SCANNING = 1, WIFI_IDLE = 2,
    WIFI_ASSOCIATING = 3, WIFI_ASSOCIATED = 4, WIFI_ONLINE = 5
} WifiLink;
typedef enum { WIFI_OPEN = 0, WIFI_WPA2_PSK = 1, WIFI_OTHER = 2 } WifiSecurity;
typedef enum { WIFI_BAND_UNKNOWN = 0, WIFI_BAND_2GHZ = 1, WIFI_BAND_5GHZ = 2 } WifiBand;
/* Frequency mapping only; permission to tune/transmit comes from the backend
 * EEPROM/NVM and country policy, never from this helper. */
unsigned wifi_channel_frequency(uint8_t channel);
WifiBand wifi_channel_band(uint8_t channel);
typedef struct {
    char ssid[WIFI_MAX_SSID + 1];
    uint8_t ssid_len, channel;
    int16_t rssi_dbm;
    WifiSecurity security;
    uint8_t bssid[6]; /* Unicast AP identity, preserved through connection. */
} WifiAp;
typedef struct {
    WifiLink link;
    int error;
    /* A driver must report true only with initialized real RF hardware. */
    bool radio_ready;
    bool can_connect; /* False for a receive-only/passive scan backend. */
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
/* WPA2 accepts a passphrase or a 64-digit hexadecimal PSK. */
int wifi_connect_ap(const WifiAp *ap, const char *password);
int wifi_state(WifiState *out);
void wifi_poll(void);
const char *wifi_connection_ssid(void);
#endif
