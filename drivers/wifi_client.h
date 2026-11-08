#ifndef LUNA_WIFI_CLIENT_H
#define LUNA_WIFI_CLIENT_H
#include "wifi_station.h"
#include "../include/net/wifi_ccmp.h"
/* Experimental, polling-based non-QoS STA MLME for a real 802.11 radio.
 * All callbacks operate on raw MPDUs with FCS already stripped. No hardware
 * transmit may be reported successful unless DMA accepted the descriptor.
 */
typedef struct {
    void *context;
    bool (*ready)(void *);
    uint32_t (*now)(void *);
    int (*tune)(void *,uint8_t);
    int (*send)(void *,const uint8_t *,unsigned,bool mgmt);
    int (*recv)(void *,uint8_t *,unsigned,unsigned *);
    int (*random)(void *,uint8_t *,size_t);
} WifiClientRadio;
typedef enum {
    WIFI_CLIENT_IDLE, WIFI_CLIENT_AUTH, WIFI_CLIENT_ASSOC, WIFI_CLIENT_PORT
} WifiClientStage;
typedef struct {
    WifiClientRadio radio;
    WifiStation station;
    WifiCcmp ccmp;
    WifiClientStage stage;
    WifiAp ap;
    uint8_t mac[6], rsn[WPA2_RSN_MAX];
    unsigned rsn_len, sequence, aid;
    char password[WIFI_MAX_PASSWORD+1];
    uint32_t deadline;
    uint32_t attempts, mgmt_rx, foreign_rx, auth_rx, assoc_rx;
    uint32_t tx_stalled_since, tx_backpressure;
    int error;
    WifiClientStage failed_stage;
} WifiClient;
int wifi_client_start(WifiClient *,const WifiClientRadio *,const uint8_t mac[6],
                      const WifiAp *,const char *,const uint8_t *rsn,size_t rsn_len);
void wifi_client_poll(WifiClient *);
void wifi_client_disconnect(WifiClient *);
WifiLink wifi_client_link(const WifiClient *);
#endif
