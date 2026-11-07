/* Native PCI/MMIO preparation against a mocked bus + real firmware image. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../drivers/iwl_pcie.h"
static unsigned step, fail_step, claimed, mapped, loaded;
static bool unknown_rf, bad_firmware, unmap_fail;
static uint8_t *blob; static unsigned blob_bytes;
static int next(void) { return ++step==fail_step ? DRV_EIO : 0; }
int drv_pci_claim(DrvPci *p) { int rc=next();if(rc)return rc;assert(!claimed);claimed=1;p->cookie=1;return 0; }
int drv_pci_config_read(DrvPci *p,unsigned off,unsigned width,uint32_t *out) {
    assert(p->cookie && off==4 && width==2);int rc=next();if(rc)return rc;*out=0;return 0;
}
int drv_pci_bar(DrvPci *p,unsigned bar,DrvBar *out) {
    assert(p->cookie && bar==0);int rc=next();if(rc)return rc;
    *out=(DrvBar){.address=0xf0000000,.size=0x4000};return 0;
}
int drv_pci_enable(DrvPci *p,bool io,bool memory) { assert(p->cookie && !io && memory);return next(); }
int drv_pci_map(DrvPci *p,unsigned bar,DrvMmio *out) {
    assert(p->cookie && !bar);int rc=next();if(rc)return rc;
    assert(!mapped);mapped=1;out->cookie=1;return 0;
}
int drv_mmio_read(DrvMmio *mm,unsigned off,unsigned width,uint32_t *out) {
    assert(mm->cookie && width==4);int rc=next();if(rc)return rc;
    if(off==0x28)*out=0x370;
    else if(off==0x9c)*out=unknown_rf ? 0x10e000 : 0x10d000;
    else {assert(off==0x24);*out=0x08000000;}return 0;
}
int drv_firmware_request(const char *name,DrvFirmware *out) {
    assert(!strcmp(name,"iwlwifi-so-a0-gf-a0-89.ucode"));int rc=next();if(rc)return rc;
    assert(!loaded);loaded=1;out->data=blob;out->size=bad_firmware ? 10 : blob_bytes;return 0;
}
int drv_firmware_release(DrvFirmware *fw) { assert(loaded);loaded=0;memset(fw,0,sizeof(*fw));return 0; }
int drv_mmio_unmap(DrvMmio *mm) {
    assert(mapped);if(unmap_fail)return DRV_EIO;mapped=0;memset(mm,0,sizeof(*mm));return 0;
}
int drv_pci_release(DrvPci *p) {assert(claimed && !mapped);claimed=0;p->cookie=0;return 0;}
int main(void) {
    FILE *f=fopen("assets/firmware/iwlwifi-so-a0-gf-a0-89.ucode","rb");assert(f);
    assert(!fseek(f,0,SEEK_END));blob_bytes=ftell(f);rewind(f);blob=malloc(blob_bytes);assert(blob);
    assert(fread(blob,1,blob_bytes,f)==blob_bytes);fclose(f);
    WifiIntelProbe p={.pci={.vendor=0x8086,.device=0x51f0,.class_code=0x028000}};
    IwlPcieState state;
    assert(iwl_pcie_prepare(&p,&state)==-95 && state.firmware_valid && state.runtime_sections==58);
    assert(!claimed && !mapped && !loaded);
    for(unsigned i=1;i<=9;i++) {
        step=0;fail_step=i;
        assert(iwl_pcie_prepare(&p,&state)==DRV_EIO && !state.firmware_valid);
        assert(!claimed && !mapped && !loaded);
    }
    step=fail_step=0;unknown_rf=true;
    assert(iwl_pcie_prepare(&p,&state)==-95 && !loaded && !state.firmware_name);
    unknown_rf=false;bad_firmware=true;step=0;
    assert(iwl_pcie_prepare(&p,&state)==-22 && !loaded && !mapped && !claimed);
    bad_firmware=false;step=0;unmap_fail=true;
    assert(iwl_pcie_prepare(&p,&state)==DRV_EIO && claimed && mapped && !loaded);
    assert(iwl_pcie_prepare(&p,&state)==DRV_EBUSY); /* retained ownership */
    free(blob);
    puts("PASS: Intel PCI/MMIO preparation, real firmware, acquisition errors and resource ownership on cleanup failure");
}
