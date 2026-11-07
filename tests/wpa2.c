#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../include/net/wpa2.h"
#include "wpa2_vectors.h"
static unsigned sends, installs, clears;
static int rng_error, install_error, send_error;
static uint8_t sent[WPA2_EAPOL_MAX]; static size_t sent_len;
static int random_data(void *ctx,uint8_t *out,size_t n) {
    (void)ctx; assert(n==32); for(unsigned i=0;i<32;i++) out[i]=33+i; return rng_error;
}
static int send_data(void *ctx,const uint8_t *p,size_t n) {
    (void)ctx; assert(n<=sizeof(sent)); memcpy(sent,p,n); sent_len=n; sends++; return send_error;
}
static int install(void *ctx,const Wpa2Keys *keys) {
    (void)ctx; assert(!memcmp(keys->pairwise,ptk_expected+32,16));
    assert(keys->group_index==1);
    for(unsigned i=0;i<16;i++) assert(keys->group[i]==i);
    installs++; return install_error;
}
static void clear(void *ctx) { (void)ctx; clears++; }
static void start(Wpa2Supplicant *s) {
    const Wpa2Ops ops={0,random_data,send_data,install,clear};
    assert(!wpa2_start(s,&ops,sta_mac,ap_mac,(const uint8_t *)"IEEE",4,"password",
                        rsn_ie,sizeof(rsn_ie),100));
}
int main(void) {
    uint8_t out[64]; assert(!wpa2_pmk("password",(const uint8_t *)"IEEE",4,out));
    assert(!memcmp(out,pmk_expected,32));
    assert(wpa2_pmk("bad",(const uint8_t *)"IEEE",4,out)==-22);
    uint8_t nonce[32]; for(unsigned i=0;i<32;i++) nonce[i]=33+i;
    assert(!wpa2_ptk(pmk_expected,ap_mac,sta_mac,m1+17,nonce,out));
    assert(!memcmp(out,ptk_expected,48));
    /* RFC 3394 section 4.1, AES-128 unwrap known-answer vector. */
    static const uint8_t kek[]={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    static const uint8_t wrapped[]={0x1f,0xa6,0x8b,0x0a,0x81,0x12,0xb4,0x47,
        0xae,0xf3,0x4b,0xd8,0xfb,0x5a,0x7b,0x82,0x9d,0x3e,0x86,0x23,0x71,0xd2,0xcf,0xe5};
    size_t n; assert(!wpa2_key_unwrap(kek,wrapped,sizeof(wrapped),out,sizeof(out),&n) && n==16);
    for(unsigned i=0;i<16;i++) assert(out[i]==17*i);
    Wpa2Supplicant s={0}; start(&s);
    assert(wpa2_receive(&s,sta_mac,m1,sizeof(m1),101)==-13 && !sends);
    assert(!wpa2_receive(&s,ap_mac,m1,sizeof(m1),101));
    assert(sent_len==sizeof(m2_expected) && !memcmp(sent,m2_expected,sent_len));
    assert(!wpa2_receive(&s,ap_mac,m1,sizeof(m1),102) && !installs);
    uint8_t corrupt[sizeof(m3)]; memcpy(corrupt,m3,sizeof(m3)); corrupt[81]^=1;
    assert(wpa2_receive(&s,ap_mac,corrupt,sizeof(corrupt),103)==-13 && !installs);
    for(unsigned i=0;i<99;i++) assert(wpa2_receive(&s,ap_mac,m3,i,104)==-22);
    assert(!wpa2_receive(&s,ap_mac,m3,sizeof(m3),105));
    assert(s.phase==WPA2_AUTHORIZED && installs==1);
    assert(sent_len==sizeof(m4_expected) && !memcmp(sent,m4_expected,sent_len));
    assert(!wpa2_receive(&s,ap_mac,m3,sizeof(m3),106) && installs==1);
    assert(wpa2_receive(&s,ap_mac,m1,sizeof(m1),107)==-95);
    wpa2_stop(&s); assert(s.phase==WPA2_OFF && !s.ptk[0]);
    start(&s); wpa2_poll(&s,10100); assert(s.phase==WPA2_FAILED && s.error==-110);
    wpa2_stop(&s); start(&s); rng_error=-5;
    assert(wpa2_receive(&s,ap_mac,m1,sizeof(m1),101)==-5 && s.phase==WPA2_FAILED);
    rng_error=0; wpa2_stop(&s); start(&s);
    assert(!wpa2_receive(&s,ap_mac,m1,sizeof(m1),101)); install_error=-5;
    assert(wpa2_receive(&s,ap_mac,m3,sizeof(m3),102)==-5 && s.phase==WPA2_FAILED);
    install_error=0; wpa2_stop(&s); start(&s); send_error=-5;
    assert(wpa2_receive(&s,ap_mac,m1,sizeof(m1),101)==-5 && s.phase==WPA2_FAILED);
    wpa2_stop(&s);
    puts("PASS: WPA2 PMK/PTK/MIC, RFC3394, four-way handshake, replay/no-reinstall, timeout and failure cleanup");
}
