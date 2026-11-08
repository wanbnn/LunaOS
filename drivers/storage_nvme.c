#include "storage.h"

/* NVMe NVM command-set, PCIe BAR0, 4-KiB pages, one polling I/O queue.
 * No MSI/APIC, namespace multipathing, metadata, >512B LBA or power states. */
#define NVME_MAX_CONTROLLERS 2
#define NVME_QUEUE_DEPTH 16u
#define NVME_CAP 0x00u
#define NVME_CC 0x14u
#define NVME_CSTS 0x1cu
#define NVME_AQA 0x24u
#define NVME_ASQ 0x28u
#define NVME_ACQ 0x30u
#define NVME_DOORBELLS 0x1000u
typedef struct {
    DrvDma sq,cq;
    uint16_t tail,head,cid;
    unsigned id;
    uint8_t phase;
} NvmeQueue;
typedef struct {
    DrvPci pci;
    DrvMmio regs;
    NvmeQueue admin,io;
    DrvDma page;
    uint64_t sectors;
    uint32_t stride;
    unsigned nsid;
    bool active;
} Nvme;
static Nvme controllers[NVME_MAX_CONTROLLERS];

static uint32_t r32(Nvme *n,uint32_t off) {
    uint32_t result=0;
    (void)drv_mmio_read(&n->regs,off,4,&result);
    return result;
}
static void w32(Nvme *n,uint32_t off,uint32_t value) {
    (void)drv_mmio_write(&n->regs,off,4,value);
}
static bool wait_rdy(Nvme *n,bool expected) {
    uint32_t start=drv_now_ms();
    unsigned polls = 0;
    while(!!(r32(n,NVME_CSTS)&1u)!=expected) {
        if(r32(n,NVME_CSTS)&2u)return false;
        if((uint32_t)(drv_now_ms()-start)>5000u)return false;
        storage_poll_wait(&polls);
    }
    return true;
}
static void release_queue(NvmeQueue *q) {
    if(q->cq.cpu)drv_dma_free(&q->cq);
    if(q->sq.cpu)drv_dma_free(&q->sq);
}
static int init_queue(NvmeQueue *q,unsigned id) {
    *q=(NvmeQueue){.id=id,.phase=1};
    int rc=drv_dma_alloc(4096,4096,UINT32_MAX,0,&q->sq);
    if(rc)return rc;
    rc=drv_dma_alloc(4096,4096,UINT32_MAX,0,&q->cq);
    if(rc){release_queue(q);return rc;}
    return 0;
}
static int cmd(Nvme *n,NvmeQueue *q,uint8_t opcode,uint32_t nsid,
               uint32_t prp,uint32_t cdw10,uint32_t cdw11,uint32_t cdw12) {
    if(!q->sq.cpu || !q->cq.cpu)return DRV_EINVAL;
    uint16_t cid=++q->cid;
    uint32_t *sq=(uint32_t *)((uint8_t *)q->sq.cpu+q->tail*64u);
    memset(sq,0,64);
    sq[0]=(uint32_t)opcode|((uint32_t)cid<<16);
    sq[1]=nsid;
    sq[6]=prp;
    sq[10]=cdw10;sq[11]=cdw11;sq[12]=cdw12;
    q->tail=(q->tail+1u)%NVME_QUEUE_DEPTH;
    drv_memory_barrier();
    w32(n,NVME_DOORBELLS+2u*q->id*n->stride,q->tail);
    uint32_t start=drv_now_ms();
    unsigned polls = 0;
    volatile uint32_t *cq=(volatile uint32_t *)((uint8_t *)q->cq.cpu+q->head*16u);
    uint32_t result;
    for(;;) {
        result=cq[3];
        if(((result>>16)&1u)==q->phase)
            break;
        if((uint32_t)(drv_now_ms()-start)>5000u) {
            n->active = false;
            drv_pci_master(&n->pci, false);
            return DRV_ETIMEDOUT;
        }
        storage_poll_wait(&polls);
    }
    if ((result & 0xffffu) != cid) {
        n->active = false;
        drv_pci_master(&n->pci, false);
        return DRV_EIO;
    }
    q->head++;
    if(q->head==NVME_QUEUE_DEPTH){q->head=0;q->phase^=1u;}
    drv_memory_barrier();
    w32(n,NVME_DOORBELLS+(2u*q->id+1u)*n->stride,q->head);
    return result >> 17 ? DRV_EIO : 0;
}
static int identify(Nvme *n,unsigned nsid,unsigned cns) {
    memset(n->page.cpu,0,4096);
    return cmd(n,&n->admin,6,nsid,n->page.bus,cns,0,0);
}
static int transfer(void *ctx,uint64_t lba,unsigned count,void *buffer,bool write) {
    Nvme *n=ctx;
    if(!n || !n->active || !count || count>STORAGE_MAX_TRANSFER ||
       lba>=n->sectors || count>n->sectors-lba)return DRV_EINVAL;
    uint8_t *data=buffer;
    while(count) {
        unsigned sectors=count>8?8:count;
        size_t bytes=sectors*512u;
        if(write)memcpy(n->page.cpu,data,bytes);
        int rc=cmd(n,&n->io,write?1:2,n->nsid,n->page.bus,
                   (uint32_t)lba,(uint32_t)(lba>>32),sectors-1u);
        if(rc)return rc;
        if(!write)memcpy(data,n->page.cpu,bytes);
        data+=bytes;lba+=sectors;count-=sectors;
    }
    return 0;
}
static int flush(void *ctx) {
    Nvme *n=ctx;
    if(!n || !n->active)return DRV_EINVAL;
    return cmd(n,&n->io,0,n->nsid,0,0,0,0);
}
static void cleanup(Nvme *n) {
    if(n->regs.base) {
        w32(n,NVME_CC,r32(n,NVME_CC)&~1u);
        (void)wait_rdy(n,false);
    }
    if(n->pci.cookie)drv_pci_master(&n->pci,false);
    if(n->page.cpu)drv_dma_free(&n->page);
    release_queue(&n->io);
    release_queue(&n->admin);
    if(n->regs.base)drv_mmio_unmap(&n->regs);
    if(n->pci.cookie)drv_pci_release(&n->pci);
    memset(n,0,sizeof(*n));
}
int storage_nvme_probe(uint8_t bus,uint8_t slot,uint8_t fn) {
    Nvme *n=0;
    for(unsigned i=0;i<NVME_MAX_CONTROLLERS;i++)
        if(!controllers[i].active && !controllers[i].pci.cookie) {n=&controllers[i];break;}
    if(!n)return DRV_ENOSPC;
    uint32_t id=pci_read(bus,slot,fn,0);
    *n=(Nvme){.pci={.bus=bus,.slot=slot,.function=fn,
                    .vendor=(uint16_t)id,.device=(uint16_t)(id>>16)}};
    int stage=1;
    /* Firmware may leave the PCI command BME enabled after boot. The
     * NVMe controller is now owned by LunaOS; turn it off before claim/reset. */
    uint32_t cmdreg=pci_read(bus,slot,fn,4);
    if(cmdreg&4u) {
        serial_write("STORAGE NVMe: disabling inherited PCI bus master\n");
        pci_write(bus,slot,fn,4,cmdreg&~4u);
        if(pci_read(bus,slot,fn,4)&4u)goto fail;
    }
    if(drv_pci_claim(&n->pci))goto fail;
    stage=2;
    if(drv_pci_map(&n->pci,0,&n->regs))goto fail;
    stage=3;
    if(n->regs.size<0x2000)goto fail;
    stage=4;
    if(drv_pci_enable(&n->pci,false,true)||
       drv_pci_master(&n->pci,true))goto fail;
    stage=5;
    uint64_t cap=(uint64_t)r32(n,NVME_CAP)|
                 ((uint64_t)r32(n,NVME_CAP+4u)<<32);
    if((cap&0xffffu)<NVME_QUEUE_DEPTH-1u ||
       ((cap>>48)&15u)!=0 || !((cap>>37)&1u))goto fail;
    stage=6;
    n->stride=4u<<(unsigned)((cap>>32)&15u);
    if(NVME_DOORBELLS+4u*n->stride>=n->regs.size)goto fail;
    w32(n,NVME_CC,r32(n,NVME_CC)&~1u);
    if(!wait_rdy(n,false))goto fail;
    stage=7;
    if(init_queue(&n->admin,0))goto fail;
    if(init_queue(&n->io,1))goto fail;
    if(drv_dma_alloc(4096,4096,UINT32_MAX,0,&n->page))goto fail;
    w32(n,NVME_AQA,((NVME_QUEUE_DEPTH-1u)<<16)|(NVME_QUEUE_DEPTH-1u));
    w32(n,NVME_ASQ,n->admin.sq.bus);w32(n,NVME_ASQ+4,0);
    w32(n,NVME_ACQ,n->admin.cq.bus);w32(n,NVME_ACQ+4,0);
    w32(n,NVME_CC,(6u<<16)|(4u<<20)|1u);
    if(!wait_rdy(n,true))goto fail;
    stage=8;
    if(identify(n,0,1))goto fail;  /* Identify Controller */
    const uint8_t *controller=n->page.cpu;
    uint32_t nn;
    memcpy(&nn,controller+516,4);
    if(!nn)goto fail;
    char model[41]={0};
    memcpy(model,controller+24,40);
    for(int i=39;i>=0 && (model[i]==' ' || !model[i]);i--)model[i]=0;
    n->nsid=1;
    stage=9;
    if(identify(n,n->nsid,0))goto fail;  /* Identify Namespace */
    const uint8_t *namespace_data=n->page.cpu;
    uint64_t nsze;
    memcpy(&nsze,namespace_data,8);
    uint8_t format=namespace_data[26]&15u;
    if(format>=16 || namespace_data[128u+4u*format+2u]!=9u || nsze<8192u)
        goto fail; /* Only 512-byte logical sectors, no metadata formats. */
    n->sectors=nsze;
    stage=10;
    /* Create I/O completion queue, then submission queue, physically contiguous. */
    if(cmd(n,&n->admin,5,0,n->io.cq.bus,
           ((NVME_QUEUE_DEPTH-1u)<<16)|1u,1u,0u))goto fail;
    if(cmd(n,&n->admin,1,0,n->io.sq.bus,
           ((NVME_QUEUE_DEPTH-1u)<<16)|1u,(1u<<16)|1u,0u))goto fail;
    n->active=true;
    StorageDevice dev={.ctx=n,.transfer=transfer,.flush=flush,
       .sectors=nsze,.logical_bytes=512,.transport=STORAGE_NVME,
       .readonly=false,.installable=true};
    strcpy(dev.model,"NVMe drive");
    if(model[0])memcpy(dev.model,model,sizeof(dev.model));
    if(storage_register(&dev)<0)goto fail;
    return 1;
fail:
    serial_write("STORAGE NVMe probe failed at stage ");
    char stage_text[16];number(stage_text,(uint32_t)stage);
    serial_write(stage_text);serial_write("\n");
    cleanup(n);
    return DRV_ENOTSUP;
}
