#include "install.h"
#include "luna.h"
#include "notices.h"
#include "heap.h"
#include "fs_volume.h"
File *files;
int fs_file_capacity;
char cwd[PATH_CAP] = "/home/luna";
bool fs_dirty;
static FileState staging;
static size_t data_limit = (4096u - 1u) * 512u;
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
    for (int i = 0; i < fs_file_capacity; i++)
        if (files[i].used && !strcmp(files[i].path, p))
            return &files[i];
    return 0;
}
/* Snapshot records retain the LunaFS2 layout. Directory storage and the
 * data volume grow independently of the historical 64-entry/2 MiB defaults. */
#define FS_SLOT_START 8192u
#define FS_SLOT_SECTORS 4096u
#define LEGACY_FILES 64u
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

static void drop_state(FileState *state) {
    for (int i = 0; i < state->capacity; i++) {
        if (state->files[i].data)
            heap_release(state->files[i].data);
    }
    if (state->files)
        heap_release(state->files);
    *state = (FileState){0};
}
static bool reserve_state(FileState *state, unsigned required) {
    if (required <= (unsigned)state->capacity)
        return true;
    unsigned capacity = state->capacity ? (unsigned)state->capacity : LEGACY_FILES;
    while (capacity < required) {
        if (capacity > UINT32_MAX / sizeof(File) / 2u)
            return false;
        capacity *= 2u;
    }
    File *next = heap_allocate_owned(capacity * sizeof(File), 1);
    if (!next)
        return false;
    memset(next, 0, capacity * sizeof(File));
    if (state->files) {
        memcpy(next, state->files, state->capacity * sizeof(File));
        heap_release(state->files);
    }
    state->files = next;
    state->capacity = (int)capacity;
    return true;
}
static bool reserve_files(unsigned required) {
    FileState live = {files, fs_file_capacity};
    if (!reserve_state(&live, required))
        return false;
    files = live.files;
    fs_file_capacity = live.capacity;
    return true;
}
static char *allocate_data(size_t bytes) {
    if (bytes == SIZE_MAX)
        return 0;
    return heap_allocate_owned(bytes + 1u, 1);
}
static bool has_nul(const char *s, size_t size) {
    for (size_t i = 0; i < size; i++)
        if (!s[i])
            return true;
    return false;
}
static bool valid_state(const File *state, int capacity, size_t limit) {
    bool root = false;
    for (unsigned i = 0; i < (unsigned)capacity; i++) {
        const File *f = &state[i];
        if (!f->used)
            continue;
        if (f->used != 1 || f->dir > 1 || f->size > limit || !has_nul(f->path, PATH_CAP) ||
            f->path[0] != '/')
            return false;
        if (f->dir && (f->size || f->data))
            return false;
        if (!f->dir && (!f->data || f->data[f->size]))
            return false;
        if (!strcmp(f->path, "/")) {
            if (root || !f->dir)
                return false;
            root = true;
        }
        for (unsigned j = 0; j < i; j++)
            if (state[j].used && !strcmp(f->path, state[j].path))
                return false;
    }
    return root;
}
static size_t state_bytes(const File *state, int capacity, size_t limit, uint32_t *entries) {
    size_t total = 0;
    uint32_t count = 0;
    for (unsigned i = 0; i < (unsigned)capacity; i++)
        if (state[i].used) {
            size_t bytes = state[i].dir ? 0u : state[i].size;
            if (limit < sizeof(Record) || bytes > limit - sizeof(Record))
                return 0;
            size_t additional = sizeof(Record) + bytes;
            if (additional > limit - total)
                return 0;
            total += additional;
            count++;
        }
    if (entries)
        *entries = count;
    return total;
}
int fs_clone(FileState *dest, const File *source) {
    if (!dest || source != files || dest->files == files)
        return -1;
    drop_state(dest);
    if (!reserve_state(dest, (unsigned)fs_file_capacity))
        return -1;
    for (int i = 0; i < fs_file_capacity; i++) {
        dest->files[i] = source[i];
        dest->files[i].data = 0;
        if (source[i].used && !source[i].dir) {
            dest->files[i].data = allocate_data(source[i].size);
            if (!dest->files[i].data) {
                drop_state(dest);
                return -1;
            }
            memcpy(dest->files[i].data, source[i].data, source[i].size + 1u);
        }
    }
    return 0;
}
void fs_discard_state(FileState *state) {
    if (state)
        drop_state(state);
}
void fs_restore_state(FileState *backup) {
    if (!backup || !backup->files)
        return;
    FileState live = {files, fs_file_capacity};
    drop_state(&live);
    files = backup->files;
    fs_file_capacity = backup->capacity;
    *backup = (FileState){0};
    fs_dirty = true;
}

int fs_create(const char *path, bool dir) {
    char p[PATH_CAP], parent[PATH_CAP];
    if (!fs_path(path, p))
        return -1;
    if (fs_find(p))
        return -2;
    strcpy(parent, p);
    char *last = 0;
    for (char *s = parent; *s; s++)
        if (*s == '/')
            last = s;
    if (last == parent)
        parent[1] = 0;
    else if (last)
        *last = 0;
    File *parent_file = fs_find(parent);
    if (!parent_file || !parent_file->dir)
        return -3;
    size_t used = state_bytes(files, fs_file_capacity, data_limit, 0);
    if (data_limit < sizeof(Record) || used > data_limit - sizeof(Record))
        return -4;
    unsigned slot = 0;
    while (slot < (unsigned)fs_file_capacity && files[slot].used)
        slot++;
    if (slot == (unsigned)fs_file_capacity && !reserve_files(slot + 1u))
        return -4;
    for (unsigned i = slot; i < (unsigned)fs_file_capacity; i++)
        if (!files[i].used) {
            char *buffer = dir ? 0 : allocate_data(0);
            if (!dir && !buffer)
                return -4;
            memset(&files[i], 0, sizeof(File));
            strcpy(files[i].path, p);
            files[i].used = 1;
            files[i].dir = dir;
            files[i].data = buffer;
            if (buffer)
                buffer[0] = 0;
            fs_dirty = true;
            return 0;
        }
    return -4;
}
int fs_remove(const char *path) {
    File *f=fs_find(path);
    if(!f || !strcmp(f->path,"/") || !strcmp(f->path,cwd))return -1;
    size_t n=strlen(f->path);
    for(int i=0;i<fs_file_capacity;i++)
        if(files[i].used && !strncmp(files[i].path,f->path,n) && files[i].path[n]=='/')
            return -2;
    if(f->data)heap_release(f->data);
    memset(f,0,sizeof(*f));
    fs_dirty=true;
    return 0;
}
int fs_write_bytes(const char *path, const void *data, size_t n, bool add) {
    if((n && !data) || n>data_limit || n>UINT32_MAX)return -1;
    File *f=fs_find(path);
    if(f && f->dir)return -1;
    size_t old=f?f->size:0u, off=add?old:0u;
    if(off>data_limit-n)return -2;
    size_t wanted=off+n, used=state_bytes(files,fs_file_capacity,data_limit,0);
    if(!used)return -2;
    size_t overhead=used-old+(f?0u:sizeof(Record));
    if(overhead>data_limit || wanted>data_limit-overhead)return -2;
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
    uint32_t base, sector, sectors;
    size_t limit;
    uint8_t buffer[512];
    size_t pos, total;
    unsigned cursor;
    uint32_t hash;
    bool ok, writing;
} Stream;
static void writer_init(Stream *s,unsigned disk,uint32_t base,const FsVolume *volume) {
    *s=(Stream){.disk=disk,.base=base,.sector=1,.hash=2166136261u,.ok=true,.writing=true,
        .sectors=volume->slot_sectors,.limit=fs_volume_bytes(volume)};
}
static void reader_init(Stream *s,unsigned disk,uint32_t base,size_t bytes,const FsVolume *volume) {
    *s=(Stream){.disk=disk,.base=base,.sector=1,.total=bytes,.cursor=512,
                 .hash=2166136261u,.ok=true,.writing=false,
        .sectors=volume->slot_sectors,.limit=fs_volume_bytes(volume)};
}
static bool put(Stream *s,const void *data,size_t n) {
    if(!s->ok || n>s->limit-s->pos)return false;
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
            if(s->sector>=s->sectors ||
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
        if(s->sector>=s->sectors ||
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
            if(s->sector>=s->sectors ||
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
static bool read_slot(unsigned device,const FsVolume *volume,unsigned slot,uint32_t *gen) {
    drop_state(&staging);
    uint32_t base = volume->start + slot * volume->slot_sectors;
    size_t limit = fs_volume_bytes(volume);
    Snapshot header;
    if (!disk_read_on(device, base, &header))
        return false;
    bool legacy = !memcmp(header.magic, "LUNAFS1", 7) && header.version == 1;
    bool modern = !memcmp(header.magic, "LUNAFS2", 7) && header.version == 2;
    if (!legacy && !modern)
        return false;
    if (!header.bytes || header.bytes > limit ||
        (legacy && header.bytes != LEGACY_FILES * sizeof(LegacyFile)) ||
        (modern && (!header.entries || header.entries > header.bytes / sizeof(Record))))
        return false;
    if (!reserve_state(&staging, legacy ? LEGACY_FILES : header.entries))
        return false;
    Stream stream;
    reader_init(&stream, device, base, header.bytes, volume);
    if (legacy) {
        LegacyFile record;
        for (unsigned i = 0; i < LEGACY_FILES; i++) {
            if (!take(&stream, &record, sizeof(record)))
                goto bad;
            if (record.used > 1 || record.dir > 1 ||
                (record.used && (record.size >= sizeof(record.data) || record.data[record.size])))
                goto bad;
            if (!record.used)
                continue;
            File *f = &staging.files[i];
            memcpy(f->path, record.path, PATH_CAP);
            f->used = 1;
            f->dir = record.dir;
            f->size = record.size;
            if (!f->dir) {
                f->data = allocate_data(f->size);
                if (!f->data)
                    goto bad;
                memcpy(f->data, record.data, f->size + 1u);
            }
        }
    } else {
        unsigned indices = header.entries < LEGACY_FILES ? LEGACY_FILES : header.entries;
        for (unsigned i = 0; i < header.entries; i++) {
            Record record;
            if (!take(&stream, &record, sizeof(record)) || record.index >= indices ||
                record.dir > 1 || record.size > limit || (record.dir && record.size))
                goto bad;
            if (!reserve_state(&staging, record.index + 1u) || staging.files[record.index].used)
                goto bad;
            File *f = &staging.files[record.index];
            memcpy(f->path, record.path, PATH_CAP);
            f->used = 1;
            f->dir = record.dir;
            f->size = record.size;
            if (!f->dir) {
                f->data = allocate_data(f->size);
                if (!f->data || !take(&stream, f->data, f->size))
                    goto bad;
                f->data[f->size] = 0;
            }
        }
    }
    if (stream.pos != stream.total || stream.hash != header.hash ||
        !valid_state(staging.files, staging.capacity, limit))
        goto bad;
    *gen = header.generation;
    return true;
bad:
    drop_state(&staging);
    return false;
}
bool fs_load(void) {
    FsVolume volume;
    if (!fs_volume(disk_boot_index(), false, &volume))
        return false;
    data_limit = fs_volume_bytes(&volume);
    FsVolume legacy = {FS_SLOT_START, FS_SLOT_SECTORS, false, true};
    uint32_t best = 0, g = 0;
    bool found = false;
    for (unsigned area = 0; area < 2; area++) {
        const FsVolume *current = area ? &volume : &legacy;
        if (area && (!volume.extended || !volume.exists))
            continue;
        for (unsigned slot = 0; slot < 2; slot++) {
            if (!read_slot(disk_boot_index(), current, slot, &g))
                continue;
            if (!found || (int32_t)(g - best) > 0) {
                FileState live = {files, fs_file_capacity};
                drop_state(&live);
                files = staging.files;
                fs_file_capacity = staging.capacity;
                staging = (FileState){0};
                best = g;
                found = true;
            } else
                drop_state(&staging);
        }
    }
    generation = best;
    fs_dirty = false;
    return found;
}
bool fs_save_to(unsigned destination) {
    FsVolume volume;
    if (!fs_volume(destination, false, &volume))
        return false;
    size_t limit = fs_volume_bytes(&volume);
    if (!valid_state(files, fs_file_capacity, limit))
        return false;
    uint32_t entries = 0;
    size_t n = state_bytes(files, fs_file_capacity, limit, &entries);
    if (!n || n > UINT32_MAX || !fs_volume(destination, true, &volume))
        return false;
    uint32_t next = generation + 1u;
    uint32_t base = volume.start + (next & 1u) * volume.slot_sectors;
    Snapshot header = {0};
    if (!disk_write_on(destination, base, &header))
        return false;
    Stream stream;
    writer_init(&stream, destination, base, &volume);
    unsigned packed = 0;
    for (int i = 0; i < fs_file_capacity; i++)
        if (files[i].used) {
            Record record = {0};
            record.index = packed++;
            memcpy(record.path, files[i].path, PATH_CAP);
            record.size = files[i].dir ? 0 : files[i].size;
            record.dir = files[i].dir;
            if (!put(&stream, &record, sizeof(record)) ||
                (!files[i].dir && !put(&stream, files[i].data, files[i].size)))
                return false;
        }
    if (!writer_finish(&stream) || stream.pos != n || !disk_flush(destination))
        return false;
    memcpy(header.magic, "LUNAFS2", 7);
    header.version = 2;
    header.generation = next;
    header.entries = entries;
    header.bytes = (uint32_t)n;
    header.hash = stream.hash;
    if (!disk_write_on(destination, base, &header) || !disk_flush(destination))
        return false;
    generation = next;
    fs_dirty = false;
    return true;
}
bool fs_save(void) {
    return fs_save_to(disk_boot_index());
}
void fs_init(void) {
    if (fs_load()) {
        serial_write("LunaFS: restored snapshot\n");
        return;
    }
    FileState old={files,fs_file_capacity};drop_state(&old);
    files=0;fs_file_capacity=0;
    if (!reserve_files(LEGACY_FILES)) return;
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
    FsVolume volume;
    if (!fs_volume(device, false, &volume) || !volume.exists)
        return false;
    uint32_t gen = 0;
    for (unsigned slot = 0; slot < 2; slot++) {
        if (!read_slot(device, &volume, slot, &gen))
            continue;
        bool same = true;
        uint32_t live_entries = 0, staged_entries = 0;
        state_bytes(files, fs_file_capacity, data_limit, &live_entries);
        state_bytes(staging.files, staging.capacity, fs_volume_bytes(&volume), &staged_entries);
        if (live_entries != staged_entries)
            same = false;
        for (int i = 0; same && i < fs_file_capacity; i++)
            if (files[i].used) {
                const File *match = 0;
                for (int j = 0; j < staging.capacity; j++)
                    if (staging.files[j].used && !strcmp(files[i].path, staging.files[j].path)) {
                        match = &staging.files[j];
                        break;
                    }
                if (!match || files[i].dir != match->dir || files[i].size != match->size ||
                    (!files[i].dir && memcmp(files[i].data, match->data, match->size)))
                    same = false;
            }
        drop_state(&staging);
        if (same)
            return true;
    }
    return false;
}
