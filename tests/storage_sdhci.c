/* Register-level regression model: the QEMU removable-slot defaults hide
 * the missing CD/WP pins, D3 state, 1.8 V and 200 MHz eMMC-host cases. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../drivers/storage_sdhci.c"
static uint8_t regs[256],sector[512],ext_csd[512];
static unsigned clock_ms, data_pos, active_cmd, commands[64], pmcsr;
static uint32_t data_lba, pmstate=3, model_id, fail_cmd=UINT32_MAX;
static bool mapped, registered, powered;
static StorageDevice published;
static uint32_t load(unsigned off,unsigned width) {
    uint32_t v=0;memcpy(&v,regs+off,width);return v;
}
static void save(unsigned off,unsigned width,uint32_t value){memcpy(regs+off,&value,width);}
void storage_poll_wait(unsigned *polls) { (void)polls; }
uint32_t drv_now_ms(void){return clock_ms++;}
int drv_sleep_ms(uint32_t ms){clock_ms+=ms;return 0;}
void serial_write(const char *s){(void)s;}
void number(char *s,uint32_t v){sprintf(s,"%u",v);}
void hex(char *s,uint32_t v){sprintf(s,"0x%08x",v);}
void append(char *s,const char *v,size_t cap) {
    size_t len=strlen(s),count=strlen(v);
    if(len>=cap)return;
    if(count>=cap-len)count=cap-len-1;
    memcpy(s+len,v,count);s[len+count]=0;
}
uint32_t pci_read(uint8_t b,uint8_t d,uint8_t f,uint8_t off) {
    assert(!b && d==5 && !f);
    if(!off)return model_id;
    if(off==4)return 1u<<20; /* PCI capabilities list */
    if(off==0x34)return 0x50;
    if(off==0x50)return 1; /* PM capability */
    return 0;
}
void pci_write(uint8_t b,uint8_t d,uint8_t f,uint8_t off,uint32_t v) {
    assert(!b && d==5 && !f && off==4 && !(v&4));
}
int drv_pci_claim(DrvPci *p){p->cookie=1;return 0;}
int drv_pci_release(DrvPci *p){p->cookie=0;return 0;}
int drv_pci_enable(DrvPci *p,bool io,bool mem){(void)p;assert(!io && mem);return 0;}
int drv_pci_config_read(DrvPci *p,unsigned off,unsigned width,uint32_t *v) {
    (void)p;assert(off==0x54 && width==2);*v=pmstate;return 0;
}
int drv_pci_config_write(DrvPci *p,unsigned off,unsigned width,uint32_t v) {
    (void)p;assert(off==0x54 && width==2);pmstate=v&3u;pmcsr++;return 0;
}
int drv_pci_map(DrvPci *p,unsigned bar,DrvMmio *m) {
    (void)p;assert(!bar && !pmstate);mapped=true;
    *m=(DrvMmio){.base=regs,.size=sizeof(regs),.cookie=1};return 0;
}
int drv_mmio_unmap(DrvMmio *m){mapped=false;*m=(DrvMmio){0};return 0;}
int drv_platform_map(uint32_t p,uint32_t n,DrvMmio *m) {
    assert(p==0xfed80000 && n==4096);mapped=true;
    *m=(DrvMmio){.base=regs,.size=sizeof(regs),.cookie=2};return 0;
}
int storage_register(const StorageDevice *d) {
    assert(!registered);registered=true;published=*d;return 5;
}
int drv_mmio_read(DrvMmio *m,uint32_t off,unsigned width,uint32_t *v) {
    assert(mapped && m->base==regs && off+width<=sizeof(regs));
    if(off==0x20) {
        assert(width==4 && data_pos<512);
        const uint8_t *p=active_cmd==8?ext_csd:sector;
        memcpy(v,p+data_pos,4);data_pos+=4;
        if(data_pos==512)save(0x30,4,load(0x30,4)|2u);
    } else *v=load(off,width);
    return 0;
}
int drv_mmio_write(DrvMmio *m,uint32_t off,unsigned width,uint32_t v) {
    assert(mapped && m->base==regs && off+width<=sizeof(regs));
    if(off==0x30){save(off,width,load(off,width)&~v);return 0;}
    if(off==0x2f) {save(off,1,0);return 0;}
    if(off==0x29) {powered=(v&1)!=0;save(off,width,v);return 0;}
    if(off==0x2c){save(off,width,v|((v&1)?2:0));return 0;}
    if(off==0x20) {
        assert(width==4 && active_cmd==24 && data_pos<512);
        memcpy(sector+data_pos,&v,4);data_pos+=4;
        if(data_pos==512)save(0x30,4,load(0x30,4)|2u);
        return 0;
    }
    save(off,width,v);
    if(off!=0x0e)return 0;
    active_cmd=v>>8;commands[active_cmd]++;data_pos=0;
    assert(powered && (load(0x2c,2)&7u)==7);
    assert(active_cmd!=8 || (v&32u)); /* dedicated eMMC never SD CMD8 */
    save(0x30,4,active_cmd==fail_cmd?0x18001u:1);
    save(0x10,4,0x900); /* ready + TRAN */
    if(active_cmd==1) {
        uint32_t requested=load(0x08,4);
        assert((requested&0x40000000u) && (requested&0x80u));
        save(0x10,4,0xc0000080u);
    }
    if(active_cmd==9)save(0x10,4,0);
    if(active_cmd==7 || active_cmd==6)save(0x30,4,load(0x30,4)|2u);
    if(active_cmd==8 || active_cmd==17 || active_cmd==24) {
        data_lba=load(0x08,4);
        assert(active_cmd==8 || data_lba==7);
        save(0x30,4,load(0x30,4)|(active_cmd==24?16u:32u));
    }
    return 0;
}
static void setup_model(uint16_t id) {
    memset(hosts,0,sizeof(hosts));memset(regs,0,sizeof(regs));
    memset(commands,0,sizeof(commands));memset(ext_csd,0,sizeof(ext_csd));
    registered=false;pmstate=3;pmcsr=0;model_id=((uint32_t)id<<16)|0x8086u;
    /* 200 MHz v3, 1.8 V only, no card-detect/WP bits, PCI D3. */
    save(0xfe,2,2);save(0x40,4,(200u<<8)|(1u<<26));
    ext_csd[192]=8;ext_csd[215]=8; /* 64 GiB in 512-byte sectors */
    ext_csd[33]=1; /* cache enabled, require real FLUSH_CACHE command */
}
int main(void) {
    uint16_t divisor;
    assert(sdhci_clock_divider(200000000,400000,2,&divisor) && divisor==0xfa00);
    assert(sdhci_clock_divider(255000000,400000,2,&divisor) && divisor==0x3f40);
    assert(!sdhci_clock_divider(200000000,400000,1,&divisor));
    assert(sdhci_clock_divider(50000000,400000,1,&divisor) && divisor==0x4000);
    assert(!sdhci_pci_embedded(0x8086,0x0f16)); /* removable SD function */
    assert(!sdhci_pci_embedded(0x1234,0x31cc));
    setup_model(0x31cc);
    assert(storage_sdhci_probe(0,5,0)==1 && registered && pmcsr==1);
    assert(published.sectors==134217728u && published.installable && !published.readonly);
    assert(load(0x29,1)==0x0b && (load(0x3e,2)&8u));
    uint8_t data[512],back[512];
    for(unsigned i=0;i<512;i++)data[i]=(uint8_t)(i*13u);
    assert(!published.transfer(published.ctx,7,1,data,true));
    assert(!published.flush(published.ctx) && commands[6]==1);
    assert(!published.transfer(published.ctx,7,1,back,false));
    assert(!memcmp(data,back,512));
    assert(published.transfer(published.ctx,published.sectors,1,back,false)==DRV_EINVAL);
    ((SdHost *)published.ctx)->write_protected=true;
    assert(published.transfer(published.ctx,7,1,data,true)==DRV_ENOTSUP);
    setup_model(0x1234); /* no PCI eMMC ID, ACPI supplied a fixed MMIO resource */
    assert(storage_sdhci_platform_probe(0xfed80000,4096,true)==1 && registered);
    assert(!pmcsr && published.installable && published.sectors==134217728u);
    assert(!published.transfer(published.ctx,7,1,data,true));
    assert(!published.flush(published.ctx));
    assert(!published.transfer(published.ctx,7,1,back,false) && !memcmp(data,back,512));
    assert(storage_sdhci_platform_probe(0xfed80000,4096,true)==DRV_EALREADY);
    setup_model(0x0f16); /* removable SD must still require card detect */
    assert(storage_sdhci_probe(0,5,0)<0 && !registered && !commands[1]);
    setup_model(0x5acc);fail_cmd=1;
    assert(storage_sdhci_probe(0,5,0)<0 && !registered && !mapped);
    char reason[160];storage_sdhci_reason(reason,sizeof(reason));
    assert(strstr(reason,"CMD1") && strstr(reason,"18001"));
    puts("PASS: Intel embedded eMMC 64 GiB, D3->D0, no CD/WP, 1.8 V, v3 200 MHz, PIO read/write/cache flush, failure diagnostics");
}
