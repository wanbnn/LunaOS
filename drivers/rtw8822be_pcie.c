/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2018-2019 Realtek Corporation
 * Native polled RTL8822BE PCIe transport adapted from rtw88/pci.c, fw.c,
 * tx.c and mac.c. No Linux runtime. Passive RX only after firmware bring-up.
 */
#include "driver.h"
#include "rtw8822be_pcie.h"
#include "rtw8822be_phy.h"
#include "rtw8822b_firmware.h"
#include "wifi_scan.h"
#include "wifi_client.h"
#include "web.h"
#define RX_COUNT 16u
#define RX_BYTES 11520u
#define TX_RING_COUNT 16u
#define TX_RING_STRIDE (TX_RING_COUNT*16u) /* 16 16-byte PCIe TXBD entries per queue */
static struct {
    bool present, master, powered, ready, emulated;
    const char *stage;
    int error;
    DrvPci pci;
    DrvMmio mmio;
    DrvDma descriptors, payload, txbd, tx, txframes;
    DrvFirmware firmware;
    RtwRegisterIo io;
    RtwFirmwareTransfer transfer;
    struct rtw_dev phy;
    WifiScanner scanner;
    WifiClient client;
    uint8_t mac[6];
    unsigned txwp[8], tx_sent, tx_busy, tx_errors;
    uint32_t diag_tx_mgmt_idx, diag_tx_be_idx, diag_txdma_status;
    uint32_t diag_tx_mgmt_base, diag_tx_mgmt_count, diag_tx_pause, diag_tx_offset;
    uint32_t diag_mgmt_avail_pages, diag_txdma_mapping, diag_tx_pkt_empty, diag_be_edca, diag_tcr;
    uint32_t tx_mgmt_last_rp, tx_mgmt_stall_at, tx_mgmt_rekicks, tx_mgmt_rekick_error;
    uint32_t tx_mgmt_last_bd0, tx_mgmt_last_bd1, tx_mgmt_last_desc0;
    bool tx_mgmt_tracking;
    unsigned rp, tag, received, dropped, rx_invalid_tag, rx_invalid_packet, rx_c2h;
    unsigned rx_dma_polls, rx_pending, rx_last_index;
    uint32_t diag_cr, diag_rcr, diag_pcictrl, diag_rxfilter;
    uint32_t diag_rxdma_status, diag_rf_rxpath, diag_rxigi;
} dev;
static int rd(void *ctx,uint32_t reg,unsigned width,uint32_t *out) {
    (void)ctx; return drv_mmio_read(&dev.mmio,reg,width,out);
}
static int wr(void *ctx,uint32_t reg,unsigned width,uint32_t value) {
    (void)ctx; return drv_mmio_write(&dev.mmio,reg,width,value);
}
static int us(void *ctx,unsigned delay) {
    (void)ctx;
    while(delay) {
        unsigned chunk=delay>1000 ? 1000 : delay;
        int rc=drv_udelay(chunk); if(rc) return rc;
        delay-=chunk;
    }
    return 0;
}
static int read32(uint32_t reg,uint32_t *v) { return rd(NULL,reg,4,v); }
static int write(uint32_t reg,unsigned width,uint32_t value) {
    int rc=wr(NULL,reg,width,value);
    if(rc) dev.io.failed_register=reg;
    return rc;
}
static int bits(uint32_t reg,unsigned width,uint32_t clear,uint32_t set) {
    uint32_t v; int rc=rd(NULL,reg,width,&v);
    return rc ? rc : write(reg,width,(v&~clear)|set);
}
static int wait_reg(uint32_t reg,unsigned width,uint32_t mask,uint32_t value) {
    for(unsigned i=0;i<1000;i++) {
        uint32_t v; int rc=rd(NULL,reg,width,&v); if(rc) return rc;
        if((v&mask)==value) return 0;
        rc=us(NULL,10); if(rc) return rc;
    }
    dev.io.failed_register=reg; return DRV_ETIMEDOUT;
}
static void append_error(char *out,size_t cap,int err) {
    char value[20];
    if(err<0) {append(out,"-",cap);number(value,0u-(uint32_t)err);}
    else number(value,(uint32_t)err);
    append(out,value,cap);
}
static void le16(uint8_t *p,uint16_t v) { p[0]=v; p[1]=v>>8; }
static void le32(uint8_t *p,uint32_t v) { le16(p,v); le16(p+2,v>>16); }
static void arm_rx(unsigned index) {
    uint8_t *bd=(uint8_t *)dev.descriptors.cpu+index*8;
    le16(bd,RX_BYTES); le16(bd+2,0);
    le32(bd+4,dev.payload.bus+index*RX_BYTES);
}
static int allocate_rings(void) {
    int rc=drv_dma_alloc(RX_COUNT*8,256,UINT32_MAX,0,&dev.descriptors);
    if(!rc) rc=drv_dma_alloc(RX_COUNT*RX_BYTES,4096,UINT32_MAX,0,&dev.payload);
    if(!rc) rc=drv_dma_alloc(8*TX_RING_STRIDE,256,UINT32_MAX,0,&dev.txbd);
    if(!rc) rc=drv_dma_alloc(4096+48,256,UINT32_MAX,0,&dev.tx);
    /* Sixteen descriptors for MGMT and BE, isolated 2048-byte DMA slots. */
    if(!rc) rc=drv_dma_alloc(2*TX_RING_COUNT*2048u,256,UINT32_MAX,0,&dev.txframes);
    return rc;
}
static int rings(void) {
    int rc=bits(0x303,1,0,0xf7);
    if(rc) return rc;
    dev.rp=dev.tag=0;
    memset(dev.txwp,0,sizeof(dev.txwp));
    dev.tx_mgmt_tracking=false;
    dev.tx_mgmt_stall_at=dev.tx_mgmt_last_rp=0;
    dev.tx_mgmt_rekicks=dev.tx_mgmt_rekick_error=0;
    memset(dev.txbd.cpu,0,dev.txbd.size);
    for(unsigned i=0;i<RX_COUNT;i++) arm_rx(i);
    static const uint32_t bases[]={0x308,0x1320,0x310,0x330,0x328,0x320,0x318,0x340};
    static const uint32_t counts[]={0,0x1328,0x380,0x38a,0x388,0x386,0x384,0x38c};
    rc=bits(0x303,1,0,0xf7); /* Stop ordinary TX DMA queues. Beacon reserved page remains usable. */
    for(unsigned i=0;i<8 && !rc;i++) {
        rc=write(bases[i],4,dev.txbd.bus+i*TX_RING_STRIDE);
        if(!rc && i) rc=write(counts[i],2,TX_RING_COUNT);
    }
    if(!rc) rc=write(0x382,2,RX_COUNT);
    if(!rc) rc=write(0x338,4,dev.descriptors.bus);
    if(!rc) rc=write(0x39c,4,UINT32_MAX);
    if(!rc) rc=bits(0x1330,4,0,(1u<<16)|(1u<<8));
    if(!rc) rc=bits(0x300,4,0,(1u<<20)|(1u<<15));
    drv_memory_barrier();
    return rc;
}
static int reserved(void *ctx,const uint8_t *data,unsigned bytes) {
    (void)ctx;
    if(!bytes || bytes>4096) return DRV_EINVAL;
    uint32_t cr,bcn,txq;
    int rc=rd(NULL,0x101,1,&cr);
    if(!rc) rc=rd(NULL,0x550,1,&bcn);
    if(!rc) rc=rd(NULL,0x422,1,&txq);
    if(rc) return rc;
    rc=write(0x204,2,0x8000); /* page zero, clear old BCN_VALID */
    if(!rc) rc=write(0x101,1,cr|1);
    if(!rc) rc=write(0x550,1,(bcn&~8u)|16);
    if(!rc) rc=write(0x422,1,txq&~64u);
    if(!rc) {
        uint8_t *packet=dev.tx.cpu, *bd=dev.txbd.cpu;
        memset(packet,0,48); memcpy(packet+48,data,bytes);
        le32(packet,bytes|(48u<<16)|(1u<<26)|(1u<<31));
        le32(packet+4,16u<<8); /* beacon QSEL */
        le32(packet+12,(1u<<8)|(1u<<10));
        le32(packet+32,1u<<15); /* hardware sequence */
        unsigned checksum=0;
        for(unsigned i=0;i<32;i+=2) checksum^=packet[i]|((unsigned)packet[i+1]<<8);
        le16(packet+28,checksum);
        memset(bd,0,16); le16(bd,48);
        le16(bd+2,((48+bytes+127)/128)|0x8000); le32(bd+4,dev.tx.bus);
        le16(bd+8,bytes); le32(bd+12,dev.tx.bus+48);
        drv_memory_barrier();
        rc=bits(0x383,1,0,16); /* BCNQ kick */
        if(!rc) rc=wait_reg(0x204,2,0x8000,0x8000);
        drv_memory_barrier();
    }
    int cleanup=write(0x204,2,0x8000);
    int r=write(0x550,1,bcn); if(!cleanup) cleanup=r;
    r=write(0x422,1,txq); if(!cleanup) cleanup=r;
    r=write(0x101,1,cr); if(!cleanup) cleanup=r;
    return rc ? rc : cleanup;
}
static bool radio_ready(void *ctx) { (void)ctx; return dev.ready && !dev.error; }
static uint32_t now(void *ctx) { (void)ctx; return drv_now_ms(); }
static int stop(void *ctx) {
    (void)ctx;
    /* RF remains on the last channel; polling drains queued frames. */
    return 0;
}
static int tune(void *ctx,uint8_t channel) {
    (void)ctx;
    int rc=bits(0x100,2,0x80,0); /* Stop MAC RX while RF settles. TX is always disabled. */
    if(!rc) rc=rtw8822be_phy_channel(&dev.phy,channel);
    /* Discard frames queued on the previous channel before accepting beacons. */
    uint32_t index=0;
    if(!rc) rc=read32(0x3b4,&index);
    unsigned wp=(index>>16)&0xfff;
    if(!rc && wp>=RX_COUNT) rc=DRV_EIO;
    while(!rc && dev.rp!=wp) {
        arm_rx(dev.rp); dev.rp=(dev.rp+1)%RX_COUNT; dev.tag=(dev.tag+1)%8192;
    }
    drv_memory_barrier();
    if(!rc) rc=write(0x3b4,2,dev.rp);
    /* Restore both MAC RX and MAC TX after retuning. Clearing bit 6
     * here silently disabled every later authentication/data transmit. */
    if(!rc) rc=bits(0x100,2,0,0xc0);
    if(rc) { dev.error=rc; dev.ready=false; dev.stage="channel/RF"; }
    return rc;
}
static int receive(void *ctx,uint8_t *out,unsigned capacity,unsigned *bytes,int16_t *rssi) {
    (void)ctx; *bytes=0;
    uint32_t index;
    int rc=read32(0x3b4,&index);
    if(rc) return rc;
    unsigned wp=(index>>16)&0xfff;
    dev.rx_dma_polls++;
    dev.rx_last_index=index;
    /* Sample read-only registers while scanning, without changing radio state. */
    if(!(dev.rx_dma_polls & 63u)) {
        (void)read32(0x100,&dev.diag_cr);
        (void)read32(0x608,&dev.diag_rcr);
        (void)read32(0x300,&dev.diag_pcictrl);
        (void)read32(0x6a0,&dev.diag_rxfilter);
        (void)read32(0x288,&dev.diag_rxdma_status);
        (void)read32(0x808,&dev.diag_rf_rxpath);
        (void)read32(0xc50,&dev.diag_rxigi);
    }
    if(wp>=RX_COUNT) return DRV_EIO;
    if(wp==dev.rp) return 0;
    dev.rx_pending++;
    drv_memory_barrier();
    const volatile uint16_t *bd=(volatile uint16_t *)((uint8_t *)dev.descriptors.cpu+dev.rp*8);
    unsigned tag=bd[1];
    RtwRxPacket packet;
    const uint8_t *payload=(const uint8_t *)dev.payload.cpu+dev.rp*RX_BYTES;
    if(tag!=dev.tag) dev.rx_invalid_tag++;
    rc=tag==dev.tag ? rtw8822b_rx_parse(payload,RX_BYTES,&packet) : DRV_EIO;
    if(rc && tag==dev.tag) dev.rx_invalid_packet++;
    if(!rc && packet.c2h) dev.rx_c2h++;
    int delivered=0;
    if(!rc && !packet.c2h && packet.frame_bytes<=capacity) {
        memcpy(out,packet.frame,packet.frame_bytes); *bytes=packet.frame_bytes;
        *rssi=-110;
        if(packet.phy && packet.phy_bytes>=4) {
            unsigned page=packet.phy[0]&15;
            if(page==0) *rssi=(int)packet.phy[1]-110;
            else if(page==1) {
                unsigned power=packet.phy[1]>packet.phy[2] ? packet.phy[1] : packet.phy[2];
                *rssi=(int)power-110;
            }
        }
        delivered=1; dev.received++;
    } else dev.dropped++;
    dev.tag=(dev.tag+1)%8192;
    arm_rx(dev.rp); dev.rp=(dev.rp+1)%RX_COUNT;
    drv_memory_barrier();
    rc=write(0x3b4,2,dev.rp);
    if(rc) { dev.error=rc; dev.ready=false; return rc; }
    return delivered;
}
/* Ordinary MGMT/BE PCIe TX uses distinct buffers from the firmware
 * reserved-page beacon queue. Every accepted MPDU gets its own live DMA slot.
 * The NIC advances its read index only after consuming a descriptor. */
static int tx_mpdu(void *ctx,const uint8_t *mpdu,unsigned n,bool mgmt) {
    (void)ctx;
    if(!dev.ready || dev.error || !mpdu || n<24 || n>1540) return DRV_EINVAL;
    /* rtw88 PCI order: BCN=0 H2C=1 MGMT=2 BK=3 BE=4 VI=5 VO=6 HI=7.
     * Index registers are NOT the count registers. */
    unsigned queue=mgmt ? 2 : 4;
    uint32_t idxreg=mgmt ? 0x3b0 : 0x3a8, index=0;
    int rc=read32(idxreg,&index);
    if(rc) {dev.tx_errors++;return rc;}
    unsigned rp=(index>>16)&0xfffu;
    if(rp>=TX_RING_COUNT) {dev.tx_errors++;return DRV_EIO;}
    unsigned wp=dev.txwp[queue], next=(wp+1)%TX_RING_COUNT;
    if(next==rp) {dev.tx_busy++;return -16;}
    unsigned slot=(mgmt?0:TX_RING_COUNT)+wp;
    uint8_t *pkt=(uint8_t *)dev.txframes.cpu+slot*2048u;
    uint32_t bus=dev.txframes.bus+slot*2048u;
    uint8_t *bd=(uint8_t *)dev.txbd.cpu+queue*TX_RING_STRIDE+wp*16u;
    memset(pkt,0,48);
    memcpy(pkt+48,mpdu,n);
    /* 48-byte Realtek TX packet descriptor. Software sequence, legacy rate
     * 1 Mbps (2.4 GHz) / 6 Mbps (5 GHz), no undocumented HW security keys. */
    le32(pkt,n|(48u<<16)|(1u<<26)|(1u<<31));
    le32(pkt+4,(mgmt ? 0x12u : 0u)<<8);
    le32(pkt+12,(1u<<8)|(1u<<10));
    le32(pkt+16,dev.phy.hal.current_channel>14 ? 4 : 0);
    unsigned checksum=0;
    for(unsigned i=0;i<32;i+=2) checksum^=pkt[i]|((unsigned)pkt[i+1]<<8);
    le16(pkt+28,checksum);
    memset(bd,0,16);
    le16(bd,48);
    le16(bd+2,(48+n+127)/128);
    le32(bd+4,bus);
    le16(bd+8,n);
    le32(bd+12,bus+48);
    drv_memory_barrier();
    rc=write(idxreg,2,next);
    if(rc) {dev.tx_errors++;return rc;}
    dev.txwp[queue]=next;
    if(mgmt) {
        if(!dev.tx_mgmt_tracking) {
            dev.tx_mgmt_tracking=true;
            dev.tx_mgmt_last_rp=rp;
            dev.tx_mgmt_stall_at=drv_now_ms();
            dev.tx_mgmt_rekicks=0;
        }
        dev.tx_mgmt_last_bd0=((uint32_t)bd[3]<<24)|((uint32_t)bd[2]<<16)|
                             ((uint32_t)bd[1]<<8)|bd[0];
        dev.tx_mgmt_last_bd1=((uint32_t)bd[7]<<24)|((uint32_t)bd[6]<<16)|
                             ((uint32_t)bd[5]<<8)|bd[4];
        dev.tx_mgmt_last_desc0=((uint32_t)pkt[3]<<24)|((uint32_t)pkt[2]<<16)|
                               ((uint32_t)pkt[1]<<8)|pkt[0];
    }
    dev.tx_sent++;
    return 0;
}
static int rx_mpdu(void *ctx,uint8_t *frame,unsigned capacity,unsigned *bytes) {
    int16_t rssi=0;
    return receive(ctx,frame,capacity,bytes,&rssi);
}
static int csprng(void *ctx,uint8_t *out,size_t bytes) {
    (void)ctx;return random_bytes(out,bytes) ? 0 : DRV_EIO;
}
static int set_mac_bssid(const uint8_t mac[6],const uint8_t ap[6]) {
    int rc=write(0x610,4,mac[0]|((uint32_t)mac[1]<<8)|
           ((uint32_t)mac[2]<<16)|((uint32_t)mac[3]<<24));
    if(!rc) rc=write(0x614,2,mac[4]|((uint32_t)mac[5]<<8));
    if(!rc) rc=write(0x618,4,ap[0]|((uint32_t)ap[1]<<8)|
           ((uint32_t)ap[2]<<16)|((uint32_t)ap[3]<<24));
    if(!rc) rc=write(0x61c,2,ap[4]|((uint32_t)ap[5]<<8));
    if(!rc) rc=bits(0x100,4,3u<<16,2u<<16); /* VIF0 infrastructure STA. */
    return rc;
}
static int station_connect(void *ctx,const WifiAp *ap,const char *pass,
                           const uint8_t *rsn,size_t rsn_len) {
    (void)ctx;
    if(!dev.ready || !ap) return DRV_EIO;
    if(dev.client.stage!=WIFI_CLIENT_IDLE) return -16;
    dev.tx_mgmt_tracking=false;
    dev.tx_mgmt_rekicks=0;
    /* Block unsupported channels before any TX is queued. */
    if(ap->channel<1 || (ap->channel>11 && ap->channel!=36 &&
       ap->channel!=40 && ap->channel!=44 && ap->channel!=48)) return -95;
    int rc=set_mac_bssid(dev.mac,ap->bssid);
    if(rc) return rc;
    WifiClientRadio radio={.ready=radio_ready,.now=now,.tune=tune,
        .send=tx_mpdu,.recv=rx_mpdu,.random=csprng};
    rc=wifi_client_start(&dev.client,&radio,dev.mac,ap,pass,rsn,rsn_len);
    if(rc) dev.stage="station connect failed";
    return rc;
}
static WifiLink station_link(void *ctx) {
    (void)ctx;return wifi_client_link(&dev.client);
}
static int station_error(void *ctx) {
    (void)ctx;return dev.client.error;
}
/* A stalled PCIe MGMT TX ring should normally consume descriptors within
 * milliseconds. Re-kick the SAME producer index at most twice per connection,
 * following the bounded queue kick used by upstream rtw88 recovery proposals.
 * Never clear indices, reuse DMA slots, or mutate queued packets here. */
static void mgmt_tx_watchdog(void) {
    if(!dev.ready || !dev.tx_mgmt_tracking || !dev.client.stage) return;
    uint32_t idx=0;
    if(read32(0x3b0,&idx)) return;
    unsigned rp=(idx>>16)&0xfffu;
    if(rp>=TX_RING_COUNT) return;
    uint32_t ms=drv_now_ms();
    if(rp!=dev.tx_mgmt_last_rp) {
        dev.tx_mgmt_last_rp=rp;
        dev.tx_mgmt_stall_at=ms;
        dev.tx_mgmt_rekicks=0;
    }
    if(rp==dev.txwp[2]) {
        dev.tx_mgmt_tracking=false;
        return;
    }
    if((uint32_t)(ms-dev.tx_mgmt_stall_at)<750 || dev.tx_mgmt_rekicks>=2) return;
    drv_memory_barrier();
    int rc=write(0x3b0,2,dev.txwp[2]);
    if(rc) dev.tx_mgmt_rekick_error++;
    dev.tx_mgmt_rekicks++;
    dev.tx_mgmt_stall_at=ms;
}
static void station_poll(void *ctx) {
    (void)ctx;
    mgmt_tx_watchdog();
    wifi_client_poll(&dev.client);
}

static void cleanup(void) {
    dev.ready=false;
    wifi_client_disconnect(&dev.client);
    /* Never free a descriptor or payload while PCI bus mastering can continue. */
    if(dev.master) {
        int rc=drv_pci_master(&dev.pci,false);
        uint32_t command=4;
        if(!rc) rc=drv_pci_config_read(&dev.pci,4,2,&command);
        if(rc || (command&4)) { dev.stage="DMA stop failed; reboot required"; return; }
        dev.master=false;
    }
    if(dev.powered && dev.mmio.cookie) {
        uint32_t id=0; rd(NULL,0xf0,4,&id);
        (void)rtw8822b_mac_power_off(&dev.io,(id>>12)&15);
        dev.powered=false;
    }
    if(dev.firmware.data) drv_firmware_release(&dev.firmware);
    if(dev.payload.cpu) drv_dma_free(&dev.payload);
    if(dev.descriptors.cpu) drv_dma_free(&dev.descriptors);
    if(dev.tx.cpu) drv_dma_free(&dev.tx);
    if(dev.txframes.cpu) drv_dma_free(&dev.txframes);
    if(dev.txbd.cpu) drv_dma_free(&dev.txbd);
    if(dev.mmio.cookie) drv_mmio_unmap(&dev.mmio);
    if(dev.pci.cookie && !dev.mmio.cookie) drv_pci_release(&dev.pci);
}
void rtw8822be_init(void) {
    dev.stage="PCI discovery";
    int rc=drv_pci_find(0x10ec,0xb822,0,&dev.pci);
    if(rc) { dev.error=rc; return; }
    dev.present=true;
    dev.emulated=dev.pci.subsystem==0x88221b36u;
    dev.io=(RtwRegisterIo){.read=rd,.write=wr,.delay_us=us};
#define STEP(label,expr) do { dev.stage=label; rc=(expr); if(rc) goto fail; } while(0)
    STEP("PCI claim",drv_pci_claim(&dev.pci));
    STEP("PCI MMIO decode",drv_pci_enable(&dev.pci,false,true));
    STEP("BAR2 map",drv_pci_map(&dev.pci,2,&dev.mmio));
    STEP("interrupt mask",write(0xb0,4,0));
    STEP("interrupt mask",write(0xb8,4,0));
    STEP("interrupt mask",write(0x10b0,4,0));
    STEP("interrupt mask",write(0x10b8,4,0));
    STEP("DMA allocation",allocate_rings());
    STEP("DMA rings",rings());
    uint32_t id;
    STEP("chip identity",read32(0xf0,&id));
    if(!id || id==UINT32_MAX) { rc=DRV_EIO; goto fail; }
    unsigned cut=(id>>12)&15;
    dev.stage="MAC power"; dev.powered=true;
    rc=rtw8822b_mac_power_on(&dev.io,cut);
    if(rc==DRV_EALREADY) {
        STEP("MAC reset",rtw8822b_mac_power_off(&dev.io,cut));
        STEP("MAC power",rtw8822b_mac_power_on(&dev.io,cut));
    } else if(rc) goto fail;
    dev.powered=true;
    STEP("firmware file",drv_firmware_request("rtw88/rtw8822b_fw.bin",&dev.firmware));
    STEP("DMA rings after power",rings());
    dev.master=true; STEP("bus mastering",drv_pci_master(&dev.pci,true));
    dev.transfer=(RtwFirmwareTransfer){.io=&dev.io,.send_reserved=reserved};
    STEP("firmware upload/MCU boot",rtw8822b_firmware_download(&dev.transfer,
        dev.firmware.data,dev.firmware.size));
    STEP("firmware release",drv_firmware_release(&dev.firmware));
    STEP("DMA pause",drv_pci_master(&dev.pci,false)); dev.master=false;
    STEP("DMA rings after firmware",rings());
    STEP("efuse/PHY tables",rtw8822be_phy_prepare(&dev.phy,&dev.io));
    memcpy(dev.mac,dev.phy.efuse.mac,6);
    /* Chip FIFO split from rtw88's PCIe page table (2048 TX pages,
     * 52 reserved, four 64-page priority queues, one gap page). */
    STEP("queue map",write(0x10c,2,(2u<<4)|(2u<<6)|(1u<<8)|(1u<<10)|(0u<<12)|(3u<<14)));
    STEP("FIFO high",write(0x230,2,64));
    STEP("FIFO low",write(0x234,2,64));
    STEP("FIFO normal",write(0x238,2,64));
    STEP("FIFO extra",write(0x23c,2,64));
    STEP("FIFO public",write(0x240,2,1739));
    STEP("FIFO load",bits(0x22c,4,0,1u<<31));
    STEP("TX free tail",bits(0x422,1,0,16));
    STEP("reserved boundary",write(0x204,2,1996));
    STEP("beacon boundary",write(0x424,2,1996));
    STEP("beacon1 boundary",write(0x456,2,1996));
    STEP("reserved tail",write(0x206,2,1996));
    STEP("LLT initialization",bits(0x208,1,0,1));
    STEP("LLT ready",wait_reg(0x208,1,1,0));
    STEP("MSR unassociated",write(0x102,1,0));
    STEP("MAC option",write(0x7d8,4,0xb0810041));
    STEP("MAC receive option",write(0x7d4,1,0x98));
    STEP("RX length limit",write(0x60c,1,24));
    /* Passive 20-MHz monitor receive path. No MAC TX, no probes or association. */
    STEP("RX FIFO",write(0x11c,4,24576-256-1));
    STEP("RX boundary",write(0x116,2,24576-128-1));
    STEP("RX descriptor length",bits(0x115,1,15,15));
    STEP("RX PHY status",write(0x60f,1,4));
    /* Complete RTL8822B TX MAC protocol configuration omitted from the
     * earlier passive-only scanner. These constants mirror the upstream
     * rtw8822b_mac_init (not invented PCIe or RF settings). */
    STEP("TX hang watchdog",bits(0x45e,1,0,1u<<2));
    STEP("TX AMPDU time",write(0x455,1,0x70));
    STEP("TX protection mode",write(0x4c8,4,0x202008ff));
    STEP("TX BAR retries",write(0x4ce,2,0x0801));
    STEP("TX fast EDCA VO",write(0x1448,1,6));
    STEP("TX fast EDCA VI",write(0x144a,1,6));
    STEP("TX fast EDCA BE",write(0x144c,1,6));
    STEP("TX fast EDCA BK",write(0x144e,1,6));
    STEP("TX slot time",write(0x51b,1,9));
    STEP("TX PIFS",write(0x512,1,0x19));
    STEP("TX SIFS",write(0x514,4,0x10100e0a));
    STEP("TX VO TXOP limit",write(0x502,2,0x186));
    STEP("TX VI TXOP limit",write(0x506,2,0x3bc));
    STEP("TX NAV",write(0x544,4,0x001b0005));
    STEP("TX TSF RX offset",write(0x55e,2,0x3030));
    STEP("TX unpause",write(0x522,2,0));
    /* Realtek enables TX rate-control descriptor features in TCR+1/+2. */
    STEP("TX TCR1",write(0x605,1,0x30));
    STEP("TX TCR2",write(0x606,1,0x30));
    STEP("RX management filter",write(0x6a0,2,0xffff));
    STEP("RX control filter",write(0x6a2,2,0x0fff));
    STEP("RX data filter",write(0x6a4,2,0xffff));
    /* Upstream rtw8822b_mac_init WLAN_RCR_CFG=0xe400220e.
     * Enable AAP bit for passive beacons addressed to other receivers.
     * Former value 0x9000200f omitted multiple required RX flags. */
    STEP("RX RCR",write(0x608,4,0xe400220fu));
    STEP("interrupt mask",write(0xb0,4,0));
    STEP("interrupt mask",write(0xb8,4,0));
    STEP("interrupt mask",write(0x10b8,4,0));
    dev.master=true; STEP("RX bus mastering",drv_pci_master(&dev.pci,true));
    /* The upstream rtw88 PCIe ring setup preserves PCI_CTRL+3 = 0xf7
     * through normal operation. Clearing the byte here diverged from the
     * reference driver and can leave the management TX engine unserviced.
     * Do not clear these control bits when enabling the MAC.
     */
    STEP("PCIe queue control",bits(0x303,1,0,0xf7));
    /* Upstream REG_CR MAC_TRX_ENABLE = 0xff (all MAC/HCI DMA paths).
     * Do not leave TX DMA disabled or retain stale power-on bits. */
    STEP("MAC TRX enable",write(0x100,1,0xff));
    dev.ready=true;
    /* Receive-only channels; no active probes or transmit-power assumptions. */
    static const uint8_t channels[]={1,2,3,4,5,6,7,8,9,10,11,36,40,44,48};
    WifiScanRadio radio={.ready=radio_ready,.now_ms=now,.set_channel=tune,
        .receive=receive,.stop=stop,.connect=station_connect,.link=station_link,
        .connection_error=station_error,.poll_client=station_poll};
    STEP("scanner attach",wifi_scan_attach(&dev.scanner,&radio,channels,sizeof(channels)));
    dev.stage="experimental STA TX/RX ready"; dev.error=0;
    serial_write("RTL8822BE: firmware booted; experimental native TX/RX STA registered\n");
#undef STEP
    return;
fail:
    dev.error=rc;
    serial_write("RTL8822BE: initialization failed at "); serial_write(dev.stage);
    char n[20]; number(n,(uint32_t)-rc); serial_write(" error="); serial_write(n); serial_write("\n");
    cleanup();
}
bool rtw8822be_present(void) { return dev.present; }
void rtw8822be_status(char *out,size_t cap) {
    out[0]=0;
    append(out,"RTL8822BE: ",cap); append(out,dev.stage ? dev.stage : "not initialized",cap);
    if(dev.emulated) append(out," [QEMU laboratory model; passive RX only]",cap);
    if(dev.error) {
        char n[20]; append(out," error=",cap); number(n,(uint32_t)-dev.error); append(out,n,cap);
    }
    char n[20]; append(out,"\nRX frames=",cap); number(n,dev.received); append(out,n,cap);
    append(out," dropped=",cap); number(n,dev.dropped); append(out,n,cap);
    append(out,"\nRX DMA polls=",cap); number(n,dev.rx_dma_polls); append(out,n,cap);
    append(out," pending=",cap); number(n,dev.rx_pending); append(out,n,cap);
    append(out," bad_tag=",cap); number(n,dev.rx_invalid_tag); append(out,n,cap);
    append(out," parse_fail=",cap); number(n,dev.rx_invalid_packet); append(out,n,cap);
    append(out," c2h=",cap); number(n,dev.rx_c2h); append(out,n,cap);
    append(out,"\nRX index=",cap); hex(n,dev.rx_last_index); append(out,n,cap);
    append(out,"\nMAC CR=",cap);hex(n,dev.diag_cr);append(out,n,cap);
    append(out," RCR=",cap);hex(n,dev.diag_rcr);append(out,n,cap);
    append(out," PCICTRL=",cap);hex(n,dev.diag_pcictrl);append(out,n,cap);
    append(out," RXFLTMAP=",cap);hex(n,dev.diag_rxfilter);append(out,n,cap);
    append(out,"\nRXDMA status=",cap);hex(n,dev.diag_rxdma_status);append(out,n,cap);
    append(out," RXPSEL=",cap);hex(n,dev.diag_rf_rxpath);append(out,n,cap);
    append(out," RXIGI_A=",cap);hex(n,dev.diag_rxigi);append(out,n,cap);
    append(out," rp=",cap); number(n,dev.rp); append(out,n,cap);
    append(out," tag=",cap); number(n,dev.tag); append(out,n,cap);
    append(out,"\nScan active=",cap); number(n,dev.scanner.scanning ? 1u : 0u);append(out,n,cap);
    append(out," channel_idx=",cap);number(n,dev.scanner.channel_index);append(out,n,cap);
    append(out," found=",cap);number(n,dev.scanner.count);append(out,n,cap);
    append(out," PHY channel=",cap);number(n,dev.phy.hal.current_channel);append(out,n,cap);
    if(dev.scanner.error) {
        append(out," scan error=",cap); number(n,(uint32_t)-dev.scanner.error); append(out,n,cap);
    }
    append(out,"\nMLME stage=",cap);number(n,dev.client.stage);append(out,n,cap);
    append(out," last_fail_stage=",cap);number(n,dev.client.failed_stage);append(out,n,cap);
    append(out," error=",cap);append_error(out,cap,dev.client.error);
    append(out," attempts=",cap);number(n,dev.client.attempts);append(out,n,cap);
    append(out," mgmt_rx=",cap);number(n,dev.client.mgmt_rx);append(out,n,cap);
    append(out," foreign=",cap);number(n,dev.client.foreign_rx);append(out,n,cap);
    append(out," auth_rx=",cap);number(n,dev.client.auth_rx);append(out,n,cap);
    append(out," assoc_rx=",cap);number(n,dev.client.assoc_rx);append(out,n,cap);
    append(out,"\nWPA2 phase=",cap);number(n,dev.client.station.wpa.phase);append(out,n,cap);
    append(out," WPA2 error=",cap);append_error(out,cap,dev.client.station.wpa.error);
    append(out," link=",cap);number(n,wifi_client_link(&dev.client));append(out,n,cap);
    (void)read32(0x3b0,&dev.diag_tx_mgmt_idx);
    (void)read32(0x3a8,&dev.diag_tx_be_idx);
    (void)read32(0x210,&dev.diag_txdma_status);
    (void)read32(0x310,&dev.diag_tx_mgmt_base);
    (void)rd(NULL,0x380,2,&dev.diag_tx_mgmt_count);
    (void)rd(NULL,0x522,1,&dev.diag_tx_pause);
    (void)read32(0x20c,&dev.diag_tx_offset);
    (void)rd(NULL,0x232,2,&dev.diag_mgmt_avail_pages);
    (void)rd(NULL,0x10c,2,&dev.diag_txdma_mapping);
    (void)rd(NULL,0x41a,2,&dev.diag_tx_pkt_empty);
    (void)read32(0x508,&dev.diag_be_edca);
    (void)read32(0x604,&dev.diag_tcr);
    append(out,"\nTX MGMT index=",cap);hex(n,dev.diag_tx_mgmt_idx);append(out,n,cap);
    append(out," BE index=",cap);hex(n,dev.diag_tx_be_idx);append(out,n,cap);
    append(out," TXDMA status=",cap);hex(n,dev.diag_txdma_status);append(out,n,cap);
    append(out,"\nTX MGMT base=",cap);hex(n,dev.diag_tx_mgmt_base);append(out,n,cap);
    append(out," count=",cap);number(n,dev.diag_tx_mgmt_count & 0xfffu);append(out,n,cap);
    append(out," expected=",cap);hex(n,dev.txbd.bus+2*TX_RING_STRIDE);append(out,n,cap);
    append(out,"\nTX pause=",cap);hex(n,dev.diag_tx_pause);append(out,n,cap);
    append(out," offset_check=",cap);hex(n,dev.diag_tx_offset);append(out,n,cap);
    append(out,"\nTX pages HQ avail=",cap);number(n,dev.diag_mgmt_avail_pages);append(out,n,cap);
    append(out," mapping=",cap);hex(n,dev.diag_txdma_mapping);append(out,n,cap);
    append(out," txpkt_empty=",cap);hex(n,dev.diag_tx_pkt_empty);append(out,n,cap);
    append(out,"\nTX BE EDCA=",cap);hex(n,dev.diag_be_edca);append(out,n,cap);
    append(out," TCR=",cap);hex(n,dev.diag_tcr);append(out,n,cap);
    append(out,"\nTX ring MGMT wp=",cap);number(n,dev.txwp[2]);append(out,n,cap);
    append(out," BE wp=",cap);number(n,dev.txwp[4]);append(out,n,cap);
    append(out," client backpressure=",cap);number(n,dev.client.tx_backpressure);append(out,n,cap);
    append(out,"\nTX MGMT rekicks=",cap);number(n,dev.tx_mgmt_rekicks);append(out,n,cap);
    append(out," re-kick errors=",cap);number(n,dev.tx_mgmt_rekick_error);append(out,n,cap);
    append(out,"\nTX MGMT BD0=",cap);hex(n,dev.tx_mgmt_last_bd0);append(out,n,cap);
    append(out," BD1=",cap);hex(n,dev.tx_mgmt_last_bd1);append(out,n,cap);
    append(out," DESC0=",cap);hex(n,dev.tx_mgmt_last_desc0);append(out,n,cap);
    append(out,"\nTX frames=",cap);number(n,dev.tx_sent);append(out,n,cap);
    append(out," busy=",cap);number(n,dev.tx_busy);append(out,n,cap);
    append(out," errors=",cap);number(n,dev.tx_errors);append(out,n,cap);
    append(out,"\nExperimental STA TX/WPA2; no physical RF validation.\n",cap);
}
