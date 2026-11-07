#include "install.h"
#include "luna.h"
#include "notices.h"
#include "heap.h"
File files[MAX_FILES];
char cwd[PATH_CAP] = "/home/luna";
bool fs_dirty;
static File staging[MAX_FILES];
static uint32_t generation;
bool fs_path(const char *in, char *out) {
    char raw[256];
    if (!in || !*in)
        in = cwd;
    if (strlen(in) > 120)
        return false;
    raw[0] = 0;
    if (*in != '/') {
        strcpy(raw, cwd);
        strcat(raw, "/");
    }
    strcat(raw, in);
    out[0] = '/';
    out[1] = 0;
    int i = 0;
    while (raw[i]) {
        while (raw[i] == '/')
            i++;
        char part[128];
        int k = 0;
        while (raw[i] && raw[i] != '/')
            part[k++] = raw[i++];
        part[k] = 0;
        if (!k || !strcmp(part, "."))
            continue;
        if (!strcmp(part, "..")) {
            int n = strlen(out);
            while (n > 1 && out[n - 1] != '/')
                n--;
            if (n > 1)
                n--;
            out[n] = 0;
            continue;
        }
        if (strlen(out) + k + 2 >= PATH_CAP)
            return false;
        if (strlen(out) > 1)
            strcat(out, "/");
        strcat(out, part);
    }
    return true;
}
File *fs_find(const char *path) {
    char p[PATH_CAP];
    if (!fs_path(path, p))
        return 0;
    for (int i = 0; i < MAX_FILES; i++)
        if (files[i].used && !strcmp(files[i].path, p))
            return &files[i];
    return 0;
}
/* LunaFS2: bounded, binary-safe copy-on-write snapshots. Unlike v1, the
 * in-memory directory contains dynamic buffers; the disk snapshot serializes
 * only bytes belonging to live files. The image has two 2 MiB slots, and the
 * archive must fit in one slot. This is a volume budget, not a per-file 8K cap. */
#define FS_SLOT_START 8192u
#define FS_SLOT_SECTORS 4096u
#define FS_DATA_BYTES ((FS_SLOT_SECTORS-1u)*512u)
#define FS_V1_FILE_BYTES (PATH_CAP+12u+8192u)
typedef struct {
    char magic[8];
    uint32_t version, generation, bytes, hash, entries;
    uint8_t pad[484];
} Snapshot;
typedef struct {
    uint32_t index;
    char path[PATH_CAP];
    uint32_t size, dir;
} Record;
typedef struct {
    char path[PATH_CAP];
    uint32_t used, size, dir;
    char data[8192];
} LegacyFile;
_Static_assert(sizeof(Snapshot)==512, "LunaFS header");
_Static_assert(sizeof(Record)==140, "LunaFS2 entry layout");
_Static_assert(sizeof(LegacyFile)==FS_V1_FILE_BYTES, "LunaFS1 legacy entry layout");

static void drop_state(File *state) {
    for (unsigned i=0; i<MAX_FILES; i++) {
        if (state[i].data) heap_release(state[i].data);
        memset(&state[i],0,sizeof(state[i]));
    }
}
static char *allocate_data(size_t bytes) {
    if (bytes >= FS_DATA_BYTES) return 0;
    return heap_allocate_owned(bytes+1u,1);
}
static bool has_nul(const char *s, size_t size) {
    for (size_t i=0; i<size; i++) if (!s[i]) return true;
    return false;
}
static bool valid_state(const File *state) {
    bool root = false;
    for (unsigned i=0; i<MAX_FILES; i++) {
        const File *f=&state[i];
        if (!f->used) continue;
        if (f->used != 1 || f->dir>1 || f->size>FS_DATA_BYTES ||
            !has_nul(f->path,PATH_CAP) || f->path[0]!='/') return false;
        if (f->dir && (f->size || f->data)) return false;
        if (!f->dir && (!f->data || f->data[f->size])) return false;
        if (!strcmp(f->path,"/")) {
            if (root || !f->dir) return false;
            root=true;
        }
        for (unsigned j=0; j<i; j++)
            if (state[j].used && !strcmp(f->path,state[j].path)) return false;
    }
    return root;
}
static size_t state_bytes(const File *state, uint32_t *entries) {
    size_t total=0;
    uint32_t count=0;
    for (unsigned i=0; i<MAX_FILES; i++) if (state[i].used) {
        size_t additional=sizeof(Record)+(state[i].dir ? 0u : state[i].size);
        if (additional>FS_DATA_BYTES-total) return 0;
        total+=additional;
        count++;
    }
    if(entries)*entries=count;
    return total;
}
int fs_clone(File *dest, const File *source) {
    if (!dest || !source || dest==source) return -1;
    /* Destination is an empty state or a previously discarded backup. */
    memset(dest,0,MAX_FILES*sizeof(File));
    for (unsigned i=0; i<MAX_FILES; i++) {
        dest[i]=source[i];
        dest[i].data=0;
        if (source[i].used && !source[i].dir) {
            dest[i].data=allocate_data(source[i].size);
            if (!dest[i].data) { drop_state(dest);return -1; }
            memcpy(dest[i].data,source[i].data,source[i].size+1u);
        }
    }
    return 0;
}
void fs_discard_state(File *state) { if(state)drop_state(state); }
void fs_restore_state(File *backup) {
    if (!backup) return;
    drop_state(files);
    memcpy(files,backup,sizeof(files));
    memset(backup,0,sizeof(files)); /* Ownership moved, never free twice. */
    fs_dirty=true;
}

int fs_create(const char *path, bool dir) {
    char p[PATH_CAP],parent[PATH_CAP];
    if (!fs_path(path,p))return -1;
    if(fs_find(p))return -2;
    strcpy(parent,p);
    char *last=0;
    for(char *s=parent;*s;s++)if(*s=='/')last=s;
    if(last==parent)parent[1]=0;
    else if(last)*last=0;
    File *parent_file=fs_find(parent);
    if(!parent_file || !parent_file->dir)return -3;
    if(state_bytes(files,0)+sizeof(Record)>FS_DATA_BYTES)return -4;
    for(unsigned i=0;i<MAX_FILES;i++)if(!files[i].used) {
        char *buffer=dir?0:allocate_data(0);
        if(!dir && !buffer)return -4;
        memset(&files[i],0,sizeof(File));
        strcpy(files[i].path,p);
        files[i].used=1;
        files[i].dir=dir;
        files[i].data=buffer;
        if(buffer)buffer[0]=0;
        fs_dirty=true;
        return 0;
    }
    return -4;
}
int fs_remove(const char *path) {
    File *f=fs_find(path);
    if(!f || !strcmp(f->path,"/") || !strcmp(f->path,cwd))return -1;
    size_t n=strlen(f->path);
    for(unsigned i=0;i<MAX_FILES;i++)
        if(files[i].used && !strncmp(files[i].path,f->path,n) && files[i].path[n]=='/')
            return -2;
    if(f->data)heap_release(f->data);
    memset(f,0,sizeof(*f));
    fs_dirty=true;
    return 0;
}
int fs_write_bytes(const char *path, const void *data, size_t n, bool add) {
    if((n && !data) || n>FS_DATA_BYTES)return -1;
    File *f=fs_find(path);
    if(f && f->dir)return -1;
    size_t old=f?f->size:0u, off=add?old:0u;
    if(off>FS_DATA_BYTES-n)return -2;
    size_t wanted=off+n, used=state_bytes(files,0);
    if(!used)return -2;
    size_t overhead=used-old+(f?0u:sizeof(Record));
    if(overhead>FS_DATA_BYTES || wanted>FS_DATA_BYTES-overhead)return -2;
    char *new_data=allocate_data(wanted);
    if(!new_data)return -3;
    if(off)memcpy(new_data,f->data,off);
    if(n)memcpy(new_data+off,data,n);
    new_data[wanted]=0; /* Text callers retain a sentinel; bytes may include NUL. */
    if(!f) {
        if(fs_create(path,false)) { heap_release(new_data);return -1; }
        f=fs_find(path);
    }
    if(f->data)heap_release(f->data);
    f->data=new_data;
    f->size=(uint32_t)wanted;
    fs_dirty=true;
    return 0;
}
int fs_write(const char *path,const char *data,bool add) {
    if(!data)return -1;
    return fs_write_bytes(path,data,strlen(data),add);
}

/* Bytewise FNV-1a over the exact serialized payload, independent of padding. */
static uint32_t hash_more(uint32_t h, const void *ptr, size_t n) {
    const uint8_t *p=ptr;
    while(n--)h=(h^*p++)*16777619u;
    return h;
}
typedef struct {
    unsigned disk;
    uint32_t base, sector;
    uint8_t buffer[512];
    size_t pos, total;
    unsigned cursor;
    uint32_t hash;
    bool ok, writing;
} Stream;
static void writer_init(Stream *s,unsigned disk,uint32_t base) {
    *s=(Stream){.disk=disk,.base=base,.sector=1,.hash=2166136261u,.ok=true,.writing=true};
}
static void reader_init(Stream *s,unsigned disk,uint32_t base,size_t bytes) {
    *s=(Stream){.disk=disk,.base=base,.sector=1,.total=bytes,.cursor=512,
                 .hash=2166136261u,.ok=true,.writing=false};
}
static bool put(Stream *s,const void *data,size_t n) {
    if(!s->ok || s->pos+n>FS_DATA_BYTES)return false;
    const uint8_t *p=data;
    s->hash=hash_more(s->hash,data,n);
    s->pos+=n;
    while(n) {
        unsigned room=512u-s->cursor;
        unsigned count=n<room?(unsigned)n:room;
        memcpy(s->buffer+s->cursor,p,count);
        s->cursor+=count;
        p+=count;n-=count;
        if(s->cursor==512) {
            if(s->sector>=FS_SLOT_SECTORS ||
               !disk_write_on(s->disk,s->base+s->sector++,s->buffer))
                return s->ok=false;
            s->cursor=0;
        }
    }
    return true;
}
static bool writer_finish(Stream *s) {
    if(!s->ok)return false;
    if(s->cursor) {
        memset(s->buffer+s->cursor,0,512-s->cursor);
        if(s->sector>=FS_SLOT_SECTORS ||
           !disk_write_on(s->disk,s->base+s->sector++,s->buffer))
            return false;
    }
    return true;
}
static bool take(Stream *s,void *data,size_t n) {
    if(!s->ok || n>s->total-s->pos)return false;
    uint8_t *out=data;
    size_t pending=n;
    while(pending) {
        if(s->cursor==512) {
            if(s->sector>=FS_SLOT_SECTORS ||
               !disk_read_on(s->disk,s->base+s->sector++,s->buffer))
                return s->ok=false;
            s->cursor=0;
        }
        unsigned room=512u-s->cursor;
        unsigned count=pending<room?(unsigned)pending:room;
        memcpy(out,s->buffer+s->cursor,count);
        s->hash=hash_more(s->hash,out,count);
        s->cursor+=count;
        s->pos+=count;
        out+=count;pending-=count;
    }
    return true;
}
static bool read_slot(unsigned device,uint32_t base,uint32_t *gen) {
    drop_state(staging);
    Snapshot header;
    if(!disk_read_on(device,base,&header))return false;
    bool legacy=!memcmp(header.magic,"LUNAFS1",7) && header.version==1;
    bool modern=!memcmp(header.magic,"LUNAFS2",7) && header.version==2;
    if(!legacy && !modern)return false;
    if(!header.bytes || header.bytes>FS_DATA_BYTES ||
       (legacy && header.bytes!=MAX_FILES*sizeof(LegacyFile)) ||
       (modern && (!header.entries || header.entries>MAX_FILES)))return false;
    Stream stream;
    reader_init(&stream,device,base,header.bytes);
    if(legacy) {
        LegacyFile record;
        for(unsigned i=0;i<MAX_FILES;i++) {
            if(!take(&stream,&record,sizeof(record)))goto bad;
            if(record.used>1 || record.dir>1 ||
               (record.used && (record.size>=sizeof(record.data) ||
                                record.data[record.size])))goto bad;
            if(!record.used)continue;
            File *f=&staging[i];
            memcpy(f->path,record.path,PATH_CAP);
            f->used=1;f->dir=record.dir;f->size=record.size;
            if(!f->dir) {
                f->data=allocate_data(f->size);
                if(!f->data)goto bad;
                memcpy(f->data,record.data,f->size+1u);
            }
        }
    } else {
        for(unsigned i=0;i<header.entries;i++) {
            Record record;
            if(!take(&stream,&record,sizeof(record)) ||
               record.index>=MAX_FILES || staging[record.index].used ||
               record.dir>1 || record.size>FS_DATA_BYTES ||
               (record.dir && record.size))goto bad;
            File *f=&staging[record.index];
            memcpy(f->path,record.path,PATH_CAP);
            f->used=1;f->dir=record.dir;f->size=record.size;
            if(!f->dir) {
                f->data=allocate_data(f->size);
                if(!f->data || !take(&stream,f->data,f->size))goto bad;
                f->data[f->size]=0;
            }
        }
    }
    if(stream.pos!=stream.total || stream.hash!=header.hash ||
       !valid_state(staging))goto bad;
    *gen=header.generation;
    return true;
bad:
    drop_state(staging);
    return false;
}
bool fs_load(void) {
    uint32_t best=0,g=0;
    bool found=false;
    for(unsigned i=0;i<2;i++) {
        if(!read_slot(disk_boot_index(),FS_SLOT_START+i*FS_SLOT_SECTORS,&g))continue;
        if(!found || (int32_t)(g-best)>0) {
            drop_state(files);
            memcpy(files,staging,sizeof(files));
            memset(staging,0,sizeof(staging));
            best=g;found=true;
        } else drop_state(staging);
    }
    generation=best;
    fs_dirty=false;
    return found;
}
bool fs_save_to(unsigned destination) {
    if(!valid_state(files))return false;
    uint32_t entries=0;
    size_t n=state_bytes(files,&entries);
    if(!n || n>FS_DATA_BYTES)return false;
    uint32_t next=generation+1u;
    uint32_t base=FS_SLOT_START+(next&1u)*FS_SLOT_SECTORS;
    Snapshot header;
    memset(&header,0,sizeof(header));
    /* An interrupted write cannot be mistaken for a committed snapshot. */
    if(!disk_write_on(destination,base,&header))return false;
    Stream stream; writer_init(&stream,destination,base);
    for(unsigned i=0;i<MAX_FILES;i++)if(files[i].used) {
        Record record={0};
        record.index=i;
        memcpy(record.path,files[i].path,PATH_CAP);
        record.size=files[i].dir?0:files[i].size;
        record.dir=files[i].dir;
        if(!put(&stream,&record,sizeof(record)) ||
           (!files[i].dir && !put(&stream,files[i].data,files[i].size)))
            return false;
    }
    if(!writer_finish(&stream) || stream.pos!=n)return false;
    memcpy(header.magic,"LUNAFS2",7);
    header.version=2; header.generation=next;
    header.entries=entries;header.bytes=(uint32_t)n;header.hash=stream.hash;
    if(!disk_write_on(destination,base,&header) || !disk_flush(destination))
        return false;
    generation=next;fs_dirty=false;
    return true;
}
bool fs_save(void) { return fs_save_to(disk_boot_index()); }
void fs_init(void) {
    if (fs_load()) {
        serial_write("LunaFS: restored snapshot\n");
        return;
    }
    memset(files, 0, sizeof(files));
    files[0].used = files[0].dir = 1;
    strcpy(files[0].path, "/");
    fs_create("/home", true);
    fs_create("/home/luna", true);
    fs_create("/etc", true);
    fs_create("/tmp", true);
    fs_create("/etc/licenses", true);
    fs_create("/etc/tls", true);
    fs_write("/etc/licenses/Inter-Tight.txt", font_license, false);
    fs_write("/etc/licenses/Lucide.txt", icon_license, false);
    for (unsigned i = 0; i < sizeof(extra_notices) / sizeof(extra_notices[0]); i++)
        fs_write(extra_notices[i].name, extra_notices[i].text, false);
    fs_create("/www", true);
    fs_write(
        "/home/luna/welcome.txt",
        "Welcome to LunaOS.\nAn independent operating system, written in C.\n\nSuper App Search | "
        "Alt+Tab Switch | Alt+F4 Close\nType help in "
        "lunaSH to explore real "
        "commands.\nRun sync to save files to the boot disk.\n",
        false);
    fs_write("/etc/luna-release",
             "LunaOS " VERSION "\nArchitecture: i386 / BIOS\nKernel: Luna native\n", false);
    fs_write("/www/index.html",
             "<h1>A quieter corner of the web.</h1><p>Welcome to Luna Browser.</p><p>This native "
             "reader opens local documents and verified HTTPS text.</p><p>A complete modern "
             "DOM/CSS web engine is not integrated yet.</p>",
             false);
    fs_write("/home/luna/hello.lsh", "echo Hello from lunaSH\npwd\nls\n", false);
    fs_dirty = false;
}

bool fs_verify_on(unsigned device) {
    uint32_t gen=0;
    for(unsigned i=0;i<2;i++) {
        if(!read_slot(device,FS_SLOT_START+i*FS_SLOT_SECTORS,&gen))continue;
        bool same=true;
        for(unsigned j=0;j<MAX_FILES;j++) {
            const File *a=&files[j],*b=&staging[j];
            if(a->used!=b->used || a->dir!=b->dir || a->size!=b->size ||
               (a->used && (strcmp(a->path,b->path) ||
                (!a->dir && memcmp(a->data,b->data,a->size))))) {
                same=false;break;
            }
        }
        drop_state(staging);
        if(same)return true;
    }
    return false;
}
