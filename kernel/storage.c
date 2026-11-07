#include "storage.h"

typedef struct {
    bool present, busy;
    StorageDevice dev;
    DiskInfo info;
} Stored;
static Stored native[STORAGE_NATIVE_SLOTS];
static StorageController controllers[STORAGE_CONTROLLER_MAX];
static unsigned controller_count;

static const char *transport_name(unsigned t) {
    switch(t) {
    case STORAGE_ATA: return "IDE";
    case STORAGE_AHCI: return "SATA/AHCI";
    case STORAGE_NVME: return "NVMe";
    case STORAGE_USB: return "USB";
    case STORAGE_EMMC: return "eMMC/SDHCI";
    case STORAGE_RAID: return "RAID";
    default: return "unknown";
    }
}
void storage_reset(void) {
    memset(native,0,sizeof(native));
    memset(controllers,0,sizeof(controllers));
    controller_count=0;
}
int storage_register(const StorageDevice *d) {
    if (!d || !d->transfer || d->logical_bytes != 512 || !d->sectors ||
        !d->model[0] || !strchr(d->model,0) || d->sectors < 1 ||
        (d->installable && d->readonly) || d->transport < STORAGE_AHCI ||
        d->transport > STORAGE_RAID)
        return DRV_EINVAL;
    /* The old installer and LunaFS address sectors via uint32_t. Exclude
     * unsafe unrepresentable targets instead of silently wrapping offsets. */
    for(unsigned i=0;i<STORAGE_NATIVE_SLOTS;i++)if(!native[i].present) {
        Stored *e=&native[i];
        e->dev=*d;
        e->info=(DiskInfo){.present=true,.supported=d->installable&&!d->readonly,
                           .optical=false,.index=(uint8_t)(STORAGE_NATIVE_BASE+i),
                           .multiple=1,.sectors=d->sectors};
        memcpy(e->info.model,d->model,sizeof(e->info.model));
        e->present=true;
        return (int)(STORAGE_NATIVE_BASE+i);
    }
    return DRV_ENOSPC;
}
const DiskInfo *storage_disk_info(unsigned index) {
    if(index<STORAGE_NATIVE_BASE || index>=STORAGE_DEVICE_COUNT)return 0;
    Stored *e=&native[index-STORAGE_NATIVE_BASE];
    return e->present?&e->info:0;
}
unsigned storage_device_count(void){return STORAGE_DEVICE_COUNT;}
int storage_transfer(unsigned index,uint32_t lba,unsigned sectors,void *buffer,bool write) {
    if(index<STORAGE_NATIVE_BASE || index>=STORAGE_DEVICE_COUNT ||
       !sectors || sectors>STORAGE_MAX_TRANSFER || !buffer)return DRV_EINVAL;
    Stored *e=&native[index-STORAGE_NATIVE_BASE];
    if(!e->present || (write && (e->dev.readonly || !e->dev.installable)))
        return DRV_ENOTSUP;
    if((uint64_t)lba+sectors>e->dev.sectors) return DRV_EINVAL;
    uint32_t flags=drv_irq_save();
    if(e->busy){drv_irq_restore(flags);return DRV_EBUSY;}
    e->busy=true;drv_irq_restore(flags);
    int result=e->dev.transfer(e->dev.ctx,lba,sectors,buffer,write);
    flags=drv_irq_save(); e->busy=false; drv_irq_restore(flags);
    return result;
}
int storage_flush(unsigned index) {
    if(index<STORAGE_NATIVE_BASE || index>=STORAGE_DEVICE_COUNT)return DRV_EINVAL;
    Stored *e=&native[index-STORAGE_NATIVE_BASE];
    if(!e->present || e->dev.readonly || !e->dev.installable)return DRV_ENOTSUP;
    uint32_t flags=drv_irq_save();
    if(e->busy){drv_irq_restore(flags);return DRV_EBUSY;}
    e->busy=true;drv_irq_restore(flags);
    int rc=e->dev.flush?e->dev.flush(e->dev.ctx):DRV_ENOTSUP;
    flags=drv_irq_save();e->busy=false;drv_irq_restore(flags);
    return rc;
}
unsigned storage_controller_count(void){return controller_count;}
const StorageController *storage_controller(unsigned i) {
    return i<controller_count?controllers+i:0;
}
static bool pci_present(uint32_t id) {
    return (id&0xffffu) && (id&0xffffu)!=0xffffu;
}
void storage_pci_init(void) {
    /* A controller is not a drive. USB mass storage also requires a HCI,
     * hub, enumeration, transport and SCSI layer. RAID needs vendor protocol. */
    controller_count=0;
    for(unsigned bus=0;bus<256;bus++)for(unsigned slot=0;slot<32;slot++) {
        uint32_t first=pci_read(bus,slot,0,0);
        if(!pci_present(first))continue;
        unsigned fns=(pci_read(bus,slot,0,0xc)&0x800000u)?8:1;
        for(unsigned fn=0;fn<fns;fn++) {
            uint32_t id=fn?pci_read(bus,slot,fn,0):first;
            if(!pci_present(id))continue;
            uint32_t cls=pci_read(bus,slot,fn,8)>>8;
            unsigned base=(cls>>16)&255u, sub=(cls>>8)&255u, pi=cls&255u;
            unsigned kind=0;
            if(base==1 && sub==6 && pi==1)kind=STORAGE_AHCI;
            else if(base==1 && sub==8 && pi==2)kind=STORAGE_NVME;
            else if(base==0x0c && sub==3)kind=STORAGE_USB;
            else if(base==8 && sub==5)kind=STORAGE_EMMC;
            else if(base==1 && sub==4)kind=STORAGE_RAID;
            if(!kind || controller_count==STORAGE_CONTROLLER_MAX)continue;
            StorageController *c=&controllers[controller_count++];
            *c=(StorageController){.bus=(uint8_t)bus,.slot=(uint8_t)slot,
                .function=(uint8_t)fn,.vendor=(uint16_t)id,.device=(uint16_t)(id>>16),
                .transport=(uint8_t)kind};
            int rc=DRV_ENOTSUP;
            if(kind==STORAGE_AHCI)rc=storage_ahci_probe(bus,slot,fn);
            if(kind==STORAGE_NVME)rc=storage_nvme_probe(bus,slot,fn);
            c->initialized=rc>0;
            const char *reason=rc>0?"driver ready":rc==DRV_ENOTSUP?
              "controller detected; data-path driver not yet available":
              "controller found; initialization failed/unsupported";
            strcpy(c->reason,reason);
        }
    }
}
static void append_number(char *out,size_t cap,uint32_t n) {
    char s[20];number(s,n);append(out,s,cap);
}
void storage_status(char *out,size_t cap) {
    if(!out || !cap)return;
    out[0]=0;
    append(out,"Storage: IDE, SATA/AHCI, NVMe + controller inventory\n",cap);
    for(unsigned i=0;i<4;i++) {
        const DiskInfo *d=disk_info(i);
        if(!d || !d->present)continue;
        append(out,"IDE",cap);append_number(out,cap,i);
        append(out,": ",cap);append(out,d->model,cap);append(out,"\n",cap);
    }
    for(unsigned i=STORAGE_NATIVE_BASE;i<STORAGE_DEVICE_COUNT;i++) {
        const DiskInfo *d=storage_disk_info(i);
        if(!d)continue;
        const Stored *e=&native[i-STORAGE_NATIVE_BASE];
        append(out,"disk",cap);append_number(out,cap,i);
        append(out," ",cap);append(out,transport_name(e->dev.transport),cap);
        append(out," ",cap);append(out,d->model,cap);
        append(out,e->info.supported?" [installable]\n":" [read-only/unsupported]\n",cap);
    }
    for(unsigned i=0;i<controller_count;i++) {
        const StorageController *c=&controllers[i];
        append(out,transport_name(c->transport),cap);append(out," PCI ",cap);
        append_number(out,cap,c->bus);append(out,":",cap);
        append_number(out,cap,c->slot);append(out,".",cap);
        append_number(out,cap,c->function);append(out,": ",cap);
        append(out,c->reason,cap);append(out,"\n",cap);
    }
}
