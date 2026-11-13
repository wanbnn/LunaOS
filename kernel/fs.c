#include "install.h"
#include "luna.h"
#include "notices.h"
#include "heap.h"
#include "fs_volume.h"
/* Clock is supplied by the LunaOS runtime. Host filesystem fixtures use 0. */
extern uint32_t rtc_epoch_seconds(void) __attribute__((weak));
static uint32_t fs_epoch(void) { return rtc_epoch_seconds ? rtc_epoch_seconds() : 0u; }
File *files;
int fs_file_capacity;
char cwd[PATH_CAP] = "/home/luna";
bool fs_dirty;
static FileState staging;
static size_t data_limit = (4096u - 1u) * 512u;
static uint32_t generation;
static uint32_t mounted_base;
static bool fs_locked;
static unsigned disk_readers;
static FileState *backups;
#define FILE_CACHE_BYTES (64u * 1024u)
#define FS_IO_BYTES 16384u

bool fs_read_bytes(const File *file, uint32_t offset, void *out, size_t bytes) {
    if (!file || file->dir || offset > file->size || bytes > file->size - offset || (bytes && !out))
        return false;
    if (!bytes)
        return true;
    if (file->data) {
        memcpy(out, file->data + offset, bytes);
        return true;
    }
    uint8_t *destination = out;
    uint64_t position = file->disk_offset + offset;
    unsigned disk = file->disk;
    /* The read may sleep. Keep its snapshot slot from being reused, even if
     * a writer already staging the other slot commits in the meantime. */
    disk_readers++;
    while (bytes) {
        uint32_t sector = (uint32_t)(position / 512u);
        unsigned skip = (unsigned)(position % 512u);
        size_t count;
        if (!skip && bytes >= 512u) {
            unsigned sectors = bytes / 512u;
            if (sectors > 32u)
                sectors = 32u;
            if (!disk_read_blocks(disk, sector, sectors, destination))
                break;
            count = sectors * 512u;
        } else {
            uint8_t block[512];
            count = 512u - skip;
            if (count > bytes)
                count = bytes;
            if (!disk_read_on(disk, sector, block))
                break;
            memcpy(destination, block + skip, count);
        }
        destination += count;
        position += count;
        bytes -= count;
    }
    disk_readers--;
    return bytes == 0;
}
bool fs_read_text(const File *file, char *out, size_t capacity) {
    if (!file || file->dir || !out || !capacity || file->size >= capacity)
        return false;
    uint32_t size = file->size;
    if (!fs_read_bytes(file, 0, out, size))
        return false;
    out[size] = 0;
    return true;
}
bool fs_materialize(File *file) {
    if (!file || file->dir)
        return false;
    if (file->data)
        return true;
    if (file->size == UINT32_MAX)
        return false;
    File snapshot = *file;
    File *table = files;
    char *data = heap_allocate_owned((size_t)snapshot.size + 1u, 1);
    if (!data)
        return false;
    if (!fs_read_bytes(&snapshot, 0, data, snapshot.size) || files != table ||
        !file->used || file->dir || file->size != snapshot.size ||
        file->disk != snapshot.disk || file->disk_offset != snapshot.disk_offset ||
        strcmp(file->path, snapshot.path)) {
        heap_release(data);
        return false;
    }
    if (file->data) {
        heap_release(data);
        return true;
    }
    data[snapshot.size] = 0;
    file->data = data;
    return true;
}
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
#define FS_SLOT_START LUNA_VOLUME_START
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
} RecordV2;
typedef struct {
    uint32_t index;
    char path[PATH_CAP];
    uint32_t size, dir, mode, mtime, ctime;
} Record;
typedef struct {
    char path[PATH_CAP];
    uint32_t used, size, dir;
    char data[8192];
} LegacyFile;
_Static_assert(sizeof(Snapshot)==512, "LunaFS header");
_Static_assert(sizeof(RecordV2)==140, "LunaFS2 legacy entry layout");
_Static_assert(sizeof(Record)==152, "LunaFS3 metadata entry layout");
_Static_assert(sizeof(LegacyFile)==FS_V1_FILE_BYTES, "LunaFS1 legacy entry layout");

static void unpin_backup(FileState *state) {
    if (!state->registered)
        return;
    FileState **link = &backups;
    while (*link && *link != state)
        link = &(*link)->next_backup;
    if (*link)
        *link = state->next_backup;
    state->registered = false;
    state->next_backup = 0;
}
static bool pinned_slot(unsigned disk, uint32_t base, uint32_t sectors) {
    uint64_t start = (uint64_t)base * 512u, end = ((uint64_t)base + sectors) * 512u;
    for (FileState *backup = backups; backup; backup = backup->next_backup)
        for (int i = 0; i < backup->capacity; i++) {
            const File *file = &backup->files[i];
            if (file->used && !file->dir && !file->data && file->disk == disk &&
                file->disk_offset >= start && file->disk_offset < end)
                return true;
        }
    return false;
}
static void drop_state(FileState *state) {
    unpin_backup(state);
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
    FileState live = {.files = files, .capacity = fs_file_capacity};
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
        if (!f->dir && ((f->data && f->data[f->size]) || (!f->data && !f->disk_offset)))
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
    if (fs_locked || disk_readers)
        return -1;
    if (!dest || source != files || dest->files == files)
        return -1;
    drop_state(dest);
    if (!reserve_state(dest, (unsigned)fs_file_capacity))
        return -1;
    for (int i = 0; i < fs_file_capacity; i++) {
        dest->files[i] = source[i];
        dest->files[i].data = 0;
        if (source[i].used && source[i].data) {
            dest->files[i].data = allocate_data(source[i].size);
            if (!dest->files[i].data) {
                drop_state(dest);
                return -1;
            }
            memcpy(dest->files[i].data, source[i].data, source[i].size + 1u);
        }
    }
    dest->snapshot_base = mounted_base;
    dest->registered = true;
    dest->next_backup = backups;
    backups = dest;
    return 0;
}
void fs_discard_state(FileState *state) {
    if (state)
        drop_state(state);
}
void fs_restore_state(FileState *backup) {
    if (fs_locked || disk_readers)
        return;
    if (!backup || !backup->files)
        return;
    FileState live = {.files = files, .capacity = fs_file_capacity};
    drop_state(&live);
    unpin_backup(backup);
    files = backup->files;
    fs_file_capacity = backup->capacity;
    mounted_base = backup->snapshot_base;
    *backup = (FileState){0};
    fs_dirty = true;
}

int fs_create(const char *path, bool dir) {
    if (fs_locked || disk_readers)
        return -1;
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
            files[i].mode = dir ? 0040755u : 0100644u;
            files[i].mtime = files[i].ctime = fs_epoch();
            files[i].data = buffer;
            if (buffer)
                buffer[0] = 0;
            fs_dirty = true;
            return 0;
        }
    return -4;
}
int fs_remove(const char *path) {
    if (fs_locked || disk_readers)
        return -1;
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
    if (fs_locked || disk_readers)
        return -1;
    if((n && !data) || n>data_limit || n>UINT32_MAX)return -1;
    File *f=fs_find(path);
    if(f && f->dir)return -1;
    size_t old=f?f->size:0u, off=add?old:0u;
    if(off>data_limit-n)return -2;
    size_t wanted=off+n, used=state_bytes(files,fs_file_capacity,data_limit,0);
    if(!used)return -2;
    size_t overhead=used-old+(f?0u:sizeof(Record));
    if(overhead>data_limit || wanted>data_limit-overhead)return -2;
    if (wanted > FILE_CACHE_BYTES) {
        if (!fs_begin_write(path, add, 0))
            return -3;
        if (!fs_write_part(data, n) || !fs_finish_write()) {
            fs_abort_write(0);
            return -3;
        }
        return 0;
    }
    char *new_data=allocate_data(wanted);
    if(!new_data)return -3;
    if (off) {
        fs_locked = true;
        bool read_ok = fs_read_bytes(f, 0, new_data, off);
        fs_locked = false;
        if (!read_ok) {
            heap_release(new_data);
            return -3;
        }
    }
    if(n)memcpy(new_data+off,data,n);
    new_data[wanted]=0; /* Text callers retain a sentinel; bytes may include NUL. */
    if(!f) {
        if(fs_create(path,false)) { heap_release(new_data);return -1; }
        f=fs_find(path);
    }
    if(f->data)heap_release(f->data);
    f->data=new_data;
    f->size=(uint32_t)wanted;
    f->mtime=f->ctime=fs_epoch();
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
    uint8_t buffer[FS_IO_BYTES];
    size_t pos, total;
    unsigned cursor, filled;
    uint32_t hash;
    bool ok, writing;
} Stream;
static void writer_init(Stream *s,unsigned disk,uint32_t base,const FsVolume *volume) {
    *s=(Stream){.disk=disk,.base=base,.sector=1,.hash=2166136261u,.ok=true,.writing=true,
        .sectors=volume->slot_sectors,.limit=fs_volume_bytes(volume)};
}
static void reader_init(Stream *s,unsigned disk,uint32_t base,size_t bytes,const FsVolume *volume) {
    *s = (Stream){.disk = disk,
                  .base = base,
                  .sector = 1,
                  .total = bytes,
                  .cursor = FS_IO_BYTES,
                  .hash = 2166136261u,
                  .ok = true,
                  .writing = false,
                  .sectors = volume->slot_sectors,
                  .limit = fs_volume_bytes(volume)};
}
static bool put(Stream *s,const void *data,size_t n) {
    if(!s->ok || n>s->limit-s->pos)return false;
    const uint8_t *p=data;
    s->hash=hash_more(s->hash,data,n);
    s->pos+=n;
    while(n) {
        unsigned room = FS_IO_BYTES - s->cursor;
        unsigned count=n<room?(unsigned)n:room;
        memcpy(s->buffer+s->cursor,p,count);
        s->cursor+=count;
        p+=count;n-=count;
        if (s->cursor == FS_IO_BYTES) {
            if (s->sectors - s->sector < FS_IO_BYTES / 512u ||
                !disk_write_blocks(s->disk, s->base + s->sector, FS_IO_BYTES / 512u, s->buffer))
                return s->ok=false;
            s->sector += FS_IO_BYTES / 512u;
            s->cursor=0;
        }
    }
    return true;
}
static bool writer_finish(Stream *s) {
    if (!s->ok)
        return false;
    if (s->cursor) {
        unsigned sectors = (s->cursor + 511u) / 512u;
        memset(s->buffer + s->cursor, 0, sectors * 512u - s->cursor);
        if (sectors > s->sectors - s->sector ||
            !disk_write_blocks(s->disk, s->base + s->sector, sectors, s->buffer))
            return false;
    }
    return true;
}
static bool take(Stream *s,void *data,size_t n) {
    if (!s->ok || n > s->total - s->pos)
        return false;
    uint8_t *out=data;
    while (n) {
        if (s->cursor >= s->filled) {
            size_t pending = s->total - s->pos;
            if (pending > FS_IO_BYTES)
                pending = FS_IO_BYTES;
            unsigned sectors = (pending + 511u) / 512u;
            if (sectors > FS_IO_BYTES / 512u)
                sectors = FS_IO_BYTES / 512u;
            if (sectors > s->sectors - s->sector ||
                !disk_read_blocks(s->disk, s->base + s->sector, sectors, s->buffer))
                return s->ok = false;
            s->sector += sectors;
            s->cursor = 0;
            s->filled = sectors * 512u;
        }
        size_t count = s->filled - s->cursor;
        if (count > n)
            count = n;
        memcpy(out,s->buffer+s->cursor,count);
        s->hash=hash_more(s->hash,out,count);
        s->cursor += count;
        s->pos += count;
        out += count;
        n -= count;
    }
    return true;
}
static bool put_file(Stream *stream, const File *file) {
    uint8_t buffer[FS_IO_BYTES];
    for (uint32_t offset = 0; offset < file->size;) {
        size_t n = file->size - offset;
        if (n > sizeof(buffer))
            n = sizeof(buffer);
        if (!fs_read_bytes(file, offset, buffer, n) || !put(stream, buffer, n))
            return false;
        offset += (uint32_t)n;
    }
    return true;
}
static void relocate_file(File *file, unsigned disk, uint64_t offset) {
    file->disk = disk;
    file->disk_offset = offset;
}
static bool read_slot(unsigned device,const FsVolume *volume,unsigned slot,uint32_t *gen) {
    drop_state(&staging);
    uint32_t base = volume->start + slot * volume->slot_sectors;
    size_t limit = fs_volume_bytes(volume);
    Snapshot header;
    if (!disk_read_on(device, base, &header))
        return false;
    bool legacy = !memcmp(header.magic, "LUNAFS1", 7) && header.version == 1;
    bool modern = !memcmp(header.magic, "LUNAFS2", 7) &&
        (header.version == 2 || header.version == 3);
    if (!legacy && !modern)
        return false;
    if (!header.bytes || header.bytes > limit ||
        (legacy && header.bytes != LEGACY_FILES * sizeof(LegacyFile)) ||
        (modern && (!header.entries ||
         header.entries > header.bytes / (header.version == 2 ?
             sizeof(RecordV2) : sizeof(Record)))))
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
            f->mode = record.dir ? 0040755u : 0100644u;
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
            Record record = {0};
            if (header.version == 2) {
                RecordV2 previous;
                if (!take(&stream, &previous, sizeof(previous)))
                    goto bad;
                record.index = previous.index;
                memcpy(record.path, previous.path, PATH_CAP);
                record.size = previous.size;
                record.dir = previous.dir;
                record.mode = record.dir ? 0040755u : 0100644u;
            } else if (!take(&stream, &record, sizeof(record)))
                goto bad;
            if (record.index >= indices || record.dir > 1 || record.size > limit ||
                (record.dir && record.size) ||
                (header.version == 3 && record.mode &&
                 ((record.mode & 0170000u) != (record.dir ? 0040000u : 0100000u))))
                goto bad;
            if (!reserve_state(&staging, record.index + 1u) || staging.files[record.index].used)
                goto bad;
            File *f = &staging.files[record.index];
            memcpy(f->path, record.path, PATH_CAP);
            f->used = 1;
            f->dir = record.dir;
            f->size = record.size;
            f->mode = record.mode ? record.mode : (record.dir ? 0040755u : 0100644u);
            f->mtime = record.mtime;
            f->ctime = record.ctime;
            if (!f->dir) {
                relocate_file(f, device, ((uint64_t)base + 1u) * 512u + stream.pos);
                if (f->size <= FILE_CACHE_BYTES) {
                    f->data = allocate_data(f->size);
                    if (!f->data || !take(&stream, f->data, f->size))
                        goto bad;
                    f->data[f->size] = 0;
                } else {
                    uint8_t buffer[FS_IO_BYTES];
                    uint32_t remaining = f->size;
                    while (remaining) {
                        size_t n = remaining > sizeof(buffer) ? sizeof(buffer) : remaining;
                        if (!take(&stream, buffer, n))
                            goto bad;
                        remaining -= (uint32_t)n;
                    }
                }
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
static bool load_snapshot(void) {
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
                FileState live = {.files = files, .capacity = fs_file_capacity};
                drop_state(&live);
                files = staging.files;
                fs_file_capacity = staging.capacity;
                staging = (FileState){0};
                mounted_base = current->start + slot * current->slot_sectors;
                best = g;
                found = true;
            } else
                drop_state(&staging);
        }
    }
    if (!found)
        mounted_base = 0;
    generation = best;
    fs_dirty = false;
    return found;
}
static bool save_snapshot(unsigned destination) {
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
    uint32_t base = volume.start;
    if (destination == disk_boot_index() && mounted_base == base)
        base += volume.slot_sectors;
    else if (destination != disk_boot_index() || !mounted_base)
        base += (next & 1u) * volume.slot_sectors;
    if (pinned_slot(destination, base, volume.slot_sectors))
        return false;
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
            record.mode = files[i].mode;
            record.mtime = files[i].mtime;
            record.ctime = files[i].ctime;
            if (!put(&stream, &record, sizeof(record)) ||
                (!files[i].dir && !put_file(&stream, &files[i])))
                return false;
        }
    if (!writer_finish(&stream) || stream.pos != n || !disk_flush(destination))
        return false;
    memcpy(header.magic, "LUNAFS2", 7);
    header.version = 3;
    header.generation = next;
    header.entries = entries;
    header.bytes = (uint32_t)n;
    header.hash = stream.hash;
    if (!disk_write_on(destination, base, &header) || !disk_flush(destination))
        return false;
    generation = next;
    if (destination == disk_boot_index()) {
        uint64_t offset = ((uint64_t)base + 1u) * 512u;
        for (int i = 0; i < fs_file_capacity; i++)
            if (files[i].used) {
                offset += sizeof(Record);
                if (!files[i].dir) {
                    relocate_file(&files[i], destination, offset);
                    offset += files[i].size;
                }
            }
        mounted_base = base;
    }
    fs_dirty = false;
    return true;
}
bool fs_load(void) {
    if (fs_locked || disk_readers)
        return false;
    fs_locked = true;
    bool result = load_snapshot();
    fs_locked = false;
    return result;
}
bool fs_save_to(unsigned destination) {
    if (fs_locked || disk_readers)
        return false;
    fs_locked = true;
    bool result = save_snapshot(destination);
    fs_locked = false;
    return result;
}
bool fs_save(void) {
    return fs_save_to(disk_boot_index());
}
void fs_init(void) {
    if (fs_locked || disk_readers)
        return;
    if (fs_load()) {
        serial_write("LunaFS: restored snapshot\n");
        return;
    }
    FileState old = {.files = files, .capacity = fs_file_capacity};
    drop_state(&old);
    files=0;fs_file_capacity=0;
    if (!reserve_files(LEGACY_FILES)) return;
    files[0].used = files[0].dir = 1;
    files[0].mode = 0040755u;
    files[0].ctime = files[0].mtime = fs_epoch();
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

static bool verify_snapshot(unsigned device) {
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
                    files[i].mode != match->mode ||
                    files[i].mtime != match->mtime || files[i].ctime != match->ctime)
                    same = false;
                if (same && !files[i].dir) {
                    uint8_t left[4096], right[4096];
                    for (uint32_t off = 0; same && off < match->size;) {
                        size_t n = match->size - off;
                        if (n > sizeof(left))
                            n = sizeof(left);
                        same = fs_read_bytes(&files[i], off, left, n) &&
                               fs_read_bytes(match, off, right, n) && !memcmp(left, right, n);
                        off += (uint32_t)n;
                    }
                }
            }
        drop_state(&staging);
        if (same)
            return true;
    }
    return false;
}

bool fs_verify_on(unsigned device) {
    if (fs_locked)
        return false;
    fs_locked = true;
    bool result = verify_snapshot(device);
    fs_locked = false;
    return result;
}

/* One writer stages a complete replacement snapshot. Readers keep using the
 * previous committed slot until the new header has reached stable storage. */
static struct {
    bool active;
    uint32_t owner, base, next, entries, bytes;
    int target;
    unsigned disk;
    FsVolume volume;
    Stream stream;
    size_t record_pos, payload_pos;
    uint32_t prefix_hash, payload_hash;
    uint32_t meta_mode, meta_mtime, meta_ctime;
    char path[PATH_CAP];
} transaction;

void fs_abort_write(uint32_t owner) {
    if (transaction.active && (!owner || transaction.owner == owner)) {
        transaction.active = false;
        fs_locked = false;
    }
}
bool fs_write_part(const void *data, size_t bytes) {
    if (!transaction.active)
        return false;
    if ((bytes && !data) || bytes > UINT32_MAX - transaction.bytes ||
        !put(&transaction.stream, data, bytes)) {
        transaction.stream.ok = false;
        return false;
    }
    transaction.payload_hash = hash_more(transaction.payload_hash, data, bytes);
    transaction.bytes += (uint32_t)bytes;
    return true;
}
bool fs_begin_write(const char *path, bool append_data, uint32_t owner) {
    if (fs_locked || disk_readers)
        return false;
    char canonical[PATH_CAP], parent[PATH_CAP];
    if (!fs_path(path, canonical))
        return false;
    File *old = fs_find(canonical);
    if (old && old->dir)
        return false;
    strcpy(parent, canonical);
    char *slash = 0;
    for (char *p = parent; *p; p++)
        if (*p == '/')
            slash = p;
    if (!slash)
        return false;
    if (slash == parent)
        parent[1] = 0;
    else
        *slash = 0;
    File *directory = fs_find(parent);
    if (!directory || !directory->dir)
        return false;
    int target = old ? (int)(old - files) : 0;
    if (!old) {
        while (target < fs_file_capacity && files[target].used)
            target++;
        if (target == fs_file_capacity && !reserve_files((unsigned)target + 1u))
            return false;
    }
    fs_locked = true;
    memset(&transaction, 0, sizeof(transaction));
    transaction.active = true;
    transaction.owner = owner;
    transaction.target = target;
    transaction.meta_mode = old ? old->mode : 0100644u;
    transaction.meta_mtime = transaction.meta_ctime = fs_epoch();
    transaction.disk = disk_boot_index();
    strcpy(transaction.path, canonical);
    if (!fs_volume(transaction.disk, true, &transaction.volume))
        goto fail;
    transaction.base = transaction.volume.start;
    if (mounted_base == transaction.base)
        transaction.base += transaction.volume.slot_sectors;
    if (pinned_slot(transaction.disk, transaction.base, transaction.volume.slot_sectors))
        goto fail;
    transaction.next = generation + 1u;
    Snapshot empty = {0};
    if (!disk_write_on(transaction.disk, transaction.base, &empty) || !disk_flush(transaction.disk))
        goto fail;
    writer_init(&transaction.stream, transaction.disk, transaction.base, &transaction.volume);
    for (int i = 0; i < fs_file_capacity; i++)
        if (files[i].used && i != target) {
            Record record = {0};
            record.index = transaction.entries++;
            memcpy(record.path, files[i].path, PATH_CAP);
            record.size = files[i].size;
            record.dir = files[i].dir;
            record.mode = files[i].mode;
            record.mtime = files[i].mtime;
            record.ctime = files[i].ctime;
            if (!put(&transaction.stream, &record, sizeof(record)) ||
                (!files[i].dir && !put_file(&transaction.stream, &files[i])))
                goto fail;
        }
    Record record = {0};
    record.index = transaction.entries++;
    strcpy(record.path, canonical);
    record.mode = transaction.meta_mode;
    record.mtime = transaction.meta_mtime;
    record.ctime = transaction.meta_ctime;
    transaction.record_pos = transaction.stream.pos;
    transaction.prefix_hash = transaction.stream.hash;
    transaction.payload_hash = 2166136261u;
    if (!put(&transaction.stream, &record, sizeof(record)))
        goto fail;
    transaction.payload_pos = transaction.stream.pos;
    if (append_data && files[target].used) {
        uint8_t buffer[FS_IO_BYTES];
        for (uint32_t offset = 0; offset < files[target].size;) {
            size_t n = files[target].size - offset;
            if (n > sizeof(buffer))
                n = sizeof(buffer);
            if (!fs_read_bytes(&files[target], offset, buffer, n) || !fs_write_part(buffer, n))
                goto fail;
            offset += (uint32_t)n;
        }
    }
    return true;
fail:
    fs_abort_write(0);
    return false;
}
static bool patch_record(void) {
    Record record = {0};
    record.index = transaction.entries - 1u;
    strcpy(record.path, transaction.path);
    record.size = transaction.bytes;
    record.mode = transaction.meta_mode;
    record.mtime = transaction.meta_mtime;
    record.ctime = transaction.meta_ctime;
    const uint8_t *data = (const void *)&record;
    size_t remaining = sizeof(record);
    uint64_t position = ((uint64_t)transaction.base + 1u) * 512u + transaction.record_pos;
    while (remaining) {
        uint8_t block[512];
        unsigned skip = position % 512u;
        size_t n = 512u - skip;
        if (n > remaining)
            n = remaining;
        uint32_t sector = position / 512u;
        if (!disk_read_on(transaction.disk, sector, block))
            return false;
        memcpy(block + skip, data, n);
        if (!disk_write_on(transaction.disk, sector, block))
            return false;
        data += n;
        position += n;
        remaining -= n;
    }
    return true;
}
bool fs_finish_write(void) {
    if (!transaction.active)
        return false;
    if (!writer_finish(&transaction.stream) || !patch_record() || !disk_flush(transaction.disk) ||
        transaction.stream.pos > UINT32_MAX)
        goto fail;
    /* The final size replaces a placeholder record. Re-read the staged payload
     * to authenticate exactly the bytes that will be visible after commit. */
    Stream reader;
    reader_init(&reader, transaction.disk, transaction.base, transaction.stream.pos,
                &transaction.volume);
    uint8_t buffer[FS_IO_BYTES];
    while (reader.pos < transaction.record_pos) {
        size_t n = transaction.record_pos - reader.pos;
        if (n > sizeof(buffer))
            n = sizeof(buffer);
        if (!take(&reader, buffer, n))
            goto fail;
    }
    if (reader.hash != transaction.prefix_hash)
        goto fail;
    Record actual, expected = {0};
    expected.index = transaction.entries - 1u;
    strcpy(expected.path, transaction.path);
    expected.size = transaction.bytes;
    expected.mode = transaction.meta_mode;
    expected.mtime = transaction.meta_mtime;
    expected.ctime = transaction.meta_ctime;
    if (!take(&reader, &actual, sizeof(actual)) || memcmp(&actual, &expected, sizeof(actual)))
        goto fail;
    uint32_t payload_hash = 2166136261u;
    while (reader.pos < reader.total) {
        size_t n = reader.total - reader.pos;
        if (n > sizeof(buffer))
            n = sizeof(buffer);
        if (!take(&reader, buffer, n))
            goto fail;
        payload_hash = hash_more(payload_hash, buffer, n);
    }
    if (payload_hash != transaction.payload_hash)
        goto fail;
    Snapshot header = {0};
    memcpy(header.magic, "LUNAFS2", 7);
    header.version = 3;
    header.generation = transaction.next;
    header.bytes = transaction.stream.pos;
    header.hash = reader.hash;
    header.entries = transaction.entries;
    if (!disk_write_on(transaction.disk, transaction.base, &header) ||
        !disk_flush(transaction.disk))
        goto fail;
    uint64_t offset = ((uint64_t)transaction.base + 1u) * 512u;
    for (int i = 0; i < fs_file_capacity; i++)
        if (files[i].used && i != transaction.target) {
            offset += sizeof(Record);
            if (!files[i].dir) {
                relocate_file(&files[i], transaction.disk, offset);
                offset += files[i].size;
            }
        }
    File *file = &files[transaction.target];
    if (file->data)
        heap_release(file->data);
    memset(file, 0, sizeof(*file));
    file->used = 1;
    file->size = transaction.bytes;
    file->mode = transaction.meta_mode;
    file->mtime = transaction.meta_mtime;
    file->ctime = transaction.meta_ctime;
    strcpy(file->path, transaction.path);
    relocate_file(file, transaction.disk,
                  ((uint64_t)transaction.base + 1u) * 512u + transaction.payload_pos);
    mounted_base = transaction.base;
    generation = transaction.next;
    data_limit = fs_volume_bytes(&transaction.volume);
    fs_dirty = false;
    fs_abort_write(0);
    if (file->size <= FILE_CACHE_BYTES)
        fs_materialize(file);
    return true;
fail:
    fs_abort_write(0);
    return false;
}
int fs_copy(const char *source, const char *destination) {
    File *file = fs_find(source);
    if (!file || file->dir)
        return -1;
    char path[PATH_CAP];
    if (!fs_path(destination, path) || !strcmp(path, file->path))
        return -1;
    int index = file - files;
    if (!fs_begin_write(path, false, 0))
        return -1;
    uint8_t buffer[FS_IO_BYTES];
    for (uint32_t offset = 0; offset < files[index].size;) {
        size_t n = files[index].size - offset;
        if (n > sizeof(buffer))
            n = sizeof(buffer);
        if (!fs_read_bytes(&files[index], offset, buffer, n) || !fs_write_part(buffer, n)) {
            fs_abort_write(0);
            return -1;
        }
        offset += (uint32_t)n;
    }
    return fs_finish_write() ? 0 : -1;
}

int fs_rename(const char *source, const char *destination) {
    if (fs_locked || disk_readers)
        return -1;
    char path[PATH_CAP], parent[PATH_CAP];
    if (!fs_path(destination, path))
        return -1;
    File *file = fs_find(source);
    if (!file || file->dir || !strcmp(file->path, path))
        return -1;
    strcpy(parent, path);
    char *slash = 0;
    for (char *p = parent; *p; p++)
        if (*p == '/')
            slash = p;
    if (!slash)
        return -1;
    if (slash == parent)
        parent[1] = 0;
    else
        *slash = 0;
    File *directory = fs_find(parent), *target = fs_find(path);
    if (!directory || !directory->dir || (target && target->dir))
        return -1;
    if (target) {
        if (target->data)
            heap_release(target->data);
        memset(target, 0, sizeof(*target));
    }
    strcpy(file->path, path);
    file->ctime = fs_epoch();
    fs_dirty = true;
    return 0;
}

/* Persisted POSIX-like metadata. chmod does not yet enforce access control.
 * Modifications commit together with file bytes in LunaFS snapshot v3. */
int fs_chmod(const char *path, uint32_t mode) {
    if (fs_locked || disk_readers || (mode & ~0777u))
        return -1;
    File *f = fs_find(path);
    if (!f) return -1;
    f->mode = (f->dir ? 0040000u : 0100000u) | mode;
    f->ctime = fs_epoch();
    fs_dirty = true;
    return 0;
}
int fs_set_mtime(const char *path, uint32_t unix_seconds) {
    if (fs_locked || disk_readers)
        return -1;
    File *f = fs_find(path);
    if (!f) return -1;
    f->mtime = unix_seconds;
    f->ctime = fs_epoch();
    fs_dirty = true;
    return 0;
}
