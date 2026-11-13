#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/elf32.h"
static void w16(uint8_t *p, unsigned n) {p[0]=n & 255;p[1]=(n>>8)&255;}
static void w32(uint8_t *p, uint32_t n) { for(int i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i)); }
static void fixture(uint8_t b[128]) {
    memset(b,0,128);
    b[0]=0x7f;b[1]='E';b[2]='L';b[3]='F';b[4]=1;b[5]=1;b[6]=1;
    w16(b+16,2);w16(b+18,3);w32(b+20,1);w32(b+24,0);
    w32(b+28,52);w16(b+40,52);w16(b+42,32);w16(b+44,1);
    w32(b+52,1);w32(b+56,84);w32(b+60,0);
    w32(b+68,4);w32(b+72,8);w32(b+76,5);
    b[84]=0x90;b[85]=0x90;b[86]=0x90;b[87]=0xc3;
}
int main(void) {
    uint8_t b[128];
    LunaElfImage img;
    fixture(b);
    assert(luna_elf32_load(b,sizeof b,&img));
    assert(img.entry==0 && img.code[0]==0x90 && img.code[3]==0xc3 && img.code[4]==0);
    assert(!luna_elf32_load(b,60,&img));
    b[4]=2;assert(!luna_elf32_load(b,sizeof b,&img));b[4]=1;
    b[5]=2;assert(!luna_elf32_load(b,sizeof b,&img));b[5]=1;
    w16(b+18,62);assert(!luna_elf32_load(b,sizeof b,&img));w16(b+18,3);
    w32(b+28,127);assert(!luna_elf32_load(b,sizeof b,&img));w32(b+28,52);
    w32(b+60,4095);w32(b+72,8);assert(!luna_elf32_load(b,sizeof b,&img));
    w32(b+60,0);
    w32(b+56,127);assert(!luna_elf32_load(b,sizeof b,&img));w32(b+56,84);
    w32(b+68,16);assert(!luna_elf32_load(b,sizeof b,&img));w32(b+68,4);
    w32(b+24,100);assert(!luna_elf32_load(b,sizeof b,&img));w32(b+24,0);
    w32(b+76,7);assert(!luna_elf32_load(b,sizeof b,&img));
    fixture(b);w16(b+44,2);assert(!luna_elf32_load(b,sizeof b,&img));
    puts("PASS: ELF32 valid, malformed headers, bounds, entry and RX checks");
    return 0;
}
