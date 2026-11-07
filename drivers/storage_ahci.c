#include "storage.h"

/* AHCI 1.3 SATA DMA backend. One polling command per port; 512-byte LBA48
 * only. No MSI, NCQ, power management, ATAPI or hotplug in this milestone. */
#define AHCI_HOSTS 2
#define AHCI_PORTS_PER_HOST 8
#define AHCI_GHC 0x04u
#define AHCI_PI 0x0cu
#define AHCI_CAP2 0x24u
#define AHCI_BOHC 0x28u
#define AHCI_PORT(p) (0x100u+(p)*0x80u)
#define PX_IS 0x10u
#define PX_CMD 0x18u
#define PX_TFD 0x20u
#define PX_SIG 0x24u
#define PX_SSTS 0x28u
#define PX_SERR 0x30u
#define PX_CI 0x38u
typedef struct AhciHost AhciHost;
typedef struct {
    AhciHost *host;
    unsigned port;
    uint64_t sectors;
    bool ready;
    DrvDma clb, fis, table, buffer;
} AhciDisk;
struct AhciHost {
    DrvPci pci;
    DrvMmio mmio;
    AhciDisk disk[AHCI_PORTS_PER_HOST];
    unsigned count;
    bool used;
};
static AhciHost hosts[AHCI_HOSTS];

static uint32_t rd(AhciHost *h,uint32_t off) {
    uint32_t value=0;
    (void)drv_mmio_read(&h->mmio,off,4,&value);
    return value;
}
static void wr(AhciHost *h,uint32_t off,uint32_t v) {
    (void)drv_mmio_write(&h->mmio,off,4,v);
}
static bool wait_reg(AhciHost *h,uint32_t off,uint32_t mask,uint32_t expected,
                     unsigned timeout_ms) {
    uint32_t start=drv_now_ms();
    while((rd(h,off)&mask)!=expected) {
        if((uint32_t)(drv_now_ms()-start)>=timeout_ms)return false;
        __asm__ volatile("pause");
    }
    return true;
}
static bool port_stop(AhciDisk *d) {
    AhciHost *h=d->host;uint32_t base=AHCI_PORT(d->port);
    uint32_t cmd=rd(h,base+PX_CMD);
    wr(h,base+PX_CMD,cmd & ~1u);
    if(!wait_reg(h,base+PX_CMD,1u<<15,0,1000))return false;
    wr(h,base+PX_CMD,rd(h,base+PX_CMD)&~(1u<<4));
    return wait_reg(h,base+PX_CMD,(1u<<14)|(1u<<15),0,1000);
}
static void port_free(AhciDisk *d) {
    if(d->buffer.cpu)drv_dma_free(&d->buffer);
    if(d->table.cpu)drv_dma_free(&d->table);
    if(d->fis.cpu)drv_dma_free(&d->fis);
    if(d->clb.cpu)drv_dma_free(&d->clb);
    d->ready=false;
}
static int port_memory(AhciDisk *d) {
    AhciHost *h=d->host;
    if(!port_stop(d))return DRV_ETIMEDOUT;
    int rc=drv_dma_alloc(1024,1024,UINT32_MAX,0,&d->clb);
    if(rc)return rc;
    rc=drv_dma_alloc(256,256,UINT32_MAX,0,&d->fis);
    if(rc)goto fail;
    rc=drv_dma_alloc(4096,128,UINT32_MAX,0,&d->table);
    if(rc)goto fail;
    rc=drv_dma_alloc(STORAGE_MAX_TRANSFER*512u,4096,UINT32_MAX,0,&d->buffer);
    if(rc)goto fail;
    uint32_t b=AHCI_PORT(d->port);
    wr(h,b+0,d->clb.bus);wr(h,b+4,0);
    wr(h,b+8,d->fis.bus);wr(h,b+12,0);
    wr(h,b+PX_IS,UINT32_MAX);wr(h,b+PX_SERR,UINT32_MAX);
    wr(h,b+PX_CMD,rd(h,b+PX_CMD)|(1u<<4));
    wr(h,b+PX_CMD,rd(h,b+PX_CMD)|1u);
    return 0;
fail:
    port_free(d);return rc;
}
static int issue(AhciDisk *d,uint8_t opcode,uint64_t lba,
                 unsigned sectors,bool write,unsigned bytes) {
    AhciHost *h=d->host;
    uint32_t base=AHCI_PORT(d->port);
    if(!d->clb.cpu || bytes>d->buffer.size ||
       (rd(h,base+PX_CMD)&(1u<<0))==0)return DRV_EINVAL;
    if(!wait_reg(h,base+PX_TFD,0x88u,0,1000))return DRV_ETIMEDOUT;
    memset(d->clb.cpu,0,1024);
    memset(d->table.cpu,0,4096);
    uint32_t *cl=(uint32_t *)d->clb.cpu;
    cl[0]=5u | (write ? 1u<<6 : 0u) | (bytes ? 1u<<16 : 0u);
    cl[2]=d->table.bus;
    uint8_t *cfis=(uint8_t *)d->table.cpu;
    cfis[0]=0x27;cfis[1]=0x80;cfis[2]=opcode;
    cfis[7]=0x40;
    for(unsigned i=0;i<6;i++)cfis[4u+i+(i>=3?1u:0u)]=(uint8_t)(lba>>(8u*i));
    cfis[12]=(uint8_t)sectors;cfis[13]=(uint8_t)(sectors>>8);
    if(bytes) {
        uint32_t *prdt=(uint32_t *)(cfis+0x80);
        prdt[0]=d->buffer.bus;
        prdt[3]=(bytes-1u)|(1u<<31);
    }
    wr(h,base+PX_IS,UINT32_MAX);
    drv_memory_barrier();
    wr(h,base+PX_CI,1);
    uint32_t start=drv_now_ms();
    for(;;) {
        if(rd(h,base+PX_IS)&(1u<<30))return DRV_EIO;
        if(!(rd(h,base+PX_CI)&1u))break;
        if((uint32_t)(drv_now_ms()-start)>3000)return DRV_ETIMEDOUT;
        __asm__ volatile("pause");
    }
    drv_memory_barrier();
    return (rd(h,base+PX_TFD)&1u)?DRV_EIO:0;
}
static int transfer(void *ctx,uint64_t lba,unsigned count,void *data,bool write) {
    AhciDisk *d=ctx;
    if(!d || !d->ready || !count || count>STORAGE_MAX_TRANSFER ||
       lba>=d->sectors || count>d->sectors-lba)return DRV_EINVAL;
    unsigned bytes=count*512u;
    if(write)memcpy(d->buffer.cpu,data,bytes);
    int rc=issue(d,write?0x35:0x25,lba,count,write,bytes);
    if(!rc && !write)memcpy(data,d->buffer.cpu,bytes);
    return rc;
}
static int flush(void *ctx) {
    AhciDisk *d=ctx;
    return d && d->ready?issue(d,0xea,0,0,false,0):DRV_EINVAL;
}
static bool identify(AhciDisk *d) {
    if(issue(d,0xec,0,0,false,512))return false;
    const uint16_t *id=d->buffer.cpu;
    if(!(id[83]&(1u<<10)))return false; /* Require LBA48 */
    if((id[106]&0xc000u)==0x4000u && (id[106]&(1u<<12)))return false;
    uint64_t sectors=(uint64_t)id[100]|((uint64_t)id[101]<<16)|
                     ((uint64_t)id[102]<<32)|((uint64_t)id[103]<<48);
    if(sectors<8192)return false;
    d->sectors=sectors;
    char model[41]={0};
    for(unsigned i=0;i<20;i++) {
        model[i*2]=(char)(id[27+i]>>8);
        model[i*2+1]=(char)(id[27+i]&255);
    }
    for(int j=39;j>=0 && (model[j]==' ' || !model[j]);j--)model[j]=0;
    d->ready=true;
    StorageDevice dev={.ctx=d,.transfer=transfer,.flush=flush,
        .sectors=sectors,.logical_bytes=512,.transport=STORAGE_AHCI,
        .readonly=false,.installable=true};
    if(!model[0])strcpy(model,"SATA drive");
    memcpy(dev.model,model,sizeof(dev.model));
    if(storage_register(&dev)<0){d->ready=false;return false;}
    return true;
}
int storage_ahci_probe(uint8_t bus,uint8_t slot,uint8_t fn) {
    AhciHost *h=0;
    for(unsigned i=0;i<AHCI_HOSTS;i++)if(!hosts[i].used){h=&hosts[i];break;}
    if(!h)return DRV_ENOSPC;
    uint32_t id=pci_read(bus,slot,fn,0);
    *h=(AhciHost){0};
    h->pci=(DrvPci){.bus=bus,.slot=slot,.function=fn,
        .vendor=(uint16_t)id,.device=(uint16_t)(id>>16)};
    int stage=1;
    if(drv_pci_claim(&h->pci))goto fail;
    stage=2;
    if(drv_pci_map(&h->pci,5,&h->mmio))goto fail;
    stage=3;
    if(h->mmio.size<0x1100)goto fail;
    stage=4;
    if(drv_pci_enable(&h->pci,false,true)||drv_pci_master(&h->pci,true))goto fail;
    stage=5;
    /* ACPI AHCI BIOS/OS ownership handoff, required by supported HBA. */
    if(rd(h,AHCI_CAP2)&1u) {
        wr(h,AHCI_BOHC,rd(h,AHCI_BOHC)|2u);
        if(!wait_reg(h,AHCI_BOHC,1u,0,1500))goto fail;
    }
    stage=6;
    wr(h,AHCI_GHC,(rd(h,AHCI_GHC)|(1u<<31))&~2u);
    uint32_t pi=rd(h,AHCI_PI);
    for(unsigned p=0;p<32 && h->count<AHCI_PORTS_PER_HOST;p++) {
        if(!(pi&(1u<<p)))continue;
        uint32_t off=AHCI_PORT(p);
        if(off+0x40>h->mmio.size)continue;
        uint32_t ssts=rd(h,off+PX_SSTS);
        if((ssts&0xfu)!=3 || ((ssts>>8)&0xfu)!=1 ||
           rd(h,off+PX_SIG)!=0x00000101u)continue;
        AhciDisk *d=&h->disk[h->count];
        *d=(AhciDisk){.host=h,.port=p};
        if(port_memory(d)) {
            serial_write("STORAGE AHCI: port DMA setup failed\n");
            port_free(d);continue;
        }
        if(!identify(d)) {
            serial_write("STORAGE AHCI: IDENTIFY failed\n");
            port_stop(d);port_free(d);continue;
        }
        h->count++;
    }
    if(h->count) {h->used=true;return (int)h->count;}
fail:
    serial_write("STORAGE AHCI probe failed at stage ");
    char stage_text[16];number(stage_text,(uint32_t)stage);
    serial_write(stage_text);serial_write("\n");
    if(h->pci.cookie) {
        drv_pci_master(&h->pci,false);
        if(h->mmio.base)drv_mmio_unmap(&h->mmio);
        drv_pci_release(&h->pci);
    }
    memset(h,0,sizeof(*h));
    return DRV_ENOTSUP;
}
