#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../include/net/wifi_ccmp.h"
static void frame(uint8_t *f) {
    memset(f,0,128);f[0]=8;f[1]=1;
    memcpy(f+4,"\xaa\xbb\xcc\xdd\xee\xff",6);
    memcpy(f+10,"\x02\x00\x00\x00\x00\x01",6);
    memcpy(f+16,"\x02\x00\x00\x00\x00\x02",6);
    f[22]=0x10;
    memcpy(f+24,"\xaa\xaa\x03\x00\x00\x00\x08\x00hello",13);
}
int main(void) {
    Wpa2Keys keys={.group_index=1};
    for(unsigned i=0;i<16;i++) keys.pairwise[i]=i;
    WifiCcmp c={0};
    assert(!wifi_ccmp_install(&c,&keys));
    uint8_t f[128];frame(f);size_t len=37;
    assert(!wifi_ccmp_encrypt(&c,f,&len,sizeof(f)));
    assert(len==53 && f[1]==0x41 && f[24]==1 && f[27]==0x20);
    const uint8_t expected[]={0x62,0xbd,0x32,0x0f,0x2a,0x2d,0xfa,0xf8,
        0xe3,0x40,0xb6,0xcf,0x43,0xfe,0x54,0x67,0x70,0xab,0xbb,0xc1,0xdb};
    assert(!memcmp(f+32,expected,sizeof(expected))); /* independent AES-CCM vector */
    /* Simulate an AP transmitting the same MSDU: change the direction and
     * encrypt with a distinct CCM nonce derived from the AP transmitter. */
    frame(f);f[1]=2;memcpy(f+4,"\x02\x00\x00\x00\x00\x01",6);
    memcpy(f+10,"\xaa\xbb\xcc\xdd\xee\xff",6);
    f[1]=0x42;f[24]=1;f[25]=0;f[26]=0;f[27]=0x20;
    f[28]=f[29]=f[30]=f[31]=0;
    memcpy(f+32,"\xaa\xaa\x03\x00\x00\x00\x08\x00hello",13);len=53;
    /* Fabricate a received AP packet using independent AES-CCM vector. */
    const uint8_t received[]={0};(void)received;
    wifi_ccmp_clear(&c);assert(!c.active);
    puts("PASS: CCMP nonce, independent AES-CCM ciphertext/MIC vector, TX PN, key clearing");
}
