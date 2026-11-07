#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/storage.h"
static uint8_t backing[2][100u*512u];
static unsigned writes,flushes;
static DiskInfo ata[4];
static int mock_transfer(void *ctx,uint64_t lba,unsigned count,void *buf,bool write) {
    unsigned n=*(unsigned *)ctx;
    if(n>=2 || count>64 || lba+count>100)return DRV_EINVAL;
    uint8_t *p=backing[n]+(size_t)lba*512u;
    if(write){memcpy(p,buf,count*512u);writes++;}
    else memcpy(buf,p,count*512u);
    return 0;
}
static int mock_flush(void *ctx){(void)ctx;flushes++;return 0;}
uint32_t drv_irq_save(void){return 0;}
void drv_irq_restore(uint32_t flags){(void)flags;}
const DiskInfo *disk_info(unsigned index) {
    if(index<4)return &ata[index];
    return storage_disk_info(index);
}
bool disk_read_blocks(unsigned index,uint32_t lba,unsigned count,void *buffer) {
    return storage_transfer(index,lba,count,buffer,false)==0;
}
void number(char *out,uint32_t value){sprintf(out,"%u",value);}
void hex(char *out,uint32_t value){sprintf(out,"%08x",value);}
void append(char *out,const char *str,size_t cap) {
    size_t n=strlen(out),len=strlen(str);
    if(n>=cap)return;
    if(len>=cap-n)len=cap-n-1;
    memcpy(out+n,str,len);
    out[n+len]=0;
}
static unsigned ctx0,ctx1;
int storage_ahci_probe(uint8_t b,uint8_t s,uint8_t f){
    assert(b==0 && s==1 && f==0);
    StorageDevice dev={.ctx=&ctx0,.transfer=mock_transfer,.flush=mock_flush,
      .sectors=100,.logical_bytes=512,.transport=STORAGE_AHCI,
      .installable=true};
    strcpy(dev.model,"SATA-TEST");
    return storage_register(&dev)>=0?1:DRV_ENOSPC;
}
int storage_nvme_probe(uint8_t b,uint8_t s,uint8_t f){
    assert(b==0 && s==2 && f==0);
    StorageDevice dev={.ctx=&ctx1,.transfer=mock_transfer,.flush=mock_flush,
      .sectors=100,.logical_bytes=512,.transport=STORAGE_NVME,
      .installable=true};
    strcpy(dev.model,"NVME-TEST");
    return storage_register(&dev)>=0?1:DRV_ENOSPC;
}
uint32_t pci_read(uint8_t b,uint8_t s,uint8_t f,uint8_t off) {
    if(b || f || s<1 || s>5)return UINT32_MAX;
    if(!off)return (uint32_t)(0x1000+s)<<16 | 0x8086u;
    if(off==0xc)return 0;
    if(off==8) {
        static const uint32_t c[]={0,0x010601,0x010802,0x0c0330,0x080500,0x010400};
        return c[s]<<8;
    }
    return 0;
}
int main(void) {
    storage_reset();
    storage_pci_init();
    assert(storage_controller_count()==5);
    assert(storage_disk_info(5) && storage_disk_info(6));
    assert(storage_controller(0)->initialized && storage_controller(1)->initialized);
    for(unsigned i=2;i<5;i++)assert(!storage_controller(i)->initialized);
    assert(!strcmp(storage_disk_type(5),"SATA/AHCI"));
    assert(!strcmp(storage_disk_type(6),"NVMe"));
    uint8_t data[1024],retrieved[1024];
    for(unsigned i=0;i<sizeof(data);i++)data[i]=(uint8_t)(i*7u);
    assert(!storage_transfer(5,10,2,data,true));
    assert(!storage_transfer(5,10,2,retrieved,false));
    assert(!memcmp(data,retrieved,sizeof(data)));
    assert(!storage_flush(5) && flushes==1);
    assert(storage_transfer(5,100,1,retrieved,false)==DRV_EINVAL);
    assert(storage_transfer(5,99,2,retrieved,false)==DRV_EINVAL);
    assert(storage_transfer(5,0,65,retrieved,false)==DRV_EINVAL);
    assert(storage_transfer(7,0,1,retrieved,false)==DRV_ENOTSUP);
    assert(storage_transfer(7,0,1,retrieved,true)==DRV_ENOTSUP);
    char status[2048];
    storage_status(status,sizeof(status));
    assert(strstr(status,"SATA/AHCI") && strstr(status,"NVMe"));
    assert(strstr(status,"USB") && strstr(status,"eMMC/SDHCI") && strstr(status,"RAID"));
    assert(strstr(status,"not yet available"));
    assert(!storage_verify_read(5,status,sizeof(status)));
    assert(strstr(status,"consistent physical reads"));
    assert(storage_verify_read(7,status,sizeof(status))==DRV_ENOTSUP);
    assert(writes==1);
    puts("PASS: block registry, read/write/flush, PCI dispatch, safe unsupported-controller inventory");
}
