#include "storage.h"

/* Legacy MegaRAID MFI, SAS1078/2108 only. Access existing logical volumes;
 * no array creation, physical-drive writes, rebuild or metadata changes. */
typedef struct RaidHost RaidHost;
typedef struct {RaidHost *host;uint8_t target;uint64_t sectors;} RaidDisk;
struct RaidHost {
    DrvPci pci;DrvMmio io;DrvDma frame,buffer,queue;
    RaidDisk disks[8];
    bool used,dead,busy;unsigned count;
};
static RaidHost hosts[2];
static uint32_t rd(RaidHost *h,unsigned off) {
    uint32_t v=0;drv_mmio_read(&h->io,off,4,&v);return v;
}
static void wr(RaidHost *h,unsigned off,uint32_t v){drv_mmio_write(&h->io,off,4,v);}
static void put32(uint8_t *p,unsigned off,uint32_t v){memcpy(p+off,&v,4);}
static uint32_t get32(const uint8_t *p){uint32_t v;memcpy(&v,p,4);return v;}
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static uint8_t *frame(RaidHost *h,unsigned cmd,unsigned dir,unsigned length) {
    uint8_t *f=h->frame.cpu;memset(f,0,128);
    f[0]=cmd;f[2]=0xff;f[16]=1u|dir;f[18]=10;
    put32(f,20,length);return f;
}
static void sgl(RaidHost *h,unsigned off,unsigned bytes) {
    uint8_t *f=h->frame.cpu;f[7]=1;
    put32(f,off,h->buffer.bus);put32(f,off+4,bytes);
}
static int issue(RaidHost *h) {
    if(h->dead)return DRV_EIO;
    volatile uint8_t *f=h->frame.cpu;
    drv_memory_barrier();wr(h,0x40,h->frame.bus);
    uint32_t start=drv_now_ms();
    unsigned polls = 0;
    while(f[2]==0xff) {
        if((uint32_t)(drv_now_ms()-start)>10000) {
            /* Never reuse a command or DMA buffer after an uncertain completion. */
            h->dead=true;drv_pci_master(&h->pci,false);return DRV_ETIMEDOUT;
        }
        storage_poll_wait(&polls);
    }
    drv_memory_barrier();return f[2] || f[3]?DRV_EIO:0;
}
static int dcmd(RaidHost *h,uint32_t opcode,unsigned bytes,unsigned mailbox) {
    uint8_t *f=frame(h,5,bytes?16:0,bytes);
    put32(f,24,opcode);f[28]=mailbox;
    if(bytes){memset(h->buffer.cpu,0,bytes);sgl(h,40,bytes);}
    return issue(h);
}
static bool enter(RaidHost *h) {
    uint32_t flags=drv_irq_save();bool ok=!h->busy;
    if(ok)h->busy=true;
    drv_irq_restore(flags);return ok;
}
static void leave(RaidHost *h) {
    uint32_t flags=drv_irq_save();h->busy=false;drv_irq_restore(flags);
}
static int transfer(void *ctx,uint64_t lba,unsigned n,void *buffer,bool write) {
    RaidDisk *d=ctx;RaidHost *h=d->host;
    if(!n || n>STORAGE_MAX_TRANSFER || lba>=d->sectors || n>d->sectors-lba)
        return DRV_EINVAL;
    if(!enter(h))return DRV_EBUSY;
    unsigned bytes=n*512u;
    uint8_t *f=frame(h,write?2:1,write?8:16,n);
    f[4]=d->target;put32(f,32,(uint32_t)lba);put32(f,36,(uint32_t)(lba>>32));
    sgl(h,40,bytes);
    if(write)memcpy(h->buffer.cpu,buffer,bytes);
    int rc=issue(h);
    if(!rc && !write)memcpy(buffer,h->buffer.cpu,bytes);
    leave(h);return rc;
}
static int flush(void *ctx) {
    RaidHost *h=((RaidDisk *)ctx)->host;
    if(!enter(h))return DRV_EBUSY;
    int rc=dcmd(h,0x01101000u,0,3);leave(h);return rc;
}
static uint64_t capacity(RaidHost *h,unsigned target,bool *readonly) {
    uint8_t *f=frame(h,3,16,8);f[4]=target;f[6]=10;f[32]=0x25;
    memset(h->buffer.cpu,0,8);sgl(h,48,8);
    if(issue(h))return 0;
    const uint8_t *p=h->buffer.cpu;
    uint32_t last=be32(p);
    if(be32(p+4)!=512 || last==UINT32_MAX)return 0;
    uint64_t sectors=(uint64_t)last+1;
    /* Logical volumes may be exported read-only by firmware. Check the
     * SCSI write-protect bit before offering destructive installation. */
    f=frame(h,3,16,4);f[4]=target;f[6]=6;
    f[32]=0x1a;f[34]=0x3f;f[36]=4;
    memset(h->buffer.cpu,0,4);sgl(h,48,4);
    if(issue(h))return 0;
    *readonly=(((const uint8_t *)h->buffer.cpu)[2]&0x80u)!=0;
    return sectors;
}
int storage_raid_probe(uint8_t bus,uint8_t slot,uint8_t fn) {
    uint32_t id=pci_read(bus,slot,fn,0);
    if((uint16_t)id!=0x1000 || ((id>>16)!=0x0060 && (id>>16)!=0x0079))
        return DRV_ENOTSUP;
    RaidHost *h=0;
    for(unsigned i=0;i<2;i++)if(!hosts[i].used){h=&hosts[i];break;}
    if(!h)return DRV_ENOSPC;
    *h=(RaidHost){0};h->pci=(DrvPci){.bus=bus,.slot=slot,.function=fn,
        .vendor=(uint16_t)pci_read(bus,slot,fn,0),.device=pci_read(bus,slot,fn,0)>>16};
    uint32_t cmd=pci_read(bus,slot,fn,4);
    pci_write(bus,slot,fn,4,cmd&~4u);
    if(drv_pci_claim(&h->pci))return DRV_EBUSY;
    if(drv_pci_map(&h->pci,(id>>16)==0x0060?0:1,&h->io) ||
       h->io.size<0xc8 || drv_pci_enable(&h->pci,false,true))goto fail;
    wr(h,0x34,UINT32_MAX); /* mask interrupts before queue initialization */
    uint32_t start=drv_now_ms();
    for(;;) {
        uint32_t state=rd(h,0xb0)&0xf0000000u;
        if(state==0xb0000000u)break;
        if(state==0x60000000u)wr(h,0x20,8);
        else if(state==0x90000000u)wr(h,0x20,0x10);
        else if(state==0xc0000000u)wr(h,0x20,3);
        else if(state==0xf0000000u)goto fail;
        if((uint32_t)(drv_now_ms()-start)>10000)goto fail;
    }
    if(drv_dma_alloc(128,64,UINT32_MAX,0,&h->frame) ||
       drv_dma_alloc(STORAGE_MAX_TRANSFER*512,4096,UINT32_MAX,0,&h->buffer) ||
       drv_dma_alloc(4096,64,UINT32_MAX,0,&h->queue))goto fail;
    memset(h->queue.cpu,0,h->queue.size);
    uint8_t *q=h->queue.cpu;
    /* Two reply entries, producer and consumer are always zero in polling
     * DONT_POST mode. Keep the queue alive for the lifetime of the host. */
    put32(q,4,2);put32(q,8,h->queue.bus+64);
    put32(q,16,h->queue.bus+128);put32(q,24,h->queue.bus+132);
    uint8_t *f=frame(h,0,0,0);put32(f,24,h->queue.bus);
    if(drv_pci_master(&h->pci,true) || issue(h))goto fail;
    if(dcmd(h,0x03010000u,8+64*16,0))goto fail;
    uint8_t list[8+64*16];memcpy(list,h->buffer.cpu,sizeof(list));
    unsigned count=get32(list);
    if(count>64)goto fail;
    for(unsigned i=0;i<count && h->count<8;i++) {
        const uint8_t *e=list+8+i*16;
        if(e[4]!=3 && e[4]!=2)continue; /* optimal or degraded, not offline */
        bool readonly=true;
        uint64_t sectors=capacity(h,e[0],&readonly);
        if(!sectors)continue;
        RaidDisk *disk=&h->disks[h->count];
        *disk=(RaidDisk){.host=h,.target=e[0],.sectors=sectors};
        StorageDevice d={.ctx=disk,.transfer=transfer,.flush=flush,
            .sectors=sectors,.logical_bytes=512,.transport=STORAGE_RAID,
            .readonly=readonly,.installable=!readonly};
        strcpy(d.model,"MegaRAID MFI logical volume");
        if(storage_register(&d)<0)break;
        h->count++;
    }
    if(h->count){h->used=true;return h->count;}
fail:
    if(drv_pci_master(&h->pci,false)) {
        h->used=h->dead=true;return DRV_EIO; /* quarantine uncertain DMA */
    }
    if(h->queue.cpu)drv_dma_free(&h->queue);
    if(h->buffer.cpu)drv_dma_free(&h->buffer);
    if(h->frame.cpu)drv_dma_free(&h->frame);
    if(h->io.base)drv_mmio_unmap(&h->io);
    drv_pci_release(&h->pci);memset(h,0,sizeof(*h));return DRV_ENOTSUP;
}
