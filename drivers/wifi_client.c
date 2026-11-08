#include "wifi_client.h"
#include "../include/net/luna80211.h"
#include <string.h>
#define INVALID (-22)
#define ACCESS (-13)
#define BUSY (-16)
#define IO (-5)
#define TIMEOUT (-110)
#define UNSUPPORTED (-95)
static void wipe(void *p,size_t n) { volatile uint8_t *v=p;while(n--) *v++=0; }
/* Avoid host _FORTIFY_SOURCE memcpy_chk references in the freestanding kernel. */
static void copy(void *dst,const void *src,size_t n) {
    volatile uint8_t *d=dst;
    const uint8_t *s=src;
    for(size_t i=0;i<n;i++) d[i]=s[i];
}
static bool unicast(const uint8_t *m) {
    if(m[0]&1) return false;
    unsigned n=0;for(unsigned i=0;i<6;i++) n|=m[i];return n!=0;
}
static unsigned u16(const uint8_t *p) {return p[0]|((unsigned)p[1]<<8);}
static void put16(uint8_t *p,unsigned n) {p[0]=n;p[1]=n>>8;}
static bool ready(void *v) {
    WifiClient *c=v;return c->radio.ready(c->radio.context);
}
static int rng(void *v,uint8_t *out,size_t n) {
    WifiClient *c=v;return c->radio.random(c->radio.context,out,n);
}
static int emit(WifiClient *c,uint8_t *frame,unsigned len,bool mgmt) {
    /* Legacy non-QoS sequence is supplied by the host, not firmware. */
    put16(frame+22,((c->sequence++ & 4095u)<<4));
    return c->radio.send(c->radio.context,frame,len,mgmt);
}
static void hdr(WifiClient *c,uint8_t *f,uint8_t subtype) {
    memset(f,0,24);f[0]=subtype;
    copy(f+4,c->ap.bssid,6);
    copy(f+10,c->mac,6);
    copy(f+16,c->ap.bssid,6);
}
static int auth(WifiClient *c) {
    uint8_t f[30];hdr(c,f,0xb0);
    put16(f+24,0);put16(f+26,1);put16(f+28,0);
    c->stage=WIFI_CLIENT_AUTH;
    int rc=emit(c,f,sizeof(f),true);
    if(!rc) { c->attempts++; c->tx_stalled_since=0; }
    if(!rc) c->deadline=c->radio.now(c->radio.context)+3000;
    return rc;
}
static int assoc(WifiClient *c) {
    uint8_t f[160];hdr(c,f,0x00);
    unsigned pos=24;
    put16(f+pos,1|(c->ap.security==WIFI_WPA2_PSK?0x10:0));pos+=2;
    put16(f+pos,10);pos+=2;
    f[pos++]=0;f[pos++]=c->ap.ssid_len;
    copy(f+pos,c->ap.ssid,c->ap.ssid_len);pos+=c->ap.ssid_len;
    static const uint8_t r2[]={0x82,0x84,0x8b,0x96,0x0c,0x12,0x18,0x24};
    static const uint8_t r5[]={0x8c,0x98,0xb0,0x12,0x24,0x48};
    bool is5=c->ap.channel>14;
    const uint8_t *rates=is5?r5:r2;
    unsigned n=is5?sizeof(r5):sizeof(r2);
    f[pos++]=1;f[pos++]=n;copy(f+pos,rates,n);pos+=n;
    if(c->ap.security==WIFI_WPA2_PSK) {
        static const uint8_t selected[]={48,20,1,0,0,15,172,4,1,0,0,15,172,4,
                                         1,0,0,15,172,2,0,0};
        copy(f+pos,selected,sizeof(selected));pos+=sizeof(selected);
    }
    c->stage=WIFI_CLIENT_ASSOC;
    int rc=emit(c,f,pos,true);
    if(!rc) { c->attempts++; c->tx_stalled_since=0; }
    if(!rc) c->deadline=c->radio.now(c->radio.context)+3000;
    return rc;
}
static void fail(WifiClient *c,int error) {
    wifi_station_disconnect(&c->station);
    wifi_ccmp_clear(&c->ccmp);
    wipe(c->password,sizeof(c->password));
    c->failed_stage=c->stage;
    c->stage=WIFI_CLIENT_IDLE;c->error=error;
}
static int tx_msdu(void *v,const uint8_t *ethernet,unsigned size,bool encrypt) {
    WifiClient *c=v;
    if(!ready(c) || !ethernet || size<14 || size>1514) return INVALID;
    if(memcmp(ethernet+6,c->mac,6)) return ACCESS;
    uint8_t f[1600];size_t n=0;
    int rc=luna80211_from_ethernet(ethernet,size,c->ap.bssid,c->sequence&4095u,
                                   f,sizeof(f),&n);
    if(rc) return rc;
    c->sequence++;
    if(encrypt) {
        rc=wifi_ccmp_encrypt(&c->ccmp,f,&n,sizeof(f));
        if(rc) return rc;
    }
    return c->radio.send(c->radio.context,f,n,false);
}
static int rx_msdu(void *v,uint8_t *out,unsigned cap,unsigned *bytes,bool *encrypted) {
    WifiClient *c=v;
    *bytes=0;*encrypted=false;
    /* Poll up to eight raw MPDUs to avoid starvation on unrelated beacons. */
    for(unsigned attempt=0;attempt<8;attempt++) {
        uint8_t f[1600];unsigned n=0;
        int rc=c->radio.recv(c->radio.context,f,sizeof(f),&n);
        if(rc<=0) return rc;
        if(rc!=1 || n>sizeof(f)) return INVALID;
        Luna80211Header h;
        if(luna80211_header(f,n,&h) || h.type!=2 || (h.subtype!=0 && h.subtype!=8) ||
           h.from_ds!=1 || h.to_ds || memcmp(h.bssid,c->ap.bssid,6) ||
           (memcmp(h.destination,c->mac,6) && !(h.destination[0]&1))) continue;
        c->data_rx++;
        bool crypted=h.protected_frame;
        if(crypted) {
            /* CCMP authentication precedes decapsulation, even for broadcasts. */
            size_t mpdu_len=n;
            if(c->station.wpa.phase!=WPA2_AUTHORIZED ||
               wifi_ccmp_decrypt(&c->ccmp,f,&mpdu_len)) {
                c->decrypt_errors++;
                continue;
            }
            n=(unsigned)mpdu_len;
        }
        size_t count=0;
        if(luna80211_to_ethernet(f,n,out,cap,&count)) {
            c->decap_errors++;
            continue;
        }
        c->ethernet_rx++;
        *bytes=count;*encrypted=crypted;return 1;
    }
    return 0;
}
static int install(void *v,const Wpa2Keys *keys) {
    WifiClient *c=v;return wifi_ccmp_install(&c->ccmp,keys);
}
static void clear(void *v) { WifiClient *c=v;wifi_ccmp_clear(&c->ccmp); }
static const WifiStationRadio station_ops={
    NULL,ready,rng,tx_msdu,rx_msdu,install,clear
};
static int associated(WifiClient *c,unsigned aid) {
    c->aid=aid;
    WifiStationRadio ops=station_ops;ops.context=c;
    int rc=wifi_station_associated(&c->station,&ops,c->mac,&c->ap,c->password,
                                   c->rsn,c->rsn_len,c->radio.now(c->radio.context));
    wipe(c->password,sizeof(c->password));
    if(rc) {fail(c,rc);return rc;}
    c->stage=WIFI_CLIENT_PORT;
    return 0;
}
int wifi_client_start(WifiClient *c,const WifiClientRadio *radio,const uint8_t mac[6],
                      const WifiAp *ap,const char *pass,const uint8_t *rsn,size_t rsn_len) {
    if(!c || !radio || !radio->ready || !radio->now || !radio->tune ||
       !radio->send || !radio->recv || !radio->random || !mac || !ap ||
       !unicast(mac) || !unicast(ap->bssid) || !ap->ssid_len ||
       ap->ssid_len>32 || (ap->channel>11 && ap->channel!=36 &&
       ap->channel!=40 && ap->channel!=44 && ap->channel!=48)) return INVALID;
    if(c->stage!=WIFI_CLIENT_IDLE || c->station.link!=WIFI_DOWN) return BUSY;
    if(!radio->ready(radio->context)) return IO;
    if(ap->security!=WIFI_OPEN && ap->security!=WIFI_WPA2_PSK) return UNSUPPORTED;
    unsigned n=0;
    if(ap->security==WIFI_WPA2_PSK) {
        if(!pass || !rsn || rsn_len<20 || rsn_len>WPA2_RSN_MAX ||
           rsn[0]!=48 || rsn[1]!=rsn_len-2) return INVALID;
        while(n<=WIFI_MAX_PASSWORD && pass[n]) n++;
        if(n<8 || n>WIFI_MAX_PASSWORD) return INVALID;
    }
    /* Only the scanned AP supplies these parameters; never auto-roam into
     * a new BSSID or silently downgrade an encrypted network to open. */
    c->radio=*radio;c->ap=*ap;copy(c->mac,mac,6);c->error=0;
    c->failed_stage=WIFI_CLIENT_IDLE;c->attempts=0;
    c->tx_stalled_since=0;c->tx_backpressure=0;
    c->mgmt_rx=c->foreign_rx=c->auth_rx=c->assoc_rx=0;
    c->rsn_len=rsn_len;if(rsn_len) copy(c->rsn,rsn,rsn_len);
    if(n) copy(c->password,pass,n);
    c->password[n]=0;
    int rc=radio->tune(radio->context,ap->channel);
    if(!rc) rc=auth(c);
    if(rc) fail(c,rc);
    return rc;
}
void wifi_client_poll(WifiClient *c) {
    if(!c || c->stage==WIFI_CLIENT_IDLE) return;
    if(!ready(c)) {fail(c,IO);return;}
    if(c->stage==WIFI_CLIENT_PORT) {
        wifi_station_poll(&c->station,c->radio.now(c->radio.context));
        if(c->station.link==WIFI_DOWN) fail(c,c->station.error?c->station.error:IO);
        return;
    }
    if((int32_t)(c->radio.now(c->radio.context)-c->deadline)>=0) {
        /* Retry unanswered management requests with bounded attempts.
         * MAC TX completion is not equivalent to an AP response. */
        if(c->attempts<3) {
            int rc=c->stage==WIFI_CLIENT_AUTH ? auth(c) : assoc(c);
            if(rc==BUSY) {
                uint32_t now=c->radio.now(c->radio.context);
                c->tx_backpressure++;
                if(!c->tx_stalled_since) c->tx_stalled_since=now ? now : 1;
                /* Do not wait forever on a DMA queue whose HW read pointer
                 * never advances. This is a TX transport fault, not a bad PSK. */
                if(c->tx_backpressure>=12 ||
                   (uint32_t)(now-c->tx_stalled_since)>=3000)
                    fail(c,BUSY);
                else c->deadline=now+250;
            } else if(rc) fail(c,rc);
        } else fail(c,TIMEOUT);
        return;
    }
    for(unsigned budget=0;budget<16;budget++) {
        uint8_t f[1600];unsigned n=0;
        int rc=c->radio.recv(c->radio.context,f,sizeof(f),&n);
        if(rc<0) {fail(c,rc);return;}
        if(!rc) break;
        Luna80211Header h;
        if(n>sizeof(f) || luna80211_header(f,n,&h) || h.type!=0) continue;
        c->mgmt_rx++;
        if(memcmp(h.source,c->ap.bssid,6) ||
           memcmp(h.bssid,c->ap.bssid,6) || memcmp(h.destination,c->mac,6) ||
           h.protected_frame) {c->foreign_rx++;continue;}
        if(c->stage==WIFI_CLIENT_AUTH && h.subtype==11 && n>=30) {
            c->auth_rx++;
            if(u16(f+24) || u16(f+26)!=2) continue;
            unsigned status=u16(f+28);
            if(status) {fail(c,-(int)status);return;}
            c->attempts=0;
            rc=assoc(c);if(rc) fail(c,rc);
            return;
        }
        if(c->stage==WIFI_CLIENT_ASSOC && h.subtype==1 && n>=30) {
            c->assoc_rx++;
            unsigned status=u16(f+26),aid=u16(f+28)&0x3fff;
            if(status || !aid || aid>2007) {fail(c,status?-(int)status:INVALID);return;}
            associated(c,aid);return;
        }
        if(h.subtype==12 || h.subtype==10) {fail(c,ACCESS);return;}
    }
}
WifiLink wifi_client_link(const WifiClient *c) {
    if(!c) return WIFI_DOWN;
    if(c->stage==WIFI_CLIENT_AUTH || c->stage==WIFI_CLIENT_ASSOC) return WIFI_ASSOCIATING;
    if(c->stage==WIFI_CLIENT_PORT) return c->station.link;
    return WIFI_IDLE;
}
void wifi_client_disconnect(WifiClient *c) {
    if(!c) return;
    fail(c,0);c->error=0;
}
