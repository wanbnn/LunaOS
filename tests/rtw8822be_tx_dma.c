#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../drivers/rtw8822be_pcie.c"
static uint8_t descriptors[8*32], frames[8192];
static unsigned mgmt_wp,be_wp,mgmt_rp,be_rp;
int drv_mmio_read(DrvMmio *m,uint32_t reg,unsigned width,uint32_t *v) {
    (void)m;assert(width==4 && (reg==0x3b0 || reg==0x3a8));
    *v=(reg==0x3b0 ? mgmt_rp : be_rp)<<16;
    return 0;
}
int drv_mmio_write(DrvMmio *m,uint32_t reg,unsigned width,uint32_t v) {
    (void)m;assert(width==2 && (reg==0x3b0 || reg==0x3a8) && v<2);
    if(reg==0x3b0)mgmt_wp=v;else be_wp=v;
    return 0;
}
void drv_memory_barrier(void){__asm__ volatile("" ::: "memory");}
int main(void) {
    dev.ready=true;dev.txbd.cpu=descriptors;dev.txbd.bus=0x40000;
    dev.txframes.cpu=frames;dev.txframes.bus=0x80000;
    uint8_t mpdu[30]={0xb0};mpdu[4]=0xaa;
    assert(!tx_mpdu(NULL,mpdu,sizeof(mpdu),true));
    assert(mgmt_wp==1 && dev.tx_sent==1);
    uint8_t *bd=descriptors+2*32;
    assert(bd[0]==48 && bd[2]==1 && bd[8]==30);
    assert(frames[48]==0xb0);
    assert(tx_mpdu(NULL,mpdu,sizeof(mpdu),true)==-16);
    assert(dev.tx_busy==1 && mgmt_wp==1);
    mgmt_rp=1;
    assert(!tx_mpdu(NULL,mpdu,sizeof(mpdu),true));
    assert(mgmt_wp==0 && dev.tx_sent==2);
    mpdu[0]=8;assert(!tx_mpdu(NULL,mpdu,sizeof(mpdu),false));
    assert(be_wp==1 && dev.tx_sent==3);
    assert(descriptors[4*32+8]==30);
    puts("PASS: distinct MGMT/BE PCIe DMA rings, descriptor layout and TX backpressure");
}
