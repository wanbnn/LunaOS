#include "../include/net/wifi_ccmp.h"
#include <string.h>
#include "bearssl.h"
#define ERR_INVAL (-22)
#define ERR_ACCESS (-13)
#define ERR_NOSPC (-28)
#define ERR_OVERFLOW (-75)
/* CCMP uses CCM with a 13-byte nonce, eight-byte MIC and 2-byte length. */
static void zero(void *p,size_t n) { volatile uint8_t *v=p; while(n--) *v++=0; }
static void aes(const br_aes_ct_cbcenc_keys *k,uint8_t b[16]) {
    uint8_t iv[16]={0};
    br_aes_ct_cbcenc_run(k,iv,b,16); /* One CBC block with zero IV is AES-ECB. */
    zero(iv,sizeof(iv));
}
static void xblock(const br_aes_ct_cbcenc_keys *k,uint8_t x[16],const uint8_t *p,size_t n) {
    for(size_t i=0;i<n;i++) x[i]^=p[i];
    aes(k,x);
}
static void nonce(const uint8_t *f,uint64_t pn,uint8_t n[13]) {
    n[0]=f[0]==0x88 ? f[24]&15 : 0; /* QoS TID, or zero for legacy */
    memcpy(n+1,f+10,6); /* transmitter address */
    for(unsigned i=0;i<6;i++) n[12-i]=(uint8_t)(pn>>(8*i));
}
static void counter(const uint8_t n[13],unsigned block,uint8_t b[16]) {
    b[0]=1; memcpy(b+1,n,13); b[14]=(uint8_t)(block>>8); b[15]=(uint8_t)block;
}
static void tag(const br_aes_ct_cbcenc_keys *k,const uint8_t *f,
                const uint8_t *p,size_t bytes,uint64_t pn,uint8_t out[8]) {
    uint8_t n[13],x[16]={0},b[16]={0},a[32]={0};
    nonce(f,pn,n);
    b[0]=0x59; memcpy(b+1,n,13); b[14]=(uint8_t)(bytes>>8); b[15]=(uint8_t)bytes;
    aes(k,b); memcpy(x,b,16);
    /* IEEE 802.11 CCMP AAD; non-QoS, non-WDS, no fragment. */
    bool qos=f[0]==0x88;
    a[1]=qos ? 24 : 22;
    a[2]=f[0]&0x8f; a[3]=f[1]&0xc7;
    memcpy(a+4,f+4,18); a[22]=f[22]&0x0f;
    if(qos) a[24]=f[24]&0x0f;
    xblock(k,x,a,16); xblock(k,x,a+16,16);
    for(size_t off=0;off<bytes;off+=16) {
        size_t nbytes=bytes-off<16?bytes-off:16;
        xblock(k,x,p+off,nbytes);
    }
    counter(n,0,b); aes(k,b);
    for(unsigned i=0;i<8;i++) out[i]=x[i]^b[i];
    zero(n,sizeof(n));zero(x,sizeof(x));zero(b,sizeof(b));zero(a,sizeof(a));
}
static void crypt(const br_aes_ct_cbcenc_keys *k,const uint8_t *f,
                  uint8_t *p,size_t bytes,uint64_t pn) {
    uint8_t n[13],b[16]; nonce(f,pn,n);
    for(size_t off=0;off<bytes;off+=16) {
        counter(n,(unsigned)(off/16)+1,b); aes(k,b);
        for(size_t j=0;j<16 && off+j<bytes;j++) p[off+j]^=b[j];
    }
    zero(n,sizeof(n));zero(b,sizeof(b));
}
static unsigned header(const uint8_t *f,size_t len,bool encrypted,bool from_ds) {
    if(!f || len<24 || (f[0]!=0x08 && f[0]!=0x88) ||
       (f[1]&0x03)!=(from_ds ? 0x02 : 0x01) ||
       (f[1]&0x80) || (f[22]&15) || (f[1]&0x0c) ||
       !!(f[1]&0x40)!=encrypted) return 0;
    if(f[0]==0x88 && (len<26 || (f[24]&0x80))) return 0; /* no A-MSDU */
    return f[0]==0x88 ? 26 : 24;
}
static uint64_t read_pn(const uint8_t *p) {
    return ((uint64_t)p[7]<<40)|((uint64_t)p[6]<<32)|((uint64_t)p[5]<<24)|
           ((uint64_t)p[4]<<16)|((uint64_t)p[1]<<8)|p[0];
}
int wifi_ccmp_install(WifiCcmp *s,const Wpa2Keys *keys) {
    if(!s || !keys || keys->group_index>3 || s->active) return ERR_INVAL;
    memcpy(s->pairwise,keys->pairwise,16);
    memcpy(s->group,keys->group,16);s->group_index=keys->group_index;
    s->tx_pn=0;
    memset(s->rx_pairwise_pn,0,sizeof(s->rx_pairwise_pn));
    memset(s->rx_group_pn,0,sizeof(s->rx_group_pn));
    /* AP-provided GTK receive sequence counter must survive authorization. */
    uint64_t rsc=0;
    for(unsigned i=0;i<6;i++) rsc|=(uint64_t)keys->group_rsc[i]<<(8*i);
    for(unsigned i=0;i<17;i++) s->rx_group_pn[s->group_index][i]=rsc;
    s->active=true;return 0;
}
void wifi_ccmp_clear(WifiCcmp *s) { if(s) zero(s,sizeof(*s)); }
int wifi_ccmp_encrypt(WifiCcmp *s,uint8_t *f,size_t *length,size_t cap) {
    if(!s || !s->active || !f || !length) return ERR_INVAL;
    unsigned hdr=header(f,*length,false,false);
    if(!hdr) return ERR_INVAL;
    size_t n=*length-hdr;
    if(n>2296 || *length>cap || cap-*length<16) return ERR_NOSPC;
    if(s->tx_pn==0xffffffffffffULL) return ERR_OVERFLOW;
    uint64_t pn=s->tx_pn+1;
    /* The packet number is consumed even if a downstream DMA write fails. */
    s->tx_pn=pn;
    memmove(f+hdr+8,f+hdr,n);
    uint8_t *iv=f+hdr;
    iv[0]=pn;iv[1]=pn>>8;iv[2]=0;iv[3]=0x20;
    iv[4]=pn>>16;iv[5]=pn>>24;iv[6]=pn>>32;iv[7]=pn>>40;
    f[1]|=0x40;
    br_aes_ct_cbcenc_keys k; br_aes_ct_cbcenc_init(&k,s->pairwise,16);
    uint8_t mic[8];tag(&k,f,f+hdr+8,n,pn,mic);
    crypt(&k,f,f+hdr+8,n,pn);
    memcpy(f+hdr+8+n,mic,8); *length+=16;
    zero(&k,sizeof(k));zero(mic,sizeof(mic));return 0;
}
int wifi_ccmp_decrypt(WifiCcmp *s,uint8_t *f,size_t *length) {
    if(!s || !s->active || !f || !length) return ERR_INVAL;
    unsigned hdr=header(f,*length,true,true);
    if(!hdr || *length<hdr+16) return ERR_INVAL;
    uint8_t *iv=f+hdr;
    if(iv[2]!=0 || (iv[3]&0x3f)!=0x20) return ERR_ACCESS;
    unsigned keyid=iv[3]>>6;
    bool group=(f[4]&1)!=0;
    if((group && keyid!=s->group_index) || (!group && keyid)) return ERR_ACCESS;
    uint64_t pn=read_pn(iv);
    unsigned tid=f[0]==0x88 ? (f[24]&15) : 16;
    uint64_t *last=group ? &s->rx_group_pn[keyid][tid] : &s->rx_pairwise_pn[tid];
    if(!pn || pn<=*last) return ERR_ACCESS;
    const uint8_t *key=group ? s->group : s->pairwise;
    size_t n=*length-hdr-16;
    if(n>2296) return ERR_INVAL;
    br_aes_ct_cbcenc_keys k; br_aes_ct_cbcenc_init(&k,key,16);
    crypt(&k,f,f+hdr+8,n,pn);
    uint8_t mic[8];tag(&k,f,f+hdr+8,n,pn,mic);
    uint8_t different=0;for(unsigned i=0;i<8;i++) different|=mic[i]^f[hdr+8+n+i];
    zero(&k,sizeof(k));zero(mic,sizeof(mic));
    if(different) { zero(f+hdr+8,n); return ERR_ACCESS; }
    *last=pn;
    memmove(f+hdr,f+hdr+8,n); *length-=16; f[1]&=~0x40;
    return 0;
}
