#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../drivers/iwl_firmware.h"
static void put32(uint8_t *p, uint32_t v) { for(unsigned i=0;i<4;i++) p[i]=v>>(8*i); }
int main(int argc, char **argv) {
    uint8_t data[112]={0}; IwlFirmware fw;
    put32(data+4,0x0a4c5749); put32(data+88,19); put32(data+92,5);
    put32(data+96,0x800000); data[100]=42;
    assert(!iwl_firmware_parse(data,104,&fw) && fw.runtime_count==1);
    assert(fw.sections[0].data==data+100 && fw.sections[0].size==1);
    for(unsigned n=0;n<104;n++) assert(iwl_firmware_parse(data,n,&fw) && !fw.section_count);
    put32(data+92,UINT32_MAX); assert(iwl_firmware_parse(data,104,&fw)==-22);
    put32(data+92,5); put32(data+96,UINT32_MAX);
    assert(iwl_firmware_parse(data,104,&fw)==-22);
    put32(data+96,0xffffcccc); put32(data+92,4);
    assert(iwl_firmware_parse(data,100,&fw)==-22); /* no runtime payload */
    assert(iwl_firmware_candidate(0x370,0x10d000));
    assert(!iwl_firmware_candidate(0x371,0x10d000));
    assert(!iwl_firmware_candidate(0x370,0x10e000));
    assert(!iwl_firmware_candidate(UINT32_MAX,UINT32_MAX));
    if(argc>1) {
        FILE *f=fopen(argv[1],"rb"); assert(f);
        assert(!fseek(f,0,SEEK_END)); long bytes=ftell(f); assert(bytes>0);
        rewind(f); uint8_t *blob=malloc(bytes); assert(blob);
        assert(fread(blob,1,bytes,f)==(size_t)bytes); fclose(f);
        int rc=iwl_firmware_parse(blob,bytes,&fw);
        printf("Intel firmware parse: rc=%d bytes=%ld runtime=%u sections=%u paging=%u CPUs=%u\n",
               rc,bytes,fw.runtime_count,fw.section_count,fw.paging_bytes,fw.cpu_count);
        assert(!rc && fw.runtime_count && fw.cpu_count==2); free(blob);
    }
    puts("PASS: Intel TLV bounds, truncation, overflow, RF selection and real firmware");
}
