#include "storage.h"
#include "storage_sdhci_policy.h"

/* SDHCI v2/v3, 1-bit default-speed PIO. SDHC/SDXC and sector-addressed
 * MMC/eMMC user area only; never changes boot partitions or RPMB. */
typedef struct {
    DrvPci pci;
    DrvMmio io;
    uint32_t rca;
    unsigned phase, last_cmd;
    uint32_t last_status;
    bool embedded, platform, platform_embedded;
    uint32_t physical;
    uint64_t sectors;
    bool used, ready, mmc, cache, write_protected;
} SdHost;
static SdHost hosts[4];
static char last_reason[160];
void storage_sdhci_reason(char *out,size_t cap) {
    if(out && cap){out[0]=0;append(out,last_reason,cap);}
}
static uint32_t rd(SdHost *h,unsigned off,unsigned width) {
    uint32_t v=0;drv_mmio_read(&h->io,off,width,&v);return v;
}
static void wr(SdHost *h,unsigned off,unsigned width,uint32_t v) {
    drv_mmio_write(&h->io,off,width,v);
}
static bool wait_bits(SdHost *h,unsigned off,unsigned width,uint32_t mask,
                      uint32_t value,unsigned ms) {
    uint32_t start=drv_now_ms();
    unsigned polls = 0;
    do {
        if((rd(h,off,width)&mask)==value)return true;
        storage_poll_wait(&polls);
    } while((uint32_t)(drv_now_ms()-start)<ms);
    return false;
}
static int status(SdHost *h,uint32_t wanted) {
    uint32_t start=drv_now_ms();
    unsigned polls = 0;
    do {
        uint32_t v=rd(h,0x30,4);
        if(v&0xffff8000u)return DRV_EIO;
        if(v&wanted){wr(h,0x30,4,wanted);return 0;}
        storage_poll_wait(&polls);
    } while((uint32_t)(drv_now_ms()-start)<2000);
    return DRV_ETIMEDOUT;
}
static void recover(SdHost *h) {
    wr(h,0x2f,1,6);
    if(!wait_bits(h,0x2f,1,6,0,100))h->ready=false;
    wr(h,0x30,4,UINT32_MAX);
}
static int command(SdHost *h,unsigned cmd,uint32_t arg,unsigned response,bool data) {
    h->last_cmd=cmd;
    if(!wait_bits(h,0x24,4,data?3u:1u,0,1000))return DRV_ETIMEDOUT;
    wr(h,0x30,4,UINT32_MAX);
    wr(h,0x08,4,arg);
    /* R2/R1 CRC+index checks, R3 has neither. */
    unsigned flags=response;
    if(response && cmd!=1 && cmd!=41)flags|=8u;
    if(response!=1 && response && cmd!=1 && cmd!=41)flags|=16u;
    if(data)flags|=32u;
    wr(h,0x0e,2,(cmd<<8)|flags);
    int rc=status(h,1);
    h->last_status=rd(h,0x30,4);
    if(!rc && response==3)rc=status(h,2);
    if(rc) {
        char text[16];serial_write("STORAGE SDHCI command ");number(text,cmd);
        serial_write(text);serial_write(" failed, status ");hex(text,rd(h,0x30,4));
        serial_write(text);serial_write("\n");recover(h);
    }
    return rc;
}
static int r1(SdHost *h,unsigned cmd,uint32_t arg,unsigned response,bool data) {
    int rc=command(h,cmd,arg,response,data);
    /* R1 error bits: excludes CARD_IS_LOCKED/READY_FOR_DATA/current state. */
    if(!rc && (rd(h,0x10,4)&0xfdffe088u))rc=DRV_EIO;
    return rc;
}
static int block(SdHost *h,unsigned cmd,uint32_t arg,void *buffer,bool write) {
    wr(h,0x04,2,512);wr(h,0x06,2,1);
    wr(h,0x0c,2,write?0:16);
    int rc=r1(h,cmd,arg,2,true);
    if(!rc)rc=status(h,write?16:32);
    if(!rc)for(unsigned i=0;i<128;i++) {
        uint32_t v;
        if(write){memcpy(&v,(uint8_t *)buffer+i*4,4);wr(h,0x20,4,v);}
        else{v=rd(h,0x20,4);memcpy((uint8_t *)buffer+i*4,&v,4);}
    }
    if(!rc)rc=status(h,2);
    if(rc)recover(h);
    return rc;
}
static int ready(SdHost *h) {
    uint32_t start=drv_now_ms();
    do {
        int rc=r1(h,13,h->rca,2,false);
        if(rc)return rc;
        uint32_t v=rd(h,0x10,4);
        if((v&0x1f00u)==0x900u)return 0; /* TRAN + READY_FOR_DATA */
    } while((uint32_t)(drv_now_ms()-start)<3000);
    return DRV_ETIMEDOUT;
}
static int transfer(void *ctx,uint64_t lba,unsigned n,void *buffer,bool write) {
    SdHost *h=ctx;
    if(!h->ready || !n || n>STORAGE_MAX_TRANSFER || lba+n>h->sectors)
        return DRV_EINVAL;
    if(write && (h->write_protected || (!h->mmc && !(rd(h,0x24,4)&(1u<<19)))))return DRV_ENOTSUP;
    for(unsigned i=0;i<n;i++) {
        int rc=block(h,write?24:17,(uint32_t)(lba+i),(uint8_t *)buffer+i*512,write);
        if(rc)return rc;
        if(write && (rc=ready(h)))return rc;
    }
    return 0;
}
static int flush(void *ctx) {
    SdHost *h=ctx;
    if(!h->ready)return DRV_EIO;
    if(h->mmc && h->cache) {
        int rc=r1(h,6,(3u<<24)|(32u<<16)|(1u<<8),3,false);
        if(rc)return rc;
    }
    return ready(h);
}
static bool clock_set(SdHost *h,unsigned hz) {
    unsigned version=rd(h,0xfe,2)&255u;
    unsigned base=(rd(h,0x40,4)>>8)&(version>=2?255u:63u);
    uint16_t divider;
    if(!sdhci_clock_divider(base*1000000u,hz,version,&divider))return false;
    wr(h,0x2c,2,0);
    wr(h,0x2c,2,divider|1u);
    if(!wait_bits(h,0x2c,2,2,2,100))return false;
    wr(h,0x2c,2,rd(h,0x2c,2)|4u);
    return true;
}
static bool card_init(SdHost *h) {
    h->phase=1;
    uint32_t caps=rd(h,0x40,4);
    h->embedded=h->platform_embedded || sdhci_pci_embedded(h->pci.vendor,h->pci.device) ||
        ((caps>>30)&3u)==1u;
    /* Soldered eMMC has neither a card-detect switch nor a WP pin. */
    if(!h->embedded && !wait_bits(h,0x24,4,1u<<16,1u<<16,100))return false;
    h->mmc=h->embedded;
    wr(h,0x2f,1,1);
    if(!wait_bits(h,0x2f,1,1,0,100))return false;
    uint32_t cap=rd(h,0x40,4);
    /* SD requires its initial 3.3/3.0 V supply. Dedicated eMMC hosts can
     * advertise only 1.8 V; this is not an SD CMD11/UHS voltage switch. */
    h->phase=9;
    bool low_voltage=h->embedded && (cap&(1u<<26));
    unsigned power=low_voltage?0x0a:(cap&(1u<<24))?0x0e:(cap&(1u<<25))?0x0c:0;
    if(!power)return false;
    wr(h,0x29,1,power);drv_sleep_ms(1);wr(h,0x29,1,power|1u);
    if((rd(h,0xfe,2)&255u)>=2) {
        uint32_t control=rd(h,0x3e,2)&~0x800fu; /* disable presets/UHS modes */
        wr(h,0x3e,2,control|(low_voltage?8u:0u));
        if((rd(h,0x3e,2)&8u)!=(low_voltage?8u:0u))return false;
    }
    drv_sleep_ms(5);wr(h,0x2e,1,0x0e);
    wr(h,0x34,4,0x03ff003bu);wr(h,0x38,4,0);
    wr(h,0x28,1,0);
    h->phase=10;
    if(!clock_set(h,400000))return false;
    drv_sleep_ms(2);
    if(command(h,0,0,0,false))return false;
    h->phase=2;
    bool sd=!h->embedded && !command(h,8,0x1aa,2,false) && (rd(h,0x10,4)&0xfffu)==0x1aa;
    uint32_t ocr=0,start=drv_now_ms();
    uint32_t voltage=low_voltage?0x80u:0x00ff8000u;
    if(sd) {
        do {
            if(r1(h,55,0,2,false) || command(h,41,0x40ff8000u,2,false))return false;
            ocr=rd(h,0x10,4);
            if(ocr&0x80000000u)break;
            drv_sleep_ms(10);
        } while((uint32_t)(drv_now_ms()-start)<2000);
    } else {
        h->mmc=true;
        if(command(h,0,0,0,false))return false;
        do {
            if(command(h,1,0x40000000u|voltage,2,false))return false;
            ocr=rd(h,0x10,4);
            if(ocr&0x80000000u)break;
            drv_sleep_ms(10);
        } while((uint32_t)(drv_now_ms()-start)<5000);
    }
    h->phase=3;
    if((ocr&0xc0000000u)!=0xc0000000u)return false;
    h->phase=4;
    if(command(h,2,0,1,false))return false;
    h->rca=h->mmc?1u<<16:0;
    if(command(h,3,h->rca,2,false))return false;
    if(!h->mmc)h->rca=rd(h,0x10,4)&0xffff0000u;
    if(!h->rca || command(h,9,h->rca,1,false))return false;
    h->write_protected=(rd(h,0x10,4)&0x30u)!=0; /* CSD bits 13:12 */
    h->phase=5;
    if(!h->mmc) {
        if((rd(h,0x1c,4)>>22)!=1)return false; /* CSD v2 */
        h->sectors=((uint64_t)((rd(h,0x14,4)>>8)&0x3fffffu)+1u)*1024u;
    }
    h->phase=6;
    if(r1(h,7,h->rca,3,false) || !clock_set(h,25000000))return false;
    h->phase=7;
    if(h->mmc) {
        uint8_t ext[512];
        if(block(h,8,0,ext,false) || ext[192]<2 || ext[179]&7u)return false;
        h->sectors=(uint32_t)ext[212]|((uint32_t)ext[213]<<8)|
          ((uint32_t)ext[214]<<16)|((uint32_t)ext[215]<<24);
        h->cache=ext[33]&1u;
        if(ext[61])return false; /* 4 KiB native sectors unsupported */
    }
    return h->sectors!=0;
}
/* Firmware may leave a non-boot eMMC function in PCI D3. Accessing its
 * SDHCI registers before requesting D0 can return all ones or time out. */
static int pci_power_on(SdHost *h) {
    if(!(pci_read(h->pci.bus,h->pci.slot,h->pci.function,4)&(1u<<20)))return 0;
    unsigned cap=pci_read(h->pci.bus,h->pci.slot,h->pci.function,0x34)&255u;
    for(unsigned n=0;cap && n<48;n++) {
        if(cap<0x40 || cap>0xf8 || (cap&3))return DRV_EINVAL;
        uint32_t v=pci_read(h->pci.bus,h->pci.slot,h->pci.function,cap);
        if((v&255u)==1) {
            uint32_t pm;
            if(drv_pci_config_read(&h->pci,cap+4,2,&pm))return DRV_EIO;
            if(pm&3u) {
                if(drv_pci_config_write(&h->pci,cap+4,2,pm&~0x8003u))return DRV_EIO;
                drv_sleep_ms(10);
                if(drv_pci_config_read(&h->pci,cap+4,2,&pm) || (pm&3u))return DRV_EIO;
            }
            return 0;
        }
        cap=(v>>8)&255u;
    }
    return cap?DRV_EINVAL:0;
}
static void diagnose(SdHost *h) {
    {char text[16];strcpy(last_reason,"SDHCI phase ");number(text,h->phase);
    append(last_reason,text,sizeof(last_reason));append(last_reason," CMD",sizeof(last_reason));
    number(text,h->last_cmd);append(last_reason,text,sizeof(last_reason));
    append(last_reason," status=",sizeof(last_reason));hex(text,h->last_status);
    append(last_reason,text,sizeof(last_reason));
    if(h->io.base) {
        append(last_reason," present=",sizeof(last_reason));hex(text,rd(h,0x24,4));
        append(last_reason,text,sizeof(last_reason));
        append(last_reason," caps=",sizeof(last_reason));hex(text,rd(h,0x40,4));
        append(last_reason,text,sizeof(last_reason));
        append(last_reason," ver=",sizeof(last_reason));hex(text,rd(h,0xfe,2));
        append(last_reason,text,sizeof(last_reason));
    }
    serial_write(last_reason);serial_write("\n");}
}
int storage_sdhci_probe(uint8_t bus,uint8_t slot,uint8_t fn) {
    strcpy(last_reason,"SDHCI: no host slot available");
    SdHost *h=0;
    for(unsigned i=0;i<4;i++)if(!hosts[i].used){h=&hosts[i];break;}
    if(!h)return DRV_ENOSPC;
    *h=(SdHost){0};h->pci=(DrvPci){.bus=bus,.slot=slot,.function=fn,
        .vendor=(uint16_t)pci_read(bus,slot,fn,0),.device=pci_read(bus,slot,fn,0)>>16};
    /* Stop inherited firmware DMA; PIO itself needs no bus mastering. */
    uint32_t cmd=pci_read(bus,slot,fn,4);
    pci_write(bus,slot,fn,4,cmd&~4u);
    if(drv_pci_claim(&h->pci)) {
        strcpy(last_reason,"SDHCI: PCI claim failed");return DRV_EBUSY;
    }
    h->phase=8;
    if(pci_power_on(h))goto fail;
    if(drv_pci_map(&h->pci,0,&h->io) || h->io.size<0x100 ||
       drv_pci_enable(&h->pci,false,true) || !card_init(h))goto fail;
    h->physical=pci_read(bus,slot,fn,0x10)&~15u;
    h->ready=true;
    bool ro=h->write_protected || (!h->mmc && !(rd(h,0x24,4)&(1u<<19)));
    StorageDevice d={.ctx=h,.transfer=transfer,.flush=flush,.sectors=h->sectors,
        .logical_bytes=512,.transport=STORAGE_EMMC,.removable=!h->mmc,
        .readonly=ro,.installable=!ro};
    strcpy(d.model,h->mmc?"MMC/eMMC user area":"SDHC/SDXC card");
    h->phase=11;
    if(storage_register(&d)<0)goto fail;
    h->used=true;return 1;
fail:
    diagnose(h);
    if(h->io.base){wr(h,0x2c,2,0);drv_mmio_unmap(&h->io);}
    drv_pci_release(&h->pci);memset(h,0,sizeof(*h));return DRV_ENOTSUP;
}

/* Same data path as PCI; discovery and ownership come from ACPI _CRS. */
int storage_sdhci_platform_probe(uint32_t physical,uint32_t length,bool embedded) {
    SdHost *h=0;
    strcpy(last_reason,"SDHCI ACPI: no host slot available");
    for(unsigned i=0;i<4;i++) {
        if(hosts[i].used && hosts[i].physical==physical)return DRV_EALREADY;
        if(!hosts[i].used && !h)h=&hosts[i];
    }
    if(!h)return DRV_ENOSPC;
    *h=(SdHost){.platform=true,.platform_embedded=embedded,.physical=physical,.phase=8};
    if(drv_platform_map(physical,length,&h->io) || !card_init(h))goto fail;
    h->ready=true;
    bool ro=h->write_protected || (!h->mmc && !(rd(h,0x24,4)&(1u<<19)));
    StorageDevice d={.ctx=h,.transfer=transfer,.flush=flush,.sectors=h->sectors,
        .logical_bytes=512,.transport=STORAGE_EMMC,.removable=!h->mmc,
        .readonly=ro,.installable=!ro};
    strcpy(d.model,h->mmc?"ACPI MMC/eMMC user area":"ACPI SDHC/SDXC card");
    if(storage_register(&d)<0)goto fail;
    h->used=true;return 1;
fail:
    diagnose(h);
    if(h->io.base){wr(h,0x2c,2,0);drv_mmio_unmap(&h->io);}
    memset(h,0,sizeof(*h));return DRV_ENOTSUP;
}
