#include "../include/net/wpa2.h"
#include <string.h>
#include "bearssl.h"
#define EINVAL (-22)
#define EACCES (-13)
#define ENOTSUP (-95)
#define ETIMEDOUT (-110)
static void wipe(void *p, size_t n) { volatile uint8_t *b=p; while(n--) *b++=0; }
static bool same(const uint8_t *a, const uint8_t *b, size_t n) {
    unsigned d=0; while(n--) d|=*a++ ^ *b++; return d==0;
}
static unsigned get16(const uint8_t *p) { return (unsigned)p[0]*256+p[1]; }
static void put16(uint8_t *p, unsigned n) { p[0]=n>>8; p[1]=n; }
static void hmac(const br_hmac_key_context *key, const void *data, size_t n, uint8_t out[20]) {
    br_hmac_context h; br_hmac_init(&h,key,0); br_hmac_update(&h,data,n); br_hmac_out(&h,out);
    wipe(&h,sizeof(h));
}
int wpa2_pmk(const char *pass, const uint8_t *ssid, size_t n, uint8_t pmk[32]) {
    if(!pass || !ssid || !n || n>32 || !pmk) return EINVAL;
    size_t bytes=0; while(bytes<64 && pass[bytes]) bytes++;
    if(bytes<8 || bytes>63) return EINVAL;
    br_hmac_key_context key; br_hmac_key_init(&key,&br_sha1_vtable,pass,bytes);
    uint8_t salt[36], u[20], block[20]; memcpy(salt,ssid,n);
    for(unsigned index=1;index<=2;index++) {
        memset(salt+n,0,4); salt[n+3]=index;
        hmac(&key,salt,n+4,u); memcpy(block,u,20);
        for(unsigned round=1;round<4096;round++) {
            hmac(&key,u,20,u); for(unsigned j=0;j<20;j++) block[j]^=u[j];
        }
        memcpy(pmk+(index-1)*20,block,index==1 ? 20 : 12);
    }
    wipe(&key,sizeof(key)); wipe(salt,sizeof(salt)); wipe(u,sizeof(u)); wipe(block,sizeof(block));
    return 0;
}
int wpa2_ptk(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6],
             const uint8_t an[32], const uint8_t sn[32], uint8_t ptk[48]) {
    if(!pmk || !aa || !spa || !an || !sn || !ptk) return EINVAL;
    static const char label[]="Pairwise key expansion";
    uint8_t data[sizeof(label)+76+1], out[20];
    memcpy(data,label,sizeof(label)); unsigned pos=sizeof(label);
    bool order=memcmp(aa,spa,6)<0;
    memcpy(data+pos,order ? aa : spa,6); memcpy(data+pos+6,order ? spa : aa,6); pos+=12;
    order=memcmp(an,sn,32)<0;
    memcpy(data+pos,order ? an : sn,32); memcpy(data+pos+32,order ? sn : an,32); pos+=64;
    br_hmac_key_context key; br_hmac_key_init(&key,&br_sha1_vtable,pmk,32);
    for(unsigned i=0;i<3;i++) {
        data[pos]=i; hmac(&key,data,sizeof(data),out);
        memcpy(ptk+i*20,out,i==2 ? 8 : 20);
    }
    wipe(data,sizeof(data)); wipe(out,sizeof(out)); wipe(&key,sizeof(key)); return 0;
}
int wpa2_key_mic(const uint8_t kck[16], const uint8_t *p, size_t n, uint8_t mic[16]) {
    if(!kck || !p || !mic || n<99 || n>WPA2_EAPOL_MAX || get16(p+2)!=n-4) return EINVAL;
    br_hmac_key_context key; br_hmac_context h; uint8_t zeros[16]={0}, digest[20];
    br_hmac_key_init(&key,&br_sha1_vtable,kck,16); br_hmac_init(&h,&key,0);
    br_hmac_update(&h,p,81); br_hmac_update(&h,zeros,16); br_hmac_update(&h,p+97,n-97);
    br_hmac_out(&h,digest); memcpy(mic,digest,16);
    wipe(&key,sizeof(key)); wipe(&h,sizeof(h)); wipe(digest,sizeof(digest)); return 0;
}
int wpa2_key_unwrap(const uint8_t kek[16], const uint8_t *in, size_t n,
                    uint8_t *out, size_t cap, size_t *written) {
    if(!written) return EINVAL;
    *written=0;
    if(!kek || !in || !out || n<24 || n>WPA2_EAPOL_MAX || n%8 || cap<n-8) return EINVAL;
    br_aes_ct_cbcdec_keys aes; br_aes_ct_cbcdec_init(&aes,kek,16);
    uint8_t a[8], b[16], iv[16]; memcpy(a,in,8); memmove(out,in+8,n-8);
    unsigned blocks=n/8-1;
    for(int j=5;j>=0;j--) for(unsigned i=blocks;i;i--) {
        memcpy(b,a,8); unsigned t=blocks*(unsigned)j+i;
        for(unsigned k=0;k<8;k++) { b[7-k]^=t&255; t>>=8; }
        memcpy(b+8,out+(i-1)*8,8); memset(iv,0,16);
        br_aes_ct_cbcdec_run(&aes,iv,b,16);
        memcpy(a,b,8); memcpy(out+(i-1)*8,b+8,8);
    }
    uint8_t expected[8]; memset(expected,0xa6,8);
    bool ok=same(a,expected,8);
    if(ok) *written=n-8; else wipe(out,n-8);
    wipe(&aes,sizeof(aes)); wipe(a,sizeof(a)); wipe(b,sizeof(b)); wipe(iv,sizeof(iv));
    return ok ? 0 : EACCES;
}
/* Validate RSN structure and the selected suites, including optional PMKIDs.
 * AP may advertise additional suites, but no PMF-required or unknown tail. */
static int rsn_validate(const uint8_t *ie, size_t n) {
    if(!ie || n<20 || n>WPA2_RSN_MAX || ie[0]!=48 || ie[1]!=n-2 ||
       ie[2]!=1 || ie[3] || memcmp(ie+4,"\x00\x0f\xac\x04",4)) return ENOTSUP;
    unsigned pos=10, count=ie[8] | (unsigned)ie[9]<<8;
    if(!count || count>(n-pos)/4) return EINVAL;
    bool ccmp=false;
    for(unsigned i=0;i<count;i++,pos+=4) if(!memcmp(ie+pos,"\x00\x0f\xac\x04",4)) ccmp=true;
    if(n-pos<2) return EINVAL;
    count=ie[pos] | (unsigned)ie[pos+1]<<8; pos+=2;
    if(!count || count>(n-pos)/4) return EINVAL;
    bool psk=false;
    for(unsigned i=0;i<count;i++,pos+=4) if(!memcmp(ie+pos,"\x00\x0f\xac\x02",4)) psk=true;
    if(n-pos==1) return EINVAL;
    if(n-pos>=2) { if(ie[pos]&64) return ENOTSUP; pos+=2; }
    if(n-pos) {
        if(n-pos<2) return EINVAL;
        count=ie[pos] | (unsigned)ie[pos+1]<<8; pos+=2;
        if(count>(n-pos)/16) return EINVAL;
        pos+=count*16;
        if(n-pos && n-pos!=4) return EINVAL;
    }
    return ccmp && psk ? 0 : ENOTSUP;
}
static int fail(Wpa2Supplicant *s, int rc) {
    s->ops.clear(s->ops.context);
    wipe(s->pmk,sizeof(s->pmk)); wipe(s->ptk,sizeof(s->ptk));
    wipe(s->snonce,sizeof(s->snonce)); wipe(s->anonce,sizeof(s->anonce));
    s->phase=WPA2_FAILED; s->error=rc; return rc;
}
void wpa2_stop(Wpa2Supplicant *s) {
    if(!s) return;
    if(s->phase!=WPA2_OFF && s->ops.clear) s->ops.clear(s->ops.context);
    wipe(s,sizeof(*s));
}
int wpa2_start(Wpa2Supplicant *s, const Wpa2Ops *ops, const uint8_t sta[6],
                const uint8_t ap[6], const uint8_t *ssid, size_t n, const char *pass,
                const uint8_t *rsn, size_t rsn_len, uint32_t now) {
    if(!s || !ops || !ops->random || !ops->send || !ops->install || !ops->clear ||
       !sta || !ap || (sta[0]&1) || (ap[0]&1)) return EINVAL;
    unsigned sta_bits=0, ap_bits=0;
    for(unsigned i=0;i<6;i++) { sta_bits|=sta[i]; ap_bits|=ap[i]; }
    if(!sta_bits || !ap_bits || !memcmp(sta,ap,6)) return EINVAL;
    if(s->phase!=WPA2_OFF) return -16; /* zero-initialized context required */
    int rc=rsn_validate(rsn,rsn_len); if(rc) return rc;
    s->ops=*ops;
    rc=wpa2_pmk(pass,ssid,n,s->pmk);
    if(rc) { wipe(s,sizeof(*s)); return rc; }
    s->ops.clear(s->ops.context);
    memcpy(s->station,sta,6); memcpy(s->bssid,ap,6);
    memcpy(s->ap_rsn,rsn,rsn_len); s->ap_rsn_len=rsn_len;
    static const uint8_t selected[]={48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0};
    memcpy(s->station_rsn,selected,sizeof(selected)); s->station_rsn_len=sizeof(selected);
    s->phase=WPA2_WAIT_M1; s->deadline=now+10000; return 0;
}
static int reply(Wpa2Supplicant *s, const uint8_t *input, bool m4) {
    uint8_t p[99+WPA2_RSN_MAX]={0}; size_t n=99+(m4 ? 0 : s->station_rsn_len);
    p[0]=input[0]; p[1]=3; put16(p+2,n-4); p[4]=2;
    put16(p+5,m4 ? 0x030a : 0x010a); memcpy(p+9,input+9,8);
    if(!m4) { memcpy(p+17,s->snonce,32); put16(p+97,s->station_rsn_len);
        memcpy(p+99,s->station_rsn,s->station_rsn_len); }
    uint8_t mic[16]; wpa2_key_mic(s->ptk,p,n,mic); memcpy(p+81,mic,16);
    int rc=s->ops.send(s->ops.context,p,n); wipe(p,sizeof(p)); wipe(mic,sizeof(mic));
    return rc;
}
static int key_data(Wpa2Supplicant *s, const uint8_t *p, size_t n, Wpa2Keys *keys) {
    bool have_rsn=false, have_gtk=false;
    for(size_t pos=0;pos<n;) {
        /* IEEE key-data padding: vendor marker followed by zeroes. */
        if(p[pos]==0xdd && (pos+1==n || p[pos+1]==0)) {
            for(size_t i=pos+1;i<n;i++) if(p[i]) return EINVAL;
            break;
        }
        if(n-pos<2 || p[pos+1]>n-pos-2) return EINVAL;
        unsigned bytes=p[pos+1]; const uint8_t *v=p+pos+2;
        if(p[pos]==48) {
            if(have_rsn || bytes+2!=s->ap_rsn_len || !same(p+pos,s->ap_rsn,bytes+2))
                return EACCES;
            have_rsn=true;
        } else if(p[pos]==221 && bytes>=4 && !memcmp(v,"\x00\x0f\xac\x01",4)) {
            if(have_gtk || bytes!=22 || (v[4]&0xf8) || v[5]) return EINVAL;
            keys->group_index=v[4]&3; memcpy(keys->group,v+6,16); have_gtk=true;
        }
        pos+=bytes+2;
    }
    return have_rsn && have_gtk ? 0 : EINVAL;
}
int wpa2_receive(Wpa2Supplicant *s, const uint8_t source[6], const uint8_t *p,
                  size_t n, uint32_t now) {
    if(!s || !source || !p || s->phase==WPA2_OFF || s->phase==WPA2_FAILED) return EINVAL;
    if(!same(source,s->bssid,6)) return EACCES;
    if(s->phase!=WPA2_AUTHORIZED && (int32_t)(now-s->deadline)>=0) return fail(s,ETIMEDOUT);
    if(n<99 || n>WPA2_EAPOL_MAX || (p[0]!=1 && p[0]!=2) || p[1]!=3 ||
       get16(p+2)!=n-4 || p[4]!=2 || get16(p+97)!=n-99 || get16(p+7)!=16) return EINVAL;
    unsigned info=get16(p+5);
    if(info==0x008a) { /* message 1: RSN descriptor v2, pairwise, ACK */
        if(s->phase==WPA2_AUTHORIZED) return ENOTSUP; /* rekey requires reconnect */
        if(s->phase==WPA2_WAIT_M3) {
            if(!same(p+9,s->replay,8) || !same(p+17,s->anonce,32)) return EACCES;
            int rc=reply(s,p,false); return rc ? fail(s,rc) : 0;
        }
        uint8_t zeros[32]={0}; if(same(p+17,zeros,32)) return EINVAL;
        int rc=s->ops.random(s->ops.context,s->snonce,32);
        if(rc || same(s->snonce,zeros,32)) return fail(s,rc ? rc : EACCES);
        memcpy(s->anonce,p+17,32); memcpy(s->replay,p+9,8);
        wpa2_ptk(s->pmk,s->bssid,s->station,s->anonce,s->snonce,s->ptk);
        rc=reply(s,p,false); if(rc) return fail(s,rc);
        s->phase=WPA2_WAIT_M3; return 0;
    }
    if(info!=0x13ca) return ENOTSUP;
    if(s->phase!=WPA2_WAIT_M3 && s->phase!=WPA2_AUTHORIZED) return EACCES;
    if(!same(p+17,s->anonce,32)) return EACCES;
    uint8_t mic[16]; wpa2_key_mic(s->ptk,p,n,mic);
    bool valid=same(mic,p+81,16); wipe(mic,sizeof(mic));
    if(!valid) return EACCES;
    br_sha1_context hash; uint8_t digest[20];
    br_sha1_init(&hash); br_sha1_update(&hash,p,n); br_sha1_out(&hash,digest);
    if(s->phase==WPA2_AUTHORIZED) {
        /* Retransmit M4 only for the exact authenticated M3. Never reinstall
         * keys or reset CCMP packet numbers (KRACK protection). */
        valid=same(s->m3_replay,p+9,8) && same(s->m3_digest,digest,20);
        wipe(digest,sizeof(digest));
        if(!valid) return EACCES;
        int rc=reply(s,p,true); return rc ? fail(s,rc) : 0;
    }
    if(memcmp(p+9,s->replay,8)<=0) { wipe(digest,sizeof(digest)); return EACCES; }
    uint8_t plain[WPA2_EAPOL_MAX]; size_t written=0;
    int rc=wpa2_key_unwrap(s->ptk+16,p+99,n-99,plain,sizeof(plain),&written);
    Wpa2Keys keys={0};
    if(!rc) rc=key_data(s,plain,written,&keys);
    wipe(plain,sizeof(plain));
    if(rc) { wipe(&keys,sizeof(keys)); wipe(digest,sizeof(digest)); return rc; }
    memcpy(keys.pairwise,s->ptk+32,16); memcpy(keys.group_rsc,p+65,6);
    rc=s->ops.install(s->ops.context,&keys); wipe(&keys,sizeof(keys));
    if(!rc) rc=reply(s,p,true);
    if(rc) { wipe(digest,sizeof(digest)); return fail(s,rc); }
    memcpy(s->m3_replay,p+9,8); memcpy(s->m3_digest,digest,20); wipe(digest,sizeof(digest));
    wipe(s->pmk,sizeof(s->pmk)); s->phase=WPA2_AUTHORIZED; return 0;
}
void wpa2_poll(Wpa2Supplicant *s, uint32_t now) {
    if(s && (s->phase==WPA2_WAIT_M1 || s->phase==WPA2_WAIT_M3) &&
       (int32_t)(now-s->deadline)>=0) fail(s,ETIMEDOUT);
}
