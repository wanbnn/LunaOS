#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../drivers/wifi_client.h"
static uint8_t incoming[4][128],sent[4][160];static unsigned rx_len[4],rxhead,rxcount,txcount,txlen[4];
static uint32_t clock_ms;
static bool radio_ready(void *ctx){(void)ctx;return true;}
static uint32_t now(void *ctx){(void)ctx;return clock_ms;}
static int tune(void *ctx,uint8_t ch){(void)ctx;assert(ch==6);return 0;}
static int send_frame(void *ctx,const uint8_t *f,unsigned n,bool mgmt) {
    (void)ctx;assert(mgmt && txcount<4 && n<sizeof(sent[0]));
    memcpy(sent[txcount],f,n);txlen[txcount++]=n;return 0;
}
static int recv_frame(void *ctx,uint8_t *out,unsigned cap,unsigned *n) {
    (void)ctx;if(!rxcount)return 0;
    assert(rx_len[rxhead]<=cap);*n=rx_len[rxhead];
    memcpy(out,incoming[rxhead],*n);rxhead++;rxcount--;return 1;
}
static int rng(void *ctx,uint8_t *out,size_t n){
    (void)ctx;for(size_t i=0;i<n;i++)out[i]=i+1;return 0;
}
static uint8_t sta[6]={2,0,0,0,0,1},ap_mac[6]={0xaa,0xbb,0xcc,0xdd,0xee,0xfe};
static unsigned associations;
int wifi_station_associated(WifiStation *s,const WifiStationRadio *ops,const uint8_t mac[6],
  const WifiAp *ap,const char *password,const uint8_t *rsn,size_t rsn_len,uint32_t t) {
    (void)rsn;(void)rsn_len;(void)t;assert(!memcmp(mac,sta,6));assert(!memcmp(ap->bssid,ap_mac,6));
    assert(!password[0]);associations++;s->radio=*ops;s->link=WIFI_ASSOCIATED;return 0;
}
void wifi_station_disconnect(WifiStation *s){memset(s,0,sizeof(*s));}
void wifi_station_poll(WifiStation *s,uint32_t t){(void)s;(void)t;}
static void reply(unsigned subtype,unsigned status,unsigned seq_or_aid) {
    uint8_t f[32]={0};f[0]=subtype;
    memcpy(f+4,sta,6);memcpy(f+10,ap_mac,6);memcpy(f+16,ap_mac,6);
    if(subtype==0xb0){f[26]=seq_or_aid;f[28]=status;}
    if(subtype==0x10){f[26]=status;f[28]=seq_or_aid;}
    unsigned slot=rxhead+rxcount;memcpy(incoming[slot],f,30);rx_len[slot]=30;rxcount++;
}
int main(void) {
    WifiClient client={0};
    WifiClientRadio r={0,radio_ready,now,tune,send_frame,recv_frame,rng};
    WifiAp ap={.ssid="Home",.ssid_len=4,.channel=6,.security=WIFI_OPEN};
    memcpy(ap.bssid,ap_mac,6);
    assert(!wifi_client_start(&client,&r,sta,&ap,"",NULL,0));
    assert(txcount==1 && txlen[0]==30 && sent[0][0]==0xb0);
    assert(wifi_client_link(&client)==WIFI_ASSOCIATING);
    reply(0xb0,0,2);wifi_client_poll(&client);
    assert(txcount==2 && sent[1][0]==0 && sent[1][28]==0);
    assert(sent[1][30]==0 && sent[1][31]==4 && !memcmp(sent[1]+32,"Home",4));
    reply(0x10,0,42);wifi_client_poll(&client);
    assert(associations==1 && client.aid==42);
    assert(wifi_client_link(&client)==WIFI_ASSOCIATED);
    wifi_client_disconnect(&client);
    assert(wifi_client_link(&client)==WIFI_IDLE && !client.password[0]);
    rxhead=rxcount=0;
    assert(!wifi_client_start(&client,&r,sta,&ap,"",NULL,0));
    clock_ms+=3001;wifi_client_poll(&client);
    assert(client.error==-110 && wifi_client_link(&client)==WIFI_IDLE);
    puts("PASS: native STA auth/association frames, BSSID/AID validation and timeout");
}
