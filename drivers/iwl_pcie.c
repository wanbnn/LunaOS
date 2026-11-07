#include "iwl_pcie.h"
#include "iwl_firmware.h"
/* CSR addresses from Linux v6.12 iwlwifi/iwl-csr.h. No reset, RF, interrupt
 * or bus-master writes are made by preparation. PCI decode is restored. */
static DrvPci owner;
static DrvMmio mapping;
static DrvFirmware firmware;
static IwlPcieState boot_state;
static bool initialized;
int iwl_pcie_prepare(const WifiIntelProbe *probe, IwlPcieState *out) {
    if(!probe || !out) return DRV_EINVAL;
    memset(out,0,sizeof(*out)); out->stage="PCI identification";
    out->present=probe->pci.vendor==0x8086;
    if(!out->present || (probe->pci.class_code>>8)!=0x0280) {
        out->result=DRV_ENOTSUP; return out->result;
    }
    /* Retain handles on failed cleanup, so a retry cannot overwrite a live
     * mapping/claim. A reboot is required if platform cleanup failed. */
    if(owner.cookie || mapping.cookie || firmware.data) {
        out->stage="previous resource cleanup failed"; out->result=DRV_EBUSY;
        return out->result;
    }
    owner=probe->pci; out->stage="PCI claim";
    int rc=drv_pci_claim(&owner); if(rc) goto done;
    uint32_t command=0; out->stage="PCI command";
    rc=drv_pci_config_read(&owner,4,2,&command); if(rc) goto done;
    DrvBar bar={0}; out->stage="BAR0";
    rc=drv_pci_bar(&owner,0,&bar); if(rc) goto done;
    if(bar.io || bar.size<0xa0) { rc=DRV_ENOTSUP; goto done; }
    out->stage="MMIO decode";
    rc=drv_pci_enable(&owner,(command&1)!=0,true); if(rc) goto done;
    out->stage="MMIO mapping";
    rc=drv_pci_map(&owner,0,&mapping); if(rc) goto done;
    out->stage="CSR hardware identification";
    rc=drv_mmio_read(&mapping,0x028,4,&out->hw_rev); if(rc) goto done;
    rc=drv_mmio_read(&mapping,0x09c,4,&out->rf_id); if(rc) goto done;
    rc=drv_mmio_read(&mapping,0x024,4,&out->gp_control); if(rc) goto done;
    if(out->hw_rev==UINT32_MAX || out->rf_id==UINT32_MAX || out->gp_control==UINT32_MAX) {
        rc=DRV_EIO; goto done;
    }
    out->stage="SO A/GF A firmware selection";
    out->firmware_name=iwl_firmware_candidate(out->hw_rev,out->rf_id);
    if(!out->firmware_name) { rc=DRV_ENOTSUP; goto done; }
    out->stage="firmware file request";
    rc=drv_firmware_request(out->firmware_name,&firmware); if(rc) goto done;
    out->stage="firmware TLV validation";
    IwlFirmware image;
    rc=iwl_firmware_parse(firmware.data,firmware.size,&image); if(rc) goto done;
    out->firmware_valid=true; out->firmware_bytes=firmware.size;
    out->runtime_sections=image.runtime_count; out->paging_bytes=image.paging_bytes;
    out->stage="Gen3 context/DMA queues, firmware ALIVE and NVM not implemented";
    rc=DRV_ENOTSUP;
 done:
    if(firmware.data) {
        int r=drv_firmware_release(&firmware);
        if(r) { rc=r; out->stage="firmware release"; }
    }
    if(mapping.cookie) {
        int r=drv_mmio_unmap(&mapping);
        if(r) { rc=r; out->stage="MMIO unmap"; }
    }
    if(owner.cookie && !mapping.cookie) {
        int r=drv_pci_release(&owner);
        if(r) { rc=r; out->stage="PCI release"; }
    }
    out->result=rc; return rc;
}
void wifi_intel_init(void) {
    if(initialized) return;
    initialized=true;
    WifiIntelProbe probe;
    int rc=wifi_intel_probe(0,&probe);
    if(rc) { boot_state.result=rc; boot_state.stage="PCI discovery"; return; }
    iwl_pcie_prepare(&probe,&boot_state);
    serial_write("Intel Wi-Fi: "); serial_write(boot_state.stage); serial_write("\n");
}
static void field(char *out,size_t cap,const char *name,uint32_t value,bool hexadecimal) {
    char text[24]; append(out,name,cap);
    if(hexadecimal) hex(text,value); else number(text,value);
    append(out,text,cap); append(out,"\n",cap);
}
int wifi_intel_diag(char *out,size_t cap) {
    if(!out || !cap) return DRV_EINVAL;
    wifi_intel_init(); out[0]=0;
    append(out,"Intel CNVi/AX211 transport preparation\n",cap);
    append(out,boot_state.stage,cap); append(out,"\n",cap);
    field(out,cap,"result=",(uint32_t)-boot_state.result,false);
    if(boot_state.present) {
        field(out,cap,"CSR_HW_REV=",boot_state.hw_rev,true);
        field(out,cap,"CSR_HW_RF_ID=",boot_state.rf_id,true);
        field(out,cap,"CSR_GP_CNTRL=",boot_state.gp_control,true);
    }
    if(boot_state.firmware_name) {
        append(out,boot_state.firmware_name,cap); append(out,"\n",cap);
    }
    if(boot_state.firmware_valid) {
        field(out,cap,"Firmware bytes=",boot_state.firmware_bytes,false);
        field(out,cap,"Runtime sections=",boot_state.runtime_sections,false);
        field(out,cap,"Paging bytes=",boot_state.paging_bytes,false);
    }
    append(out,"Firmware has NOT been booted. Radio, scan and connection inactive.\n",cap);
    return boot_state.result;
}
