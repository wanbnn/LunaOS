/* Radio/RX simulation for the passive scanner; never linked into LunaOS. */
#include "../drivers/wifi_scan.h"
#include "../drivers/rtw8822b_native.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t clock_ms;
static unsigned channel, emitted, stops;
static bool ready=true;
static bool radio_ready(void *ctx) { (void)ctx; return ready; }
static uint32_t now(void *ctx) { (void)ctx; return clock_ms; }
static int tune(void *ctx,uint8_t ch) { (void)ctx; channel=ch; emitted=0; return 0; }
static int stop(void *ctx) { (void)ctx; stops++; return 0; }
static int receive(void *ctx,uint8_t *out,unsigned cap,unsigned *bytes,int16_t *rssi) {
    (void)ctx;
    if(emitted++>1) return 0; /* include a duplicate beacon */
    uint8_t dma[128]={0}; uint8_t *f=dma+24;
    f[0]=0x80; memset(f+4,255,6); f[10]=f[16]=2; f[15]=f[21]=channel;
    f[34]=channel==1 ? 16 : 0;
    unsigned p=36;
    f[p++]=0; f[p++]=4; memcpy(f+p,channel==1 ? "test" : "five",4); p+=4;
    f[p++]=3; f[p++]=1; f[p++]=channel;
    if(channel==1) {
        static const uint8_t rsn[]={48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0};
        memcpy(f+p,rsn,sizeof(rsn)); p+=sizeof(rsn);
    }
    unsigned packet_bytes=p+4; dma[0]=packet_bytes; dma[1]=packet_bytes>>8;
    RtwRxPacket packet;
    assert(!rtw8822b_rx_parse(dma,24+packet_bytes,&packet));
    assert(packet.frame_bytes<=cap);
    memcpy(out,packet.frame,packet.frame_bytes); *bytes=packet.frame_bytes;
    *rssi=-40; return 1;
}
int main(void) {
    static WifiScanner scanner;
    const uint8_t channels[]={1,36};
    const WifiScanRadio radio={.ready=radio_ready,.now_ms=now,.set_channel=tune,
        .receive=receive,.stop=stop};
    assert(!wifi_scan_attach(&scanner,&radio,channels,2));
    assert(!wifi_scan_start()); assert(wifi_scan_start()==-16);
    wifi_poll();
    WifiAp aps[WIFI_MAX_APS]; unsigned count;
    assert(!wifi_scan_results(aps,8,&count) && count==1);
    assert(!strcmp(aps[0].ssid,"test") && aps[0].security==WIFI_WPA2_PSK);
    assert(aps[0].bssid[0]==2 && aps[0].bssid[5]==1);
    assert(wifi_channel_band(aps[0].channel)==WIFI_BAND_2GHZ);
    clock_ms=220; wifi_poll(); assert(channel==36); wifi_poll();
    assert(!wifi_scan_results(aps,8,&count) && count==2);
    assert(!strcmp(aps[1].ssid,"five") && aps[1].security==WIFI_OPEN);
    assert(aps[1].bssid[5]==36);
    assert(wifi_channel_band(aps[1].channel)==WIFI_BAND_5GHZ);
    clock_ms=440; wifi_poll();
    WifiState state; assert(!wifi_state(&state) && state.link==WIFI_IDLE && stops==1);
    assert(wifi_connect_ap(aps,"12345678")==-95); /* scan is not association */
    assert(!wifi_scan_start()); ready=false; wifi_poll();
    assert(wifi_scan_results(aps,8,&count)==-95 && !count);
    wifi_scan_detach(&scanner);
    assert(wifi_scan_start()==-95);
    puts("PASS: simulated RTL8822B RX -> real scanner -> public setup API; 2.4/5GHz, RSN, dedup, completion, radio loss");
}
