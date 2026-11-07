/* Focused RX ring simulation, no model networks in the production image. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../drivers/rtw8822be_pcie.c"
static uint8_t rx_storage[RX_COUNT*RX_BYTES], descriptors[RX_COUNT*8];
static unsigned hardware_wp, host_rp;
int drv_mmio_read(DrvMmio *m,uint32_t reg,unsigned width,uint32_t *value) {
    (void)m; assert(reg==0x3b4 && width==4);
    *value=hardware_wp<<16; return 0;
}
int drv_mmio_write(DrvMmio *m,uint32_t reg,unsigned width,uint32_t value) {
    (void)m; assert(reg==0x3b4 && width==2 && value<RX_COUNT);
    host_rp=value; return 0;
}
void drv_memory_barrier(void) { __asm__ volatile("" ::: "memory"); }
int main(void) {
    dev.payload.cpu=rx_storage; dev.payload.bus=0x100000;
    dev.descriptors.cpu=descriptors; dev.ready=true;
    uint8_t out[64]; unsigned bytes; int16_t rssi;
    assert(receive(NULL,out,sizeof(out),&bytes,&rssi)==0 && bytes==0);
    for(unsigned n=0;n<40;n++) {
        unsigned slot=dev.rp;
        uint8_t *payload=rx_storage+slot*RX_BYTES;
        memset(payload,0,RX_BYTES); le32(payload,28);
        payload[24]=0x80; /* 24-byte management frame + four-byte FCS */
        le16(descriptors+slot*8+2,dev.tag);
        hardware_wp=(slot+1)%RX_COUNT;
        assert(receive(NULL,out,sizeof(out),&bytes,&rssi)==1);
        assert(bytes==24 && out[0]==0x80 && host_rp==hardware_wp && dev.received==n+1);
        assert(dev.rp==hardware_wp && dev.tag==n+1);
    }
    /* CRC error is consumed and recycled without delivering a fake frame. */
    unsigned slot=dev.rp;
    le32(rx_storage+slot*RX_BYTES,28|(1u<<14));
    le16(descriptors+slot*8+2,dev.tag); hardware_wp=(slot+1)%RX_COUNT;
    assert(!receive(NULL,out,sizeof(out),&bytes,&rssi) && !bytes && dev.dropped==1);
    assert(host_rp==hardware_wp);
    puts("PASS: native RTL8822BE RX DMA descriptor consumption, tag/wrap/rearm and CRC discard (simulated bus)");
}
