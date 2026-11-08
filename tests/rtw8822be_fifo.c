/* Register-sequence model; does not validate physical FIFO/RF behavior. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../drivers/rtw8822be_pcie.c"
static uint8_t regs[0x800];
static unsigned phase, loaded, llt_ready;
static uint32_t fail_reg;
int drv_mmio_read(DrvMmio *m,uint32_t reg,unsigned width,uint32_t *value) {
    (void)m;assert(reg+width<=sizeof(regs));
    *value=0;
    for(unsigned i=0;i<width;i++) *value|=(uint32_t)regs[reg+i]<<(8*i);
    return 0;
}
int drv_mmio_write(DrvMmio *m,uint32_t reg,unsigned width,uint32_t value) {
    (void)m;assert(reg+width<=sizeof(regs));
    if(reg==fail_reg) return DRV_EIO;
    if(reg==0x10c) {assert(phase==0 && width==2 && value==0xc5a0);phase=1;}
    if(reg==0x100) {
        assert(width==1);
        if(value==0) {assert(phase==1);phase=2;}
        else {assert(phase==2 && value==0xff);phase=3;}
    }
    if(reg>=0x230 && reg<=0x240) assert(phase==3);
    if(reg==0x22c) {
        assert(phase==3 && regs[0x100]==0xff && (value&(1u<<31)));
        for(unsigned r=0x230;r<=0x23c;r+=4) assert(regs[r]==64 && !regs[r+1]);
        assert((regs[0x240]|((unsigned)regs[0x241]<<8))==1739);
        loaded=1;
    }
    if(reg==0x208) {
        assert(loaded && regs[0x100]==0xff && (value&1));
        assert((regs[0x204]|((unsigned)regs[0x205]<<8))==1996);
        assert(regs[0x422]&16);
        value&=~1u;llt_ready=1;
    }
    if(reg==0x103) assert(llt_ready && width==1 && value==0);
    for(unsigned i=0;i<width;i++) regs[reg+i]=value>>(8*i);
    return 0;
}
int drv_udelay(unsigned delay) {(void)delay;return 0;}
int main(void) {
    /* Firmware restores disabled CR; require a fresh 0 -> ff before RQPN. */
    regs[0x103]=0xff;
    assert(!station_fifo_init());
    assert(phase==3 && loaded && llt_ready && regs[0x103]==0);
    /* A failed enable must never continue to FIFO load or LLT setup. */
    memset(regs,0,sizeof(regs));phase=loaded=llt_ready=0;fail_reg=0x100;
    assert(station_fifo_init()==DRV_EIO);
    assert(!loaded && !llt_ready && dev.io.failed_register==0x100);
    puts("PASS: RTL8822BE enables MAC DMA before RQPN/LLT, propagates MMIO failures");
}
