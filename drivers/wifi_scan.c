#include "wifi_scan.h"
#include "../include/net/luna80211.h"
#include "luna.h"
#define EINVAL (-22)
#define ENOTSUP (-95)
static int state(void *ctx, WifiState *s) {
    WifiScanner *w = ctx;
    *s = (WifiState){.radio_ready=w->radio.ready(w->radio.context),
        .can_connect=w->radio.connect && w->radio.link,
        .link=w->scanning ? WIFI_SCANNING : (w->radio.link ?
              w->radio.link(w->radio.context) : WIFI_IDLE),
        .error=w->error ? w->error : (w->radio.connection_error ?
              w->radio.connection_error(w->radio.context) : 0)};
    if (!s->radio_ready) s->link=WIFI_DOWN;
    return s->radio_ready ? 0 : ENOTSUP;
}
static int start(void *ctx) {
    WifiScanner *w=ctx;
    if (!w->radio.ready(w->radio.context)) return ENOTSUP;
    if (w->scanning || (w->radio.link && w->radio.link(w->radio.context)>=WIFI_ASSOCIATING)) return -16;
    w->count=0; w->error=0; w->channel_index=0;
    int rc=w->radio.set_channel(w->radio.context,w->channels[0]);
    if (rc) { w->error=rc; return rc; }
    w->deadline=w->radio.now_ms(w->radio.context)+220;
    w->scanning=true;
    return 0;
}
static int results(void *ctx, WifiAp *out, unsigned cap, unsigned *count) {
    WifiScanner *w=ctx;
    *count=0;
    if (w->error) return w->error;
    unsigned n=w->count < cap ? w->count : cap;
    for(unsigned i=0;i<n;i++) out[i]=w->aps[i];
    *count=n;
    return 0;
}
/* Scan support is independent of association; never imply that selecting an
 * SSID is sufficient to connect before authentication is implemented. */
static int connect_ap(void *ctx, const WifiAp *ap, const char *password) {
    WifiScanner *w=ctx;
    if(!w->radio.connect || !w->radio.link) return ENOTSUP;
    if(w->scanning || w->radio.link(w->radio.context)>=WIFI_ASSOCIATING) return -16;
    for(unsigned i=0;i<w->count;i++) if(!memcmp(ap->bssid,w->aps[i].bssid,6) &&
        ap->ssid_len==w->aps[i].ssid_len && !memcmp(ap->ssid,w->aps[i].ssid,ap->ssid_len) &&
        ap->channel==w->aps[i].channel && ap->security==w->aps[i].security)
        return w->radio.connect(w->radio.context,ap,password,w->rsn[i],w->rsn_len[i]);
    return -2; /* Stale or spoofed selection: require a new genuine scan. */
}
static WifiSecurity security(const uint8_t *frame, unsigned bytes, uint16_t caps) {
    if (!(caps & 16)) return WIFI_OPEN;
    /* Privacy without a parsed RSN suite is not assumed to be WPA2. */
    for(unsigned p=36;p+2<=bytes;) {
        unsigned id=frame[p++], n=frame[p++];
        if(n>bytes-p) return WIFI_OTHER;
        if(id==48) {
            const uint8_t *r=frame+p;
            /* RSN v1, CCMP group + pairwise, PSK AKM; reject PMF-required.
             * More complex suites remain visible as unsupported. */
            if(n<18 || r[0]!=1 || r[1] || memcmp(r+2,"\x00\x0f\xac\x04",4))
                return WIFI_OTHER;
            unsigned pairs=r[6] | ((unsigned)r[7]<<8), a=8;
            if(!pairs || pairs>(n-a)/4) return WIFI_OTHER;
            bool ccmp=false;
            for(unsigned i=0;i<pairs;i++,a+=4)
                if(!memcmp(r+a,"\x00\x0f\xac\x04",4)) ccmp=true;
            if(n-a<2) return WIFI_OTHER;
            unsigned akms=r[a] | ((unsigned)r[a+1]<<8); a+=2;
            if(!akms || akms>(n-a)/4) return WIFI_OTHER;
            bool psk=false;
            for(unsigned i=0;i<akms;i++,a+=4)
                if(!memcmp(r+a,"\x00\x0f\xac\x02",4)) psk=true;
            if(n-a==1 || (n-a>=2 && (r[a]&64))) return WIFI_OTHER;
            return ccmp && psk ? WIFI_WPA2_PSK : WIFI_OTHER;
        }
        p+=n;
    }
    return WIFI_OTHER;
}
static void ingest(WifiScanner *w, unsigned bytes, int16_t rssi) {
    Luna80211Bss b;
    if(luna80211_bss(w->frame,bytes,&b) || !b.ssid_len ||
       (b.bssid[0]&1)) return;
    if ((w->frame[1] & 0x44) || (w->frame[22] & 15)) return;
    bool nonzero=false;
    for(unsigned i=0;i<6;i++) if(b.bssid[i]) nonzero=true;
    if(!nonzero) return;
    for(unsigned i=0;i<b.ssid_len;i++) if(b.ssid[i]<32 || b.ssid[i]==127) return;
    uint8_t channel=w->channels[w->channel_index];
    if(b.channel && b.channel!=channel) return; /* adjacent-channel beacon */
    unsigned slot=w->count;
    for(unsigned i=0;i<w->count;i++)
        if(!memcmp(w->bssid[i],b.bssid,6)) { slot=i; break; }
    if(slot==WIFI_MAX_APS) {
        slot=0;
        for(unsigned i=1;i<w->count;i++)
            if(w->aps[i].rssi_dbm<w->aps[slot].rssi_dbm) slot=i;
        if(rssi<=w->aps[slot].rssi_dbm) return;
    } else if(slot==w->count) w->count++;
    WifiAp *ap=w->aps+slot;
    *ap=(WifiAp){.ssid_len=b.ssid_len,.channel=channel,.rssi_dbm=rssi,
        .security=security(w->frame,bytes,b.capability)};
    memcpy(ap->bssid,b.bssid,6);
    memcpy(ap->ssid,b.ssid,b.ssid_len); memcpy(w->bssid[slot],b.bssid,6);
    /* Preserve the AP's exact RSN IE for downgrade checks in WPA2 M3. */
    w->rsn_len[slot]=0;
    if(ap->security==WIFI_WPA2_PSK) for(unsigned p=36;p+2<=bytes;) {
        unsigned id=w->frame[p], n=w->frame[p+1];
        if(n>bytes-p-2) break;
        if(id==48 && n+2<=WPA2_RSN_MAX) {
            memcpy(w->rsn[slot],w->frame+p,n+2);w->rsn_len[slot]=n+2;break;
        }
        p+=n+2;
    }
}
static void finish(WifiScanner *w, int error) {
    w->scanning=false;
    int rc=w->radio.stop(w->radio.context);
    w->error=error ? error : rc;
}
static void poll(void *ctx) {
    WifiScanner *w=ctx;
    if(!w->scanning) {
        if(w->radio.poll_client) w->radio.poll_client(w->radio.context);
        return;
    }
    if(!w->radio.ready(w->radio.context)) { finish(w,ENOTSUP); return; }
    for(unsigned budget=0;budget<16;budget++) {
        unsigned bytes=0; int16_t rssi=0;
        int rc=w->radio.receive(w->radio.context,w->frame,sizeof(w->frame),&bytes,&rssi);
        if(rc<0) { finish(w,rc); return; }
        if(!rc) break;
        if(rc!=1 || bytes>sizeof(w->frame)) { finish(w,EINVAL); return; }
        ingest(w,bytes,rssi);
    }
    if((int32_t)(w->radio.now_ms(w->radio.context)-w->deadline)<0) return;
    if(++w->channel_index==w->channel_count) { finish(w,0); return; }
    int rc=w->radio.set_channel(w->radio.context,w->channels[w->channel_index]);
    if(rc) { finish(w,rc); return; }
    w->deadline=w->radio.now_ms(w->radio.context)+220;
}
static const WifiDriverOps ops={start,results,connect_ap,state,poll};
int wifi_scan_attach(WifiScanner *w, const WifiScanRadio *r,
                     const uint8_t *channels, unsigned count) {
    if(!w || !r || !r->ready || !r->now_ms || !r->set_channel ||
       !r->receive || !r->stop || !channels || !count || count>64) return EINVAL;
    if(w->attached) return -16; /* context must be zero-initialized */
    for(unsigned i=0;i<count;i++) {
        if(!wifi_channel_frequency(channels[i])) return EINVAL;
        for(unsigned j=0;j<i;j++) if(channels[j]==channels[i]) return EINVAL;
    }
    memset(w,0,sizeof(*w)); w->radio=*r; w->channel_count=count;
    memcpy(w->channels,channels,count);
    int rc=wifi_register_driver(&ops,w);
    if(!rc) w->attached=true;
    return rc;
}
void wifi_scan_detach(WifiScanner *w) {
    if(!w || !w->attached) return;
    if(w->scanning) finish(w,0);
    wifi_unregister_driver(&ops); w->attached=false;
}
