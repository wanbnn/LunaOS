#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../kernel/luna.h"
#include "../kernel/install.h"
#include "../kernel/fs_volume.h"
#define SLOT_START LUNA_VOLUME_START
#define SLOT_SECTORS 4096u
#define SECTOR_COUNT (224u * 1024u)
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
static DiskInfo device;
const DiskInfo *disk_info(unsigned d) { return d ? 0 : &device; }
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
bool disk_read_blocks(unsigned d, uint32_t sector, unsigned count, void *buffer) {
    for (unsigned i = 0; i < count; i++)
        if (!disk_read_on(d, sector + i, (uint8_t *)buffer + i * 512u))
            return false;
    return true;
}
bool disk_write_blocks(unsigned d, uint32_t sector, unsigned count, const void *buffer) {
    for (unsigned i = 0; i < count; i++)
        if (!disk_write_on(d, sector + i, (const uint8_t *)buffer + i * 512u))
            return false;
    return true;
}
static bool matches(const File *f, const void *data, size_t n) {
    if (!f || f->size != n)
        return false;
    uint8_t buffer[16384];
    for (size_t off = 0; off < n;) {
        size_t bytes = n - off;
        if (bytes > sizeof(buffer))
            bytes = sizeof(buffer);
        if (!fs_read_bytes(f, off, buffer, bytes) ||
            memcmp(buffer, (const uint8_t *)data + off, bytes))
            return false;
        off += bytes;
    }
    return true;
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
static LegacyFile legacy[64];
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
    assert(matches(f, blob, 256u * 1024u));
    assert(fs_save() && !fs_dirty);
    assert(!fs_remove("/home/luna/big.bin"));
    assert(fs_load());
    f=fs_find("/home/luna/big.bin");
    assert(matches(f, blob, 256u * 1024u));
    /* Deep copy backup: update the original, restore the prior data. */
    static FileState backup;
    assert(!fs_clone(&backup,files));
    assert(!fs_write("/home/luna/big.bin","overwritten",false));
    fs_restore_state(&backup);
    f=fs_find("/home/luna/big.bin");
    assert(matches(f, blob, 256u * 1024u));
    /* Interrupted write must leave the previously committed slot intact. */
    assert(!fs_write("/home/luna/big.bin","uncommitted",false));
    writes_left=3;
    assert(!fs_save());
    writes_left=-1;
    assert(fs_load());
    f=fs_find("/home/luna/big.bin");
    assert(matches(f, blob, 256u * 1024u));
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
    /* Grow onto free disk space without overwriting another partition. */
    device = (DiskInfo){.present=true,.supported=true,.sectors=SECTOR_COUNT};
    disk[0][510]=0x55;disk[0][511]=0xaa;
    memcpy(disk[LUNA_IMAGE_INFO_SECTOR],"LUNAIMG1",8);
    uint32_t image_version=1,image_bytes=LUNA_IMAGE_BYTES;
    memcpy(disk[LUNA_IMAGE_INFO_SECTOR]+8,&image_version,4);
    memcpy(disk[LUNA_IMAGE_INFO_SECTOR]+12,&image_bytes,4);
    uint32_t partitions[3][2]={{LUNA_VOLUME_START,65536-LUNA_VOLUME_START},{65536,69632},{SECTOR_COUNT-2048,2048}};
    for(unsigned i=0;i<3;i++) {
        uint8_t *entry=disk[0]+446+i*16;
        entry[4]=i==0?0xda:i==1?0xef:0x83;
        memcpy(entry+8,&partitions[i][0],4);
        memcpy(entry+12,&partitions[i][1],4);
    }
    memset(disk[SECTOR_COUNT-2048],0xa5,512);
    uint8_t original_table[48];memcpy(original_table,disk[0]+446,48);
    assert(fs_load());
    assert(fs_find("/home/luna/legacy.txt"));
    size_t large_size=3u*1024u*1024u;
    uint8_t *large=malloc(large_size);
    assert(large);memset(large,0x5a,large_size);
    assert(!fs_write_bytes("/home/luna/expanded.bin",large,large_size,false));
    for(unsigned i=0;i<200;i++) {
        char path[80];snprintf(path,sizeof(path),"/home/luna/file-%u",i);
        assert(!fs_write(path,"expanded directory",false));
    }
    assert(!fs_copy("/home/luna/expanded.bin", "/home/luna/copy.bin"));
    assert(matches(fs_find("/home/luna/copy.bin"), large, large_size));
    assert(!fs_write_bytes("/home/luna/copy.bin", "tail", 4, true));
    f = fs_find("/home/luna/copy.bin");
    assert(f && f->size == large_size + 4u && !f->data);
    char suffix[4];
    assert(fs_read_bytes(f, large_size, suffix, 4) && !memcmp(suffix, "tail", 4));
    assert(fs_file_capacity>64);
    assert(!fs_clone(&backup,files));
    writes_left=1;assert(!fs_save());writes_left=-1;
    assert(fs_load() && fs_find("/home/luna/legacy.txt"));
    fs_restore_state(&backup);
    assert(fs_save() && fs_verify_on(0));
    assert(!memcmp(original_table,disk[0]+446,48));
    for(unsigned i=0;i<512;i++)assert(disk[SECTOR_COUNT-2048][i]==0xa5);
    assert(fs_load());
    f=fs_find("/home/luna/expanded.bin");
    assert(matches(f, large, large_size));
    assert(fs_find("/home/luna/file-199"));
    assert(!fs_write("/home/luna/expanded.bin","interrupted",false));
    writes_left=5;assert(!fs_save());writes_left=-1;
    assert(fs_load());
    f=fs_find("/home/luna/expanded.bin");
    assert(matches(f, large, large_size));
    FsVolume volume;
    assert(fs_volume(0,false,&volume) && volume.extended && volume.exists);
    /* An overlapping table must never authorize writes into another volume. */
    uint32_t overlap=LUNA_IMAGE_BYTES/512;
    memcpy(disk[0]+446+2*16+8,&overlap,4);
    assert(fs_volume(0,false,&volume) && !volume.extended);
    free(large);
    free(blob);
    puts("PASS: LunaFS growth beyond 64 entries and 2 MiB, partition isolation, persistence, crash recovery and legacy migration");
    return 0;
}
