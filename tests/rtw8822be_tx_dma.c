#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../drivers/rtw8822be_pcie.c"
static uint8_t descriptors[8*TX_RING_STRIDE], frames[2*TX_RING_COUNT*2048u];
static unsigned mgmt_wp,be_wp,mgmt_rp,be_rp;
static uint32_t fake_clock;
static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
uint32_t drv_now_ms(void) {return fake_clock;}
int drv_mmio_read(DrvMmio *m,uint32_t reg,unsigned width,uint32_t *v) {
    (void)m;assert(width==4 && (reg==0x3b0 || reg==0x3a8));
    *v=(reg==0x3b0 ? mgmt_rp : be_rp)<<16;
    return 0;
}
int drv_mmio_write(DrvMmio *m,uint32_t reg,unsigned width,uint32_t v) {
    (void)m;assert(width==2 && (reg==0x3b0 || reg==0x3a8) && v<TX_RING_COUNT);
    if(reg==0x3b0)mgmt_wp=v;else be_wp=v;
    return 0;
}
void drv_memory_barrier(void){__asm__ volatile("" ::: "memory");}
int main(void) {
    dev.ready=true;dev.txbd.cpu=descriptors;dev.txbd.bus=0x40000;
    dev.txframes.cpu=frames;dev.txframes.bus=0x80000;
    uint8_t mpdu[30]={0xb0};mpdu[4]=0xaa;
    mpdu[22]=0x30;mpdu[23]=0x12; /* software sequence 0x123 */
    assert(!tx_mpdu(NULL,mpdu,sizeof(mpdu),true));
    assert(mgmt_wp==1 && dev.tx_sent==1);
    uint8_t *bd=descriptors+2*TX_RING_STRIDE;
    assert(bd[0]==48 && bd[2]==1 && bd[8]==30);
    assert(frames[48]==0xb0);
    assert((get_le32(frames+36)>>12)==0x123);
    for(unsigned i=1;i<TX_RING_COUNT-1;i++)
        assert(!tx_mpdu(NULL,mpdu,sizeof(mpdu),true));
    assert(mgmt_wp==TX_RING_COUNT-1 && dev.tx_sent==TX_RING_COUNT-1);
    assert(tx_mpdu(NULL,mpdu,sizeof(mpdu),true)==-16);
    assert(dev.tx_busy==1 && mgmt_wp==TX_RING_COUNT-1);
    dev.client.stage=WIFI_CLIENT_AUTH;
    fake_clock=800;mgmt_tx_watchdog();
    assert(dev.tx_mgmt_rekicks==1 && mgmt_wp==TX_RING_COUNT-1);
    fake_clock=1600;mgmt_tx_watchdog();
    assert(dev.tx_mgmt_rekicks==2);
    fake_clock=2500;mgmt_tx_watchdog();
    assert(dev.tx_mgmt_rekicks==2); /* bounded: no infinite kicks */
    mgmt_rp=1;
    fake_clock=2600;mgmt_tx_watchdog();
    assert(dev.tx_mgmt_rekicks==0); /* HW RX pointer progress */
    assert(!tx_mpdu(NULL,mpdu,sizeof(mpdu),true));
    assert(mgmt_wp==0 && dev.tx_sent==TX_RING_COUNT);
    mpdu[0]=8;mpdu[22]=0xf0;mpdu[23]=0xff;
    assert(!tx_mpdu(NULL,mpdu,sizeof(mpdu),false));
    assert(be_wp==1 && dev.tx_sent==TX_RING_COUNT+1);
    assert(descriptors[4*TX_RING_STRIDE+8]==30);
    assert((get_le32(frames+TX_RING_COUNT*2048u+36)>>12)==0xfff);
    /* Complete and wrap BE repeatedly, carrying distinct software sequences
     * across descriptor reuse. Data retries must not all use sequence zero. */
    for(unsigned i=0;i<2*TX_RING_COUNT;i++) {
        be_rp=be_wp;
        unsigned slot=be_wp,seq=i&0xfff;
        mpdu[22]=(uint8_t)(seq<<4);mpdu[23]=(uint8_t)(seq>>4);
        assert(!tx_mpdu(NULL,mpdu,sizeof(mpdu),false));
        uint8_t *packet=frames+(TX_RING_COUNT+slot)*2048u;
        assert((get_le32(packet+36)>>12)==seq);
        assert(!memcmp(packet+48,mpdu,sizeof(mpdu)));
    }
    uint8_t full_mtu[1601]={8};
    assert(!tx_mpdu(NULL,full_mtu,1548,false));
    assert(tx_mpdu(NULL,full_mtu,sizeof(full_mtu),false)==DRV_EINVAL);
    puts("PASS: distinct MGMT/BE PCIe DMA rings, descriptor sequence, full encrypted MTU and TX backpressure");
}
