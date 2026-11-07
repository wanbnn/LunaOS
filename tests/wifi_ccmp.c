#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../include/net/wifi_ccmp.h"
static const uint8_t llc[13]={0xaa,0xaa,3,0,0,0,8,0,'h','e','l','l','o'};
static void sta_frame(uint8_t *f) {
    memset(f,0,128);f[0]=8;f[1]=1;
    memcpy(f+4,"\xaa\xbb\xcc\xdd\xee\xff",6);
    memcpy(f+10,"\x02\x00\x00\x00\x00\x01",6);
    memcpy(f+16,"\x02\x00\x00\x00\x00\x02",6);
    f[22]=0x10;memcpy(f+24,llc,13);
}
static void ap_frame(uint8_t *f) {
    sta_frame(f);f[1]=0x42;
    memcpy(f+4,"\x02\x00\x00\x00\x00\x01",6);
    memcpy(f+10,"\xaa\xbb\xcc\xdd\xee\xff",6);
    uint8_t iv[8]={1,0,0,0x20,0,0,0,0};
    memcpy(f+24,iv,8);
    static const uint8_t ciphertext[]={0x1d,0x5b,0x55,0xb0,0x0e,
        0x88,0x23,0x62,0xcd,0x4a,0xb9,0x3c,0xea,0x02,0x9e,0x38,
        0x85,0x0b,0xa0,0xc2,0x51};
    memcpy(f+32,ciphertext,sizeof(ciphertext));
}
int main(void) {
    Wpa2Keys keys={.group_index=1};
    for(unsigned i=0;i<16;i++) keys.pairwise[i]=i;
    WifiCcmp c={0};
    assert(!wifi_ccmp_install(&c,&keys));
    assert(wifi_ccmp_install(&c,&keys)!=0); /* no key reinstall / PN reset */
    uint8_t f[128];sta_frame(f);size_t len=37;
    assert(!wifi_ccmp_encrypt(&c,f,&len,sizeof(f)));
    assert(len==53 && f[1]==0x41 && f[24]==1 && f[27]==0x20);
    const uint8_t expected[]={0x62,0xbd,0x32,0x0f,0x2a,0x2d,0xfa,0xf8,
        0xe3,0x40,0xb6,0xcf,0x43,0xfe,0x54,0x67,0x70,0xab,0xbb,0xc1,0xdb};
    assert(!memcmp(f+32,expected,sizeof(expected))); /* Python AESCCM */
    assert(c.tx_pn==1);
    ap_frame(f);len=53;
    uint8_t tampered[128];memcpy(tampered,f,len);tampered[35]^=0x80;
    size_t bad_len=len;
    assert(wifi_ccmp_decrypt(&c,tampered,&bad_len)==-13);
    assert(c.rx_pairwise_pn[16]==0);
    assert(!wifi_ccmp_decrypt(&c,f,&len));
    assert(len==37 && f[1]==2 && !memcmp(f+24,llc,13));
    ap_frame(f);len=53;
    assert(wifi_ccmp_decrypt(&c,f,&len)==-13); /* replay */
    assert(c.rx_pairwise_pn[16]==1);
    wifi_ccmp_clear(&c);assert(!c.active && c.tx_pn==0);
    puts("PASS: independent AESCCM vectors, MIC tampering, replay, PN and safe key teardown");
}
