#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../kernel/luna.h"
#include "../kernel/install.h"
#define SLOT_START 8192u
#define SLOT_SECTORS 4096u
#define SECTOR_COUNT (SLOT_START + 2u*SLOT_SECTORS)
static uint8_t disk[SECTOR_COUNT][512];
static int writes_left = -1;
void *heap_allocate_owned(size_t size, uint32_t owner) {
    assert(owner == 1);
    return malloc(size);
}
int heap_release(void *p) {
    free(p);
    return 0;
}
unsigned disk_boot_index(void) { return 0; }
bool disk_read_on(unsigned d, uint32_t sector, void *buffer) {
    if(d || sector>=SECTOR_COUNT || !buffer) return false;
    memcpy(buffer,disk[sector],512);return true;
}
bool disk_write_on(unsigned d, uint32_t sector, const void *buffer) {
    if(d || sector>=SECTOR_COUNT || !buffer || writes_left==0)return false;
    if(writes_left>0)writes_left--;
    memcpy(disk[sector],buffer,512);return true;
}
bool disk_flush(unsigned d) { return d==0; }
void serial_write(const char *s) { (void)s; }

typedef struct {
    char path[PATH_CAP];
    uint32_t used, size, dir;
    char data[8192];
} LegacyFile;
typedef struct {
    char magic[8];
    uint32_t version,generation,bytes,hash;
    uint8_t pad[488];
} OldHeader;
static LegacyFile legacy[MAX_FILES];
static uint32_t fnv(const uint8_t *p,size_t n) {
    uint32_t h=2166136261u;
    while(n--) h=(h^*p++)*16777619u;
    return h;
}
static void legacy_directory(unsigned index,const char *path) {
    legacy[index].used=legacy[index].dir=1;
    strcpy(legacy[index].path,path);
}
static void legacy_snapshot(uint32_t generation) {
    memset(legacy,0,sizeof(legacy));
    legacy_directory(0,"/");
    legacy_directory(1,"/home");
    legacy_directory(2,"/home/luna");
    legacy[3].used=1;
    strcpy(legacy[3].path,"/home/luna/legacy.txt");
    strcpy(legacy[3].data,"preserved v1");
    legacy[3].size=strlen(legacy[3].data);
    OldHeader h={0};
    memcpy(h.magic,"LUNAFS1",7);
    h.version=1;h.generation=generation;
    h.bytes=sizeof(legacy);h.hash=fnv((const uint8_t *)legacy,sizeof(legacy));
    const uint8_t *p=(const uint8_t *)legacy;
    for(size_t off=0;off<sizeof(legacy);off+=512) {
        size_t n=sizeof(legacy)-off;
        if(n>512)n=512;
        memcpy(disk[SLOT_START+1+off/512],p+off,n);
    }
    memcpy(disk[SLOT_START],&h,sizeof(h));
}
int main(void) {
    fs_init();
    assert(fs_find("/home/luna"));
    uint8_t *blob=malloc(256u*1024u);
    assert(blob);
    for(size_t i=0;i<256u*1024u;i++)blob[i]=(uint8_t)(i*17u);
    /* Binary data with zero bytes, far beyond former 8 KiB per-file cap. */
    assert(!fs_write_bytes("/home/luna/big.bin",blob,256u*1024u,false));
    File *f=fs_find("/home/luna/big.bin");
    assert(f && f->size==256u*1024u && !memcmp(f->data,blob,f->size));
    assert(!fs_save() && !fs_dirty);
    assert(!fs_remove("/home/luna/big.bin"));
    assert(!fs_load());
    f=fs_find("/home/luna/big.bin");
    assert(f && f->size==256u*1024u && !memcmp(f->data,blob,f->size));
    /* Deep copy backup: update the original, restore the prior data. */
    static File backup[MAX_FILES];
    assert(!fs_clone(backup,files));
    assert(!fs_write("/home/luna/big.bin","overwritten",false));
    fs_restore_state(backup);
    f=fs_find("/home/luna/big.bin");
    assert(f && f->size==256u*1024u && !memcmp(f->data,blob,f->size));
    /* Interrupted write must leave the previously committed slot intact. */
    assert(!fs_write("/home/luna/big.bin","uncommitted",false));
    writes_left=3;
    assert(!fs_save());
    writes_left=-1;
    assert(fs_load());
    f=fs_find("/home/luna/big.bin");
    assert(f && f->size==256u*1024u && !memcmp(f->data,blob,f->size));
    /* Old v1 installed volume loads and upgrades on the next sync. */
    legacy_snapshot(100);
    assert(fs_load());
    f=fs_find("/home/luna/legacy.txt");
    assert(f && f->size==12 && !strcmp(f->data,"preserved v1"));
    assert(!fs_write("/home/luna/legacy.txt","migrated to v2",false));
    assert(fs_save());
    assert(fs_load());
    f=fs_find("/home/luna/legacy.txt");
    assert(f && !strcmp(f->data,"migrated to v2"));
    /* No overrun of the volume slot; bound is aggregate capacity, not 8K. */
    assert(fs_write_bytes("/home/luna/too-big",blob,3u*1024u*1024u,false)<0);
    free(blob);
    puts("PASS: LunaFS2 256 KiB binary file, persistence, crash fallback, deep-copy and v1 migration");
    return 0;
}
