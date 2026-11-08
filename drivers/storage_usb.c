#include "storage.h"

/* EHCI high-speed root-port devices, USB Mass Storage SCSI/Bulk-Only LUN0.
 * No companion HC, hubs, UAS, hotplug or low/full-speed split transactions. */
#define USB_BYTES 16384u
typedef struct UsbHost UsbHost;
typedef struct {
    UsbHost *host;
    uint8_t address,interface,in,out,config,toggle_in,toggle_out;
    bool ready;
    uint64_t sectors;
    uint32_t tag;
} UsbDisk;
struct UsbHost {
    DrvPci pci;DrvMmio io;DrvDma schedule,buffer;
    unsigned op,count;bool used,dead,busy;
    UsbDisk disk[8];
};
static UsbHost hosts[2];
static uint32_t rd(UsbHost *h,unsigned off) {
    uint32_t v=0;drv_mmio_read(&h->io,off,4,&v);return v;
}
static void wr(UsbHost *h,unsigned off,uint32_t v){drv_mmio_write(&h->io,off,4,v);}
static bool wait_bits(UsbHost *h,unsigned off,uint32_t mask,uint32_t value,unsigned ms) {
    uint32_t start=drv_now_ms();
    unsigned polls = 0;
    do {
        if((rd(h,off)&mask)==value)return true;
        storage_poll_wait(&polls);
    } while((uint32_t)(drv_now_ms()-start)<ms);
    return false;
}
static uint32_t get32(const uint8_t *p){uint32_t v;memcpy(&v,p,4);return v;}
static void put32(uint8_t *p,unsigned off,uint32_t v){memcpy(p+off,&v,4);}
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static void putbe32(uint8_t *p,uint32_t v) {
    for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(24-8*i));
}
static void fatal(UsbHost *h) {
    h->dead=true;
    wr(h,h->op,rd(h,h->op)&~0x21u);
    drv_pci_master(&h->pci,false);
}
/* One qTD, at most four aligned data pages. Stop the async schedule before
 * touching any descriptor; keep all DMA allocations alive after a timeout. */
static int packet(UsbHost *h,unsigned address,unsigned ep,unsigned mps,
                  unsigned pid,unsigned toggle,void *data,unsigned length,
                  unsigned *actual,unsigned *next_toggle) {
    if(h->dead || length>USB_BYTES)return DRV_EIO;
    wr(h,h->op,rd(h,h->op)&~0x20u);
    if(!wait_bits(h,h->op+4,1u<<15,0,1000)){fatal(h);return DRV_ETIMEDOUT;}
    uint32_t *qh=h->schedule.cpu;
    uint32_t *td=(uint32_t *)((uint8_t *)h->schedule.cpu+128);
    memset(qh,0,96);memset(td,0,64);
    qh[0]=h->schedule.bus|2u;
    qh[1]=address|(ep<<8)|(2u<<12)|(1u<<14)|(1u<<15)|(mps<<16);
    qh[2]=1u<<30;qh[4]=h->schedule.bus+128;qh[5]=1;
    td[0]=1;td[1]=1;
    td[2]=(toggle<<31)|(length<<16)|(3u<<10)|(pid<<8)|0x80u;
    for(unsigned i=0;i<5;i++)td[3+i]=h->buffer.bus+i*4096;
    if(length && pid!=1)memcpy(h->buffer.cpu,data,length);
    drv_memory_barrier();
    wr(h,h->op+0x18,h->schedule.bus);
    wr(h,h->op,rd(h,h->op)|0x21u);
    volatile uint32_t *token=&td[2];uint32_t start=drv_now_ms();int rc=0;
    unsigned polls = 0;
    while(*token&0x80u) {
        if((uint32_t)(drv_now_ms()-start)>3000){rc=DRV_ETIMEDOUT;break;}
        storage_poll_wait(&polls);
    }
    wr(h,h->op,rd(h,h->op)&~0x20u);
    if(!wait_bits(h,h->op+4,1u<<15,0,1000)){fatal(h);return DRV_ETIMEDOUT;}
    drv_memory_barrier();
    uint32_t result=*token;
    if(rc){fatal(h);return rc;}
    if(result&0x7cu)return DRV_EIO;
    unsigned remaining=(result>>16)&0x7fffu;
    if(remaining>length)return DRV_EIO;
    unsigned bytes=length-remaining;
    if(actual)*actual=bytes;
    if(next_toggle)*next_toggle=(qh[6]>>31)&1u;
    if(pid==1 && bytes)memcpy(data,h->buffer.cpu,bytes);
    return 0;
}
static int control(UsbHost *h,unsigned address,uint8_t type,uint8_t req,
                   uint16_t value,uint16_t index,void *data,uint16_t length) {
    uint8_t setup[8]={type,req,(uint8_t)value,(uint8_t)(value>>8),
        (uint8_t)index,(uint8_t)(index>>8),(uint8_t)length,(uint8_t)(length>>8)};
    int rc=packet(h,address,0,64,2,0,setup,8,0,0);
    if(!rc && length) {
        unsigned actual=0;
        rc=packet(h,address,0,64,(type&0x80)?1:0,1,data,length,&actual,0);
        if(!rc && actual!=length)rc=DRV_EIO;
    }
    if(!rc)rc=packet(h,address,0,64,(type&0x80)?0:1,1,0,0,0,0);
    return rc;
}
static int bulk(UsbDisk *d,bool in,void *data,unsigned bytes) {
    unsigned actual=0,toggle=0;
    int rc=packet(d->host,d->address,in?d->in:d->out,512,in?1:0,
        in?d->toggle_in:d->toggle_out,data,bytes,&actual,&toggle);
    if(in)d->toggle_in=toggle;else d->toggle_out=toggle;
    return rc?rc:actual==bytes?0:DRV_EIO;
}
static void reset_bot(UsbDisk *d) {
    UsbHost *h=d->host;
    if(h->dead || control(h,d->address,0x21,0xff,0,d->interface,0,0) ||
       control(h,d->address,2,1,0,d->in|0x80,0,0) ||
       control(h,d->address,2,1,0,d->out,0,0))d->ready=false;
    d->toggle_in=d->toggle_out=0;
}
static int scsi(UsbDisk *d,const uint8_t *cdb,unsigned cdb_len,
                void *data,unsigned bytes,bool write) {
    if(d->host->dead || cdb_len>16 || !cdb_len)return DRV_EIO;
    uint8_t cbw[31]={0},csw[13]={0};
    put32(cbw,0,0x43425355u);put32(cbw,4,++d->tag);put32(cbw,8,bytes);
    cbw[12]=write?0:0x80;cbw[14]=cdb_len;memcpy(cbw+15,cdb,cdb_len);
    int rc=bulk(d,false,cbw,sizeof(cbw));
    if(!rc && bytes)rc=bulk(d,!write,data,bytes);
    if(!rc)rc=bulk(d,true,csw,sizeof(csw));
    if(rc || get32(csw)!=0x53425355u || get32(csw+4)!=d->tag ||
       csw[12]>1 || get32(csw+8)>bytes) {
        reset_bot(d);return rc?rc:DRV_EIO;
    }
    return csw[12] || get32(csw+8)?DRV_EIO:0;
}
static bool enter(UsbHost *h) {
    uint32_t flags=drv_irq_save();bool ok=!h->busy;
    if(ok)h->busy=true;
    drv_irq_restore(flags);return ok;
}
static void leave(UsbHost *h) {
    uint32_t flags=drv_irq_save();h->busy=false;drv_irq_restore(flags);
}
static int transfer(void *ctx,uint64_t lba,unsigned n,void *data,bool write) {
    UsbDisk *d=ctx;
    if(!d->ready || !n || n>STORAGE_MAX_TRANSFER || lba>=d->sectors ||
       n>d->sectors-lba || lba+n>UINT32_MAX)return DRV_EINVAL;
    if(!enter(d->host))return DRV_EBUSY;
    int rc=0;
    while(n) {
        unsigned count=n>USB_BYTES/512?USB_BYTES/512:n;
        uint8_t cdb[10]={0};cdb[0]=write?0x2a:0x28;
        putbe32(cdb+2,(uint32_t)lba);cdb[7]=count>>8;cdb[8]=count;
        rc=scsi(d,cdb,10,data,count*512u,write);
        if(rc)break;
        n-=count;lba+=count;data=(uint8_t *)data+count*512u;
    }
    leave(d->host);return rc;
}
static int sync_disk(UsbDisk *d) {
    uint8_t cdb[10]={0x35};return scsi(d,cdb,10,0,0,false);
}
static int flush(void *ctx) {
    UsbDisk *d=ctx;
    if(!d->ready)return DRV_EIO;
    if(!enter(d->host))return DRV_EBUSY;
    int rc=sync_disk(d);leave(d->host);return rc;
}
static bool enumerate(UsbDisk *d) {
    UsbHost *h=d->host;unsigned address=d->address;
    uint8_t desc[512]={0};
    if(control(h,0,0x80,6,0x0100,0,desc,8) || desc[7]!=64 ||
       control(h,0,0,5,address,0,0,0))return false;
    drv_sleep_ms(2);
    if(control(h,address,0x80,6,0x0100,0,desc,18) || desc[0]!=18 ||
       control(h,address,0x80,6,0x0200,0,desc,9))return false;
    unsigned total=desc[2]|((unsigned)desc[3]<<8);
    if(total<9 || total>sizeof(desc))return false;
    if(control(h,address,0x80,6,0x0200,0,desc,total))return false;
    d->config=desc[5];bool matching=false,found=false;
    for(unsigned off=9;off+2<=total;) {
        unsigned len=desc[off];
        if(len<2 || off+len>total)return false;
        uint8_t *p=desc+off;
        if(p[1]==4) {
            if(matching && d->in && d->out){found=true;break;}
            matching=len>=9 && !p[3] && p[5]==8 && p[6]==6 && p[7]==0x50;
            d->in=d->out=0;
            if(matching)d->interface=p[2];
        } else if(matching && p[1]==5 && len>=7 && (p[3]&3u)==2) {
            unsigned mps=p[4]|((unsigned)p[5]<<8);
            if(mps!=512 || !(p[2]&15u))return false;
            if(p[2]&0x80)d->in=p[2]&15u;else d->out=p[2]&15u;
        }
        off+=len;
    }
    if(!found && !(matching && d->in && d->out))return false;
    if(control(h,address,0,9,d->config,0,0,0))return false;
    uint8_t inquiry[36],cdb[16]={0x12,0,0,0,36};
    if(scsi(d,cdb,6,inquiry,sizeof(inquiry),false) || (inquiry[0]&31u)!=0)return false;
    bool available=false;
    for(unsigned i=0;i<8;i++) {
        uint8_t tur[6]={0},sense[18],request[6]={3,0,0,0,18};
        if(!scsi(d,tur,6,0,0,false)){available=true;break;}
        if(scsi(d,request,6,sense,sizeof(sense),false))return false;
        drv_sleep_ms(20);
    }
    if(!available)return false;
    uint8_t cap[8],capacity[10]={0x25};
    if(scsi(d,capacity,10,cap,8,false) || be32(cap+4)!=512 || be32(cap)==UINT32_MAX)
        return false;
    d->sectors=(uint64_t)be32(cap)+1;
    uint8_t mode[4],mode_cdb[6]={0x1a,0,0x3f,0,4};
    if(scsi(d,mode_cdb,6,mode,4,false))return false;
    bool ro=(mode[2]&0x80u)!=0;
    bool sync_ok=ro || sync_disk(d)==0;
    d->ready=true;
    StorageDevice disk={.ctx=d,.transfer=transfer,.flush=flush,.sectors=d->sectors,
        .logical_bytes=512,.transport=STORAGE_USB,.removable=true,
        .readonly=ro,.installable=!ro && sync_ok};
    memcpy(disk.model,inquiry+8,8);disk.model[8]=' ';memcpy(disk.model+9,inquiry+16,16);
    disk.model[25]=0;
    for(int i=24;i>=0 && disk.model[i]==' ';i--)disk.model[i]=0;
    if(!disk.model[0])strcpy(disk.model,"USB Mass Storage");
    if(storage_register(&disk)<0){d->ready=false;return false;}
    return true;
}
int storage_usb_probe(uint8_t bus,uint8_t slot,uint8_t fn) {
    if((pci_read(bus,slot,fn,8)>>8)!=0x0c0320u)return DRV_ENOTSUP;
    UsbHost *h=0;
    for(unsigned i=0;i<2;i++)if(!hosts[i].used){h=&hosts[i];break;}
    if(!h)return DRV_ENOSPC;
    *h=(UsbHost){0};h->pci=(DrvPci){.bus=bus,.slot=slot,.function=fn,
        .vendor=(uint16_t)pci_read(bus,slot,fn,0),.device=pci_read(bus,slot,fn,0)>>16};
    uint32_t cmd=pci_read(bus,slot,fn,4);pci_write(bus,slot,fn,4,cmd&~4u);
    if(drv_pci_claim(&h->pci))return DRV_EBUSY;
    if(drv_pci_map(&h->pci,0,&h->io) || h->io.size<0x80 ||
       drv_pci_enable(&h->pci,false,true))goto fail;
    h->op=rd(h,0)&255u;
    unsigned ports=rd(h,4)&15u;
    if(h->op<0x10 || h->op+0x44+ports*4>h->io.size)goto fail;
    /* EHCI extended legacy capability: bounded PCI capability walk. */
    unsigned eecp=(rd(h,8)>>8)&255u;
    for(unsigned n=0;eecp && n<48;n++) {
        if(eecp<0x40 || eecp>0xf8 || (eecp&3))goto fail;
        uint32_t cap=pci_read(bus,slot,fn,eecp);
        if((cap&255u)==1) {
            pci_write(bus,slot,fn,eecp,cap|(1u<<24));
            uint32_t start=drv_now_ms();
            while(pci_read(bus,slot,fn,eecp)&(1u<<16))
                if((uint32_t)(drv_now_ms()-start)>2000)goto fail;
            pci_write(bus,slot,fn,eecp+4,0);break;
        }
        eecp=(cap>>8)&255u;
        if(n==47 && eecp)goto fail;
    }
    wr(h,h->op,0);
    if(!wait_bits(h,h->op+4,1u<<12,1u<<12,1000))goto fail;
    wr(h,h->op,2);
    if(!wait_bits(h,h->op,2,0,1000))goto fail;
    wr(h,h->op+8,0);wr(h,h->op+0x10,0);
    if(drv_dma_alloc(4096,4096,UINT32_MAX,0,&h->schedule) ||
       drv_dma_alloc(USB_BYTES,4096,UINT32_MAX,0,&h->buffer) ||
       drv_pci_master(&h->pci,true))goto fail;
    wr(h,h->op,1);wr(h,h->op+0x40,1);
    for(unsigned p=0;p<ports && p<8 && !h->dead;p++) {
        unsigned off=h->op+0x44+p*4;
        uint32_t port=rd(h,off);
        if(rd(h,4)&16u)wr(h,off,(port&~0x2au)|(1u<<12));
    }
    drv_sleep_ms(25);
    for(unsigned p=0;p<ports && p<8 && !h->dead;p++) {
        unsigned off=h->op+0x44+p*4;
        uint32_t port=rd(h,off);
        if(!(port&1u))continue;
        wr(h,off,(port&~0x202eu)|(1u<<8));drv_sleep_ms(50);
        wr(h,off,rd(h,off)&~0x12au);
        if(!wait_bits(h,off,1u<<8,0,1000) || !(rd(h,off)&4u))continue;
        drv_sleep_ms(10);
        UsbDisk *d=&h->disk[h->count];
        *d=(UsbDisk){.host=h,.address=p+1};
        if(enumerate(d))h->count++;
        else {
            /* A failed enumeration may leave address zero live; disable
             * this port before trying another root-port device. */
            wr(h,off,rd(h,off)&~0x2eu);
        }
    }
    if(h->count){h->used=true;return h->count;}
fail:
    if(h->io.base)wr(h,h->op,0);
    if(h->pci.cookie && drv_pci_master(&h->pci,false)) {
        h->used=h->dead=true;return DRV_EIO; /* quarantine DMA */
    }
    if(h->buffer.cpu)drv_dma_free(&h->buffer);
    if(h->schedule.cpu)drv_dma_free(&h->schedule);
    if(h->io.base)drv_mmio_unmap(&h->io);
    drv_pci_release(&h->pci);memset(h,0,sizeof(*h));return DRV_ENOTSUP;
}
