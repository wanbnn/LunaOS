#include "../drivers/rtw8822b_native.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t regs[0x2000];
static unsigned writes, waits;
static int bus_fault, stuck, retry_only, partial_fault;
static int rd(void *ctx, uint32_t offset, unsigned width, uint32_t *out) {
    (void)ctx;
    assert(offset + width <= sizeof(regs));
    if (bus_fault) return -5;
    if (offset == 5 && !stuck && (!retry_only || waits >= 20000)) regs[5] &= ~3;
    *out = 0;
    for (unsigned i = 0; i < width; i++) *out |= (uint32_t)regs[offset+i] << (8*i);
    return 0;
}
static int wr(void *ctx, uint32_t offset, unsigned width, uint32_t value) {
    (void)ctx;
    assert(offset + width <= sizeof(regs));
    if (partial_fault && writes >= 3) return -5;
    ++writes;
    for (unsigned i = 0; i < width; i++) regs[offset+i] = value >> (8*i);
    if (offset == 5 && (value & 1)) regs[0x100] = 0;
    return 0;
}
static int delay(void *ctx, unsigned us) {
    (void)ctx;
    assert(us <= 1000000);
    waits++;
    return 0;
}
static RtwRegisterIo reset(void) {
    memset(regs, 0, sizeof(regs));
    regs[0xf0]=0x3d; regs[0xf1]=0x3d; regs[0xf2]=0x49; regs[0xf3]=0x0c;
    regs[0x100]=0xea; regs[6]=2;
    writes=waits=0; bus_fault=stuck=retry_only=partial_fault=0;
    return (RtwRegisterIo){.read=rd,.write=wr,.delay_us=delay};
}
static void put32(uint8_t *p, uint32_t v) {
    for (unsigned i=0; i<4; i++) p[i]=v>>(8*i);
}
int main(void) {
    RtwFirmwareImage image;
    uint8_t tiny[82] = {[0]=0x22,[1]=0x88,[36]=1,[48]=1};
    assert(!rtw8822b_firmware_parse(tiny,sizeof(tiny),&image));
    assert(image.count==2 && image.segments[0].offset==64 && image.segments[1].offset==73);
    for (unsigned n=0; n<sizeof(tiny); n++)
        assert(rtw8822b_firmware_parse(tiny,n,&image));
    tiny[0]=0x21; assert(rtw8822b_firmware_parse(tiny,sizeof(tiny),&image)==-95); tiny[0]=0x22;
    put32(tiny+36,UINT32_MAX);
    assert(rtw8822b_firmware_parse(tiny,sizeof(tiny),&image)==-22);
    tiny[36]=1; tiny[37]=tiny[38]=tiny[39]=0;
    put32(tiny+32,0xffffffff);
    assert(!rtw8822b_firmware_parse(tiny,sizeof(tiny),&image)); /* bit31 is the upstream address flag */
    uint8_t emem[91] = {[0]=0x22,[1]=0x88,[24]=16,[36]=1,[48]=1,[52]=1};
    assert(!rtw8822b_firmware_parse(emem,sizeof(emem),&image) && image.count==3);
    FILE *f=fopen("assets/firmware/rtw88/rtw8822b_fw.bin","rb"); assert(f);
    assert(!fseek(f,0,SEEK_END)); long n=ftell(f); assert(n>64); rewind(f);
    uint8_t *data=malloc(n+1); assert(data); assert(fread(data,1,n,f)==(size_t)n); fclose(f);
    assert(!rtw8822b_firmware_parse(data,n,&image));
    assert(image.version==30 && image.h2c_version==14 && image.count==2);
    assert(image.segments[0].device_address==0x200000 && image.segments[1].device_address==0);
    assert(rtw8822b_firmware_parse(data,n+1,&image)==-22);
    free(data);
    uint8_t rx[88]={0}; RtwRxPacket packet;
    put32(rx, 28 | (4u<<16) | (1u<<26));
    assert(!rtw8822b_rx_parse(rx,84,&packet));
    assert(packet.frame==rx+56 && packet.frame_bytes==24 && packet.phy_bytes==32);
    for (unsigned i=0; i<84; i++) assert(rtw8822b_rx_parse(rx,i,&packet));
    put32(rx,28 | (1u<<14)); assert(rtw8822b_rx_parse(rx,sizeof(rx),&packet)==-5);
    put32(rx,28 | (1u<<26)); assert(rtw8822b_rx_parse(rx,sizeof(rx),&packet)==-22);
    put32(rx,28 | (1u<<16)); assert(rtw8822b_rx_parse(rx,sizeof(rx),&packet)==-22);
    put32(rx,28); put32(rx+12,0x54);
    assert(rtw8822b_rx_parse(rx,sizeof(rx),&packet)==-22);
    put32(rx,2); put32(rx+8,1u<<28);
    assert(!rtw8822b_rx_parse(rx,sizeof(rx),&packet) && packet.c2h && packet.frame_bytes==2);
    RtwRegisterIo io=reset();
    assert(!rtw8822b_mac_power_on(&io,3) && writes && !waits);
    assert(regs[0x1103]==12 && (regs[3]&0xdc)==0xdc);
    assert(!rtw8822b_mac_power_off(&io,3));
    io=reset(); bus_fault=1;
    assert(rtw8822b_mac_power_on(&io,3)==-5 && writes==0);
    io=reset(); regs[0x100]=0;
    assert(rtw8822b_mac_power_on(&io,3)==-114 && writes==0);
    io=reset(); assert(rtw8822b_mac_power_on(&io,7)==-22 && writes==0);
    io=reset(); assert(rtw8822b_mac_power_on(&io,2)==-22 && writes==0);
    io=reset(); stuck=1;
    assert(rtw8822b_mac_power_on(&io,3)==-110 && waits==40000 && io.failed_register==5);
    io=reset(); retry_only=1;
    assert(!rtw8822b_mac_power_on(&io,3) && waits==20000);
    io=reset(); partial_fault=1;
    assert(rtw8822b_mac_power_on(&io,3)==-5 && io.writes_started);
    puts("PASS: RTL8822B real firmware, bounds, PCIe MAC power, timeout, retry and bus errors (mock registers)");
    return 0;
}
