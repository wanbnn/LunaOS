/* Indirect register model: tests arbitration controls, not RF transmission. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../drivers/rtw8822be_pcie.c"
static uint8_t regs[0x2000];
static uint32_t indirect[256], read_data, write_data;
static unsigned writes, delays, commands;
static bool stuck, ignore_write;
static uint32_t fail_reg;
int drv_mmio_read(DrvMmio *m,uint32_t reg,unsigned width,uint32_t *value) {
    (void)m;assert(reg+width<=sizeof(regs));
    if(reg==fail_reg) return DRV_EIO;
    if(reg==0x1700) {*value=stuck ? 0 : 1u<<29;return 0;}
    if(reg==0x1708) {*value=read_data;return 0;}
    *value=0;
    for(unsigned i=0;i<width;i++) *value|=(uint32_t)regs[reg+i]<<(8*i);
    return 0;
}
int drv_mmio_write(DrvMmio *m,uint32_t reg,unsigned width,uint32_t value) {
    (void)m;assert(reg+width<=sizeof(regs));
    if(reg==fail_reg) return DRV_EIO;
    writes++;
    if(reg==0x1704) {assert(width==4);write_data=value;return 0;}
    if(reg==0x1700) {
        assert(width==4 && !stuck);commands++;
        unsigned offset=value&0xffff;assert(offset<256);
        if((value&0xffff0000)==0x800f0000) read_data=indirect[offset];
        else {assert((value&0xffff0000)==0xc00f0000);if(!ignore_write) indirect[offset]=write_data;}
        return 0;
    }
    for(unsigned i=0;i<width;i++) regs[reg+i]=value>>(8*i);
    return 0;
}
int drv_udelay(unsigned delay) {assert(delay==10);delays++;return 0;}
static void reset(void) {
    memset(&dev,0,sizeof(dev));memset(regs,0xa5,sizeof(regs));
    memset(indirect,0,sizeof(indirect));indirect[0x38]=0xa55a8888;
    indirect[0xa0]=0x13570000;indirect[0xa4]=0x24680000;
    regs[0x73]=0xa1;regs[0x4f]=0xa4;regs[0xcbe]=0xad;
    writes=delays=commands=0;stuck=ignore_write=false;fail_reg=UINT32_MAX;
    dev.phy.hal.current_channel=1;
}
int main(void) {
    reset();assert(!wlan_only_init());
    assert(dev.coex_before==0xa55a8888 && dev.coex_after==0xa55a7708);
    assert(indirect[0xa0]==0x1357ffff && indirect[0xa4]==0x2468ffff);
    assert(regs[0x73]==0xa5 && regs[0x4e]==0x25 && regs[0x4f]==0xa5);
    assert(regs[0xcb4]==0x77 && regs[0xcbd]==0xa6);
    assert(regs[0x974]==0xff && regs[0x1991]==0xa4 && regs[0xcbe]==0xa5);
    assert(!wlan_antenna(36) && regs[0xcbd]==0xa5);
    assert(!wlan_antenna(11) && regs[0xcbd]==0xa6);
    /* Readiness must time out before issuing an indirect command. */
    reset();stuck=true;assert(wlan_only_init()==DRV_ETIMEDOUT);
    assert(delays==1000 && writes==0 && commands==0);
    /* Bus errors stop initialization; silent write loss fails readback. */
    reset();fail_reg=0x1704;assert(wlan_only_init()==DRV_EIO);
    assert(indirect[0x38]==0xa55a8888 && regs[0xcb4]==0xa5);
    reset();ignore_write=true;assert(wlan_only_init()==DRV_EIO);
    puts("PASS: WLAN-only RF/BB grants, preserved bits, band antenna, timeout and MMIO/readback failures");
}
