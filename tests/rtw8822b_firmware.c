/* Behavioral register/device-memory model, not RTL8822BE hardware validation. */
#include "../drivers/rtw8822b_firmware.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t regs[0x1800], dmem[65536], imem[262144];
static const uint8_t *pending;
static unsigned pending_len, chunks, transferred, writes, delayed;
static int send_fail, checksum_fail, dma_stuck, boot_stuck, coex_stuck;
static uint32_t get(uint32_t off, unsigned width) {
    uint32_t v=0; for(unsigned i=0;i<width;i++) v|=(uint32_t)regs[off+i]<<(8*i);
    return v;
}
static void put(uint32_t off, unsigned width, uint32_t v) {
    for(unsigned i=0;i<width;i++) regs[off+i]=v>>(8*i);
}
static int read_reg(void *ctx, uint32_t off, unsigned width, uint32_t *v) {
    (void)ctx; assert(off+width<=sizeof(regs));
    *v=off==0x1700 ? (coex_stuck ? 0 : 1u<<29) : get(off,width);
    return 0;
}
static int write_reg(void *ctx, uint32_t off, unsigned width, uint32_t v) {
    (void)ctx; assert(off+width<=sizeof(regs)); ++writes; put(off,width,v);
    if(off==0x1208 && (v & (1u<<31))) {
        assert(get(0x1200,4)==0x18780030 && pending && pending_len==(v&0x3ffff));
        uint32_t dst=get(0x1204,4), base=dst>=0x200000 ? 0x200000 : 0;
        assert(base || dst<sizeof(imem));
        uint8_t *target=base ? dmem : imem;
        unsigned capacity=base ? sizeof(dmem) : sizeof(imem);
        assert(dst-base+pending_len<=capacity);
        memcpy(target+dst-base,pending,pending_len);
        transferred+=pending_len; pending=NULL;
        if(!dma_stuck) put(0x1208,4,checksum_fail ? 1u<<27 : 0);
    }
    if(off==3 && (v&4) && (get(0x80,2)&0x4000) && !boot_stuck)
        put(0x80,2,0xc078);
    return 0;
}
static int delay_us(void *ctx, unsigned us) {
    (void)ctx; assert(us==10); ++delayed; return 0;
}
static int send_reserved(void *ctx, const uint8_t *data, unsigned bytes) {
    (void)ctx; assert(bytes && bytes<=4096 && !pending); ++chunks;
    if(send_fail) return -5;
    pending=data; pending_len=bytes; return 0;
}
static void reset(void) {
    memset(regs,0,sizeof(regs)); memset(dmem,0,sizeof(dmem)); memset(imem,0,sizeof(imem));
    pending=NULL; pending_len=chunks=transferred=writes=delayed=0;
    send_fail=checksum_fail=dma_stuck=boot_stuck=coex_stuck=0;
    put(0x1708,4,0x12345678);
    put(0x10d,1,0xa5); put(0x100,1,0x17); put(0x550,1,0x19);
}
int main(void) {
    FILE *f=fopen("assets/firmware/rtw88/rtw8822b_fw.bin","rb"); assert(f);
    assert(!fseek(f,0,SEEK_END)); long bytes=ftell(f); assert(bytes>64); rewind(f);
    uint8_t *data=malloc(bytes); assert(data); assert(fread(data,1,bytes,f)==(size_t)bytes); fclose(f);
    RtwRegisterIo io={.read=read_reg,.write=write_reg,.delay_us=delay_us};
    RtwFirmwareTransfer t={.io=&io,.send_reserved=send_reserved};
    reset(); assert(!rtw8822b_firmware_download(&t,data,bytes));
    assert(t.firmware_running && !t.cleanup_error && transferred==(unsigned)bytes-64);
    assert(t.transferred_bytes==transferred && t.transferred_chunks==chunks && chunks>2);
    RtwFirmwareImage image; assert(!rtw8822b_firmware_parse(data,bytes,&image));
    assert(!memcmp(dmem,data+image.segments[0].offset,image.segments[0].bytes));
    assert(!memcmp(imem,data+image.segments[1].offset,image.segments[1].bytes));
    assert(get(0x1704,4)==0x12345678 && get(0x10d,1)==0xa5 && get(0x550,1)==0x19);
    reset(); assert(rtw8822b_firmware_download(&t,data,63)==-22 && !writes && !t.firmware_running);
    reset(); coex_stuck=1;
    assert(rtw8822b_firmware_download(&t,data,bytes)==-110 && !writes && !t.firmware_running);
    reset(); send_fail=1;
    assert(rtw8822b_firmware_download(&t,data,bytes)==-5 && !t.firmware_running);
    assert(!(regs[3]&4) && !(regs[0x80]&1) && get(0x10d,1)==0xa5);
    reset(); checksum_fail=1;
    assert(rtw8822b_firmware_download(&t,data,bytes)==-5 && !t.firmware_running);
    assert(!(regs[3]&4) && !(regs[0x80]&1));
    reset(); dma_stuck=1;
    assert(rtw8822b_firmware_download(&t,data,bytes)==-110 && !t.firmware_running && delayed==1000);
    assert(!(regs[3]&4));
    reset(); boot_stuck=1;
    assert(rtw8822b_firmware_download(&t,data,bytes)==-110 && !t.firmware_running && delayed==1000);
    assert(!(regs[3]&4));
    free(data);
    puts("PASS: actual firmware segmented transfer, device-memory model, checksum/boot failures and cleanup (mock device)");
    return 0;
}
