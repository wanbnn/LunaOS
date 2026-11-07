#include "wifi_station.h"
#include <string.h>
static bool unicast(const uint8_t mac[6]) {
    if(mac[0]&1) return false;
    unsigned bits=0;
    for(unsigned i=0;i<6;i++) bits|=mac[i];
    return bits!=0;
}
static bool authorized(const WifiStation *s) {
    return s->link>=WIFI_ASSOCIATED && !s->error && s->radio.ready(s->radio.context) &&
        (s->security==WIFI_OPEN || s->wpa.phase==WPA2_AUTHORIZED);
}
static bool nic_up(void *ctx) { return authorized(ctx); }
static int nic_send(void *ctx,const uint8_t *p,unsigned n) {
    WifiStation *s=ctx;
    if(!authorized(s)) return -13;
    return s->radio.transmit(s->radio.context,p,n,s->security==WIFI_WPA2_PSK);
}
static int nic_receive(void *ctx,uint8_t *out,unsigned cap,unsigned *n) {
    WifiStation *s=ctx; *n=0;
    if(!authorized(s)) return -100;
    if(!s->count) return 0;
    if(s->lengths[s->head]>cap) return -28;
    *n=s->lengths[s->head]; memcpy(out,s->queued[s->head],*n);
    s->head=(s->head+1)%8; s->count--; return 1;
}
static const NetDeviceOps nic={nic_up,nic_send,nic_receive};
static int rng(void *ctx,uint8_t *p,size_t n) {
    WifiStation *s=ctx; return s->radio.random(s->radio.context,p,n);
}
static int eapol_send(void *ctx,const uint8_t *p,size_t n) {
    WifiStation *s=ctx;
    if(n>WPA2_EAPOL_MAX) return -22;
    uint8_t frame[14+WPA2_EAPOL_MAX];
    memcpy(frame,s->bssid,6); memcpy(frame+6,s->mac,6); frame[12]=0x88; frame[13]=0x8e;
    memcpy(frame+14,p,n);
    /* Initial handshake EAPOL is sent before the controlled port opens. */
    return s->radio.transmit(s->radio.context,frame,n+14,false);
}
static int install(void *ctx,const Wpa2Keys *keys) {
    WifiStation *s=ctx; return s->radio.install(s->radio.context,keys);
}
static void clear(void *ctx) {
    WifiStation *s=ctx; s->radio.clear(s->radio.context);
    /* Closing the controlled port also invalidates DHCP and all queued IP. */
    if(s->attached) net_detach_driver(&nic,s);
    s->attached=false; s->head=s->count=0;
}
void wifi_station_disconnect(WifiStation *s) {
    if(!s) return;
    if(s->attached) net_detach_driver(&nic,s);
    s->attached=false;
    if(s->wpa.phase!=WPA2_OFF) wpa2_stop(&s->wpa);
    else if(s->radio.clear) s->radio.clear(s->radio.context);
    memset(s,0,sizeof(*s));
}
int wifi_station_associated(WifiStation *s,const WifiStationRadio *r,const uint8_t mac[6],
                             const WifiAp *ap,const char *pass,const uint8_t *rsn,
                             size_t rsn_len,uint32_t now) {
    if(!s || !r || !r->ready || !r->random || !r->transmit || !r->receive ||
       !r->install || !r->clear || !mac || !ap || !unicast(mac) || !unicast(ap->bssid) ||
       !ap->ssid_len || ap->ssid_len>WIFI_MAX_SSID) return -22;
    if(s->link!=WIFI_DOWN) return -16;
    if(ap->security!=WIFI_OPEN && ap->security!=WIFI_WPA2_PSK) return -95;
    if(!r->ready(r->context)) return -100;
    memset(s,0,sizeof(*s)); s->radio=*r; memcpy(s->mac,mac,6);
    memcpy(s->bssid,ap->bssid,6); s->security=ap->security;
    s->link=WIFI_ASSOCIATED; s->now=now;
    if(ap->security==WIFI_OPEN) { s->radio.clear(s->radio.context); return 0; }
    const Wpa2Ops ops={s,rng,eapol_send,install,clear};
    int rc=wpa2_start(&s->wpa,&ops,mac,ap->bssid,(const uint8_t *)ap->ssid,
                       ap->ssid_len,pass,rsn,rsn_len,now);
    if(rc) { s->error=rc; s->link=WIFI_DOWN; s->radio.clear(s->radio.context); }
    return rc;
}
void wifi_station_poll(WifiStation *s,uint32_t now) {
    if(!s || s->link==WIFI_DOWN) return;
    s->now=now;
    if(!s->radio.ready(s->radio.context)) {
        clear(s); wpa2_stop(&s->wpa); s->error=-100; s->link=WIFI_DOWN; return;
    }
    uint8_t frame[1514]; const uint8_t broadcast[6]={255,255,255,255,255,255};
    for(unsigned budget=0;budget<16;budget++) {
        unsigned bytes=0; bool encrypted=false;
        int rc=s->radio.receive(s->radio.context,frame,sizeof(frame),&bytes,&encrypted);
        if(!rc) break;
        if(rc<0 || rc!=1 || bytes>sizeof(frame)) {
            clear(s); wpa2_stop(&s->wpa); s->error=rc<0 ? rc : -22; s->link=WIFI_DOWN; return;
        }
        if(bytes<14 || (memcmp(frame,s->mac,6) && memcmp(frame,broadcast,6))) continue;
        if(frame[12]==0x88 && frame[13]==0x8e && s->security==WIFI_WPA2_PSK) {
            /* Rekey is unsupported: close the port and require a fresh
             * association rather than continuing with expired keys. */
            rc=wpa2_receive(&s->wpa,frame+6,frame+14,bytes-14,now);
            if(s->wpa.phase==WPA2_FAILED || (rc==-95 && s->wpa.phase==WPA2_AUTHORIZED)) {
                clear(s); wpa2_stop(&s->wpa); s->error=rc; s->link=WIFI_DOWN; return;
            }
            continue;
        }
        if(!authorized(s) || (s->security==WIFI_WPA2_PSK && !encrypted) || s->count==8) continue;
        unsigned tail=(s->head+s->count)%8;
        memcpy(s->queued[tail],frame,bytes); s->lengths[tail]=bytes; s->count++;
    }
    if(s->security==WIFI_WPA2_PSK) {
        wpa2_poll(&s->wpa,now);
        if(s->wpa.phase==WPA2_FAILED) { s->error=s->wpa.error; s->link=WIFI_DOWN; return; }
    }
    if(authorized(s) && !s->attached) {
        uint32_t xid=0; int rc=rng(s,(uint8_t *)&xid,sizeof(xid));
        if(!rc && !xid) rc=-5;
        if(!rc) rc=net_attach_driver(&nic,s,s->mac,xid);
        if(rc) { clear(s); wpa2_stop(&s->wpa); s->error=rc; s->link=WIFI_DOWN; return; }
        s->attached=true;
    }
    if(s->attached) s->link=net_driver_ipv4(&nic,s,0) ? WIFI_ONLINE : WIFI_ASSOCIATED;
}
