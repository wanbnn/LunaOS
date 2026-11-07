#include "iwl_firmware.h"
#include <string.h>
#include <stdbool.h>
#define EINVAL (-22)
#define ENOSPC (-28)
#define ENOTSUP (-95)
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
int iwl_firmware_parse(const uint8_t *data, size_t n, IwlFirmware *out) {
    if(!out) return EINVAL;
    memset(out,0,sizeof(*out));
    if(!data || n<88 || n>2u*1024*1024 || le32(data) || le32(data+4)!=0x0a4c5749)
        return EINVAL;
    IwlFirmware fw={.version=le32(data+72),.build=le32(data+76),.cpu_count=1};
    bool have_cpu=false, have_paging=false, have_version=false, have_flags=false;
    for(size_t p=88;p<n;) {
        if(n-p<8) return EINVAL;
        uint32_t type=le32(data+p), bytes=le32(data+p+4); p+=8;
        if(bytes>n-p) return EINVAL;
        const uint8_t *v=data+p;
        size_t padded=((size_t)bytes+3)&~(size_t)3;
        if(padded>n-p) return EINVAL;
        if(type==19 || type==20 || type==21 || type==24 || type==25 || type==26) {
            if(bytes<4) return EINVAL;
            uint32_t address=le32(v), size=bytes-4;
            bool separator=address==0xffffcccc || address==0xaaaabbbb;
            if(!separator && (!size || size>UINT32_MAX-address)) return EINVAL;
            if(fw.section_count==IWL_FW_SECTIONS) return ENOSPC;
            fw.sections[fw.section_count++]=(IwlFwSection){type,address,size,v+4};
            if((type==19 || type==24) && !separator) fw.runtime_count++;
        } else if(type==27) {
            if(bytes!=4 || have_cpu) return EINVAL;
            fw.cpu_count=le32(v); have_cpu=true;
            if(fw.cpu_count<1 || fw.cpu_count>2) return ENOTSUP;
        } else if(type==32) {
            if(bytes!=4 || have_paging) return EINVAL;
            fw.paging_bytes=le32(v); have_paging=true;
            if(fw.paging_bytes>1024*1024 || fw.paging_bytes%4096) return EINVAL;
        } else if(type==36) {
            if(bytes!=12 || have_version) return EINVAL;
            for(unsigned i=0;i<3;i++) fw.version_triplet[i]=le32(v+i*4);
            have_version=true;
        } else if(type==18) {
            if(bytes<4 || bytes%4 || have_flags) return EINVAL;
            fw.flags=le32(v); have_flags=true;
        } else if(type==48) {
            if(bytes%4 || fw.command_versions) return EINVAL;
            fw.command_versions=v; fw.command_versions_bytes=bytes;
        }
        p+=padded;
    }
    if(!fw.runtime_count) return EINVAL;
    *out=fw; return 0;
}
const char *iwl_firmware_candidate(uint32_t hw, uint32_t rf) {
    /* 8000+ CSR format moves the step to bits 3:2 before iwlwifi matching.
     * Firmware SO A0/GF A0 is selected only for step A and known type. */
    if((hw&0x0000fff0)!=0x00000370 || (hw&0x0f)!=0 ||
       (rf&0x00fff000)!=0x0010d000 || (rf&0xf00)) return 0;
    return "iwlwifi-so-a0-gf-a0-89.ucode";
}
