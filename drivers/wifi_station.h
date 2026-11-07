#ifndef LUNA_WIFI_STATION_H
#define LUNA_WIFI_STATION_H
#include "wifi.h"
#include "netif.h"
#include "../include/net/wpa2.h"
/* Shared post-association data plane. A hardware MLME backend must complete
 * auth/association, validate RSN and provide the station MAC from NVM first.
 * Hardware callbacks exchange Ethernet MSDUs and offload CCMP. Receive must
 * reject failed decrypt/MIC/replay, wrong BSSID and unauthenticated sources.
 * encrypted=true attests that the MSDU passed those checks. */
typedef struct {
    void *context;
    bool (*ready)(void *);
    int (*random)(void *,uint8_t *,size_t);
    int (*transmit)(void *,const uint8_t *,unsigned,bool encrypt);
    int (*receive)(void *,uint8_t *,unsigned,unsigned *,bool *encrypted);
    int (*install)(void *,const Wpa2Keys *);
    void (*clear)(void *);
} WifiStationRadio;
typedef struct {
    WifiStationRadio radio;
    Wpa2Supplicant wpa;
    WifiLink link;
    WifiSecurity security;
    uint8_t mac[6], bssid[6];
    bool attached;
    int error;
    uint32_t now;
    uint8_t queued[8][1514];
    unsigned lengths[8], head, count;
} WifiStation;
/* Context must be zero-initialized. This does not perform association. */
int wifi_station_associated(WifiStation *,const WifiStationRadio *,const uint8_t mac[6],
                             const WifiAp *,const char *password,const uint8_t *rsn,
                             size_t rsn_len,uint32_t now);
void wifi_station_poll(WifiStation *,uint32_t now);
void wifi_station_disconnect(WifiStation *);
#endif
