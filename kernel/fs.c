#include "install.h"
#include "luna.h"
#include "notices.h"
File files[MAX_FILES];
char cwd[PATH_CAP] = "/home/luna";
bool fs_dirty;
static File staging[MAX_FILES];
static uint32_t generation;
static uint32_t checksum(const void *p, size_t n) {
    const uint8_t *s = p;
    uint32_t h = 2166136261u;
    while (n--) {
        h ^= *s++;
        h *= 16777619;
    }
    return h;
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
    for (int i = 0; i < MAX_FILES; i++)
        if (files[i].used && !strcmp(files[i].path, p))
            return &files[i];
    return 0;
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
    File *f = fs_find(parent);
    if (!f || !f->dir)
        return -3;
    for (int i = 0; i < MAX_FILES; i++)
        if (!files[i].used) {
            memset(&files[i], 0, sizeof(File));
            strcpy(files[i].path, p);
            files[i].used = 1;
            files[i].dir = dir;
            fs_dirty = true;
            return 0;
        }
    return -4;
}
int fs_remove(const char *path) {
    File *f = fs_find(path);
    if (!f || !strcmp(f->path, "/") || !strcmp(f->path, cwd))
        return -1;
    size_t n = strlen(f->path);
    for (int i = 0; i < MAX_FILES; i++)
        if (files[i].used && !strncmp(files[i].path, f->path, n) && files[i].path[n] == '/')
            return -2;
    memset(f, 0, sizeof(*f));
    fs_dirty = true;
    return 0;
}
int fs_write(const char *path, const char *data, bool add) {
    File *f = fs_find(path);
    if (!f) {
        if (fs_create(path, false))
            return -1;
        f = fs_find(path);
    }
    if (!f || f->dir)
        return -1;
    size_t n = strlen(data), off = add ? f->size : 0;
    if (off + n >= FILE_CAP)
        return -2;
    memcpy(f->data + off, data, n + 1);
    f->size = off + n;
    fs_dirty = true;
    return 0;
}
static bool valid_files(void) {
    bool root = false;
    for (int i = 0; i < MAX_FILES; i++) {
        File *f = &staging[i];
        if (f->used > 1 || f->dir > 1)
            return false;
        if (!f->used)
            continue;
        bool end = false;
        for (int j = 0; j < PATH_CAP; j++)
            if (!f->path[j]) {
                end = true;
                break;
            }
        if (!end || f->path[0] != '/' || f->size >= FILE_CAP || f->data[f->size])
            return false;
        if (!strcmp(f->path, "/") && f->dir)
            root = true;
        for (int j = 0; j < i; j++)
            if (staging[j].used && !strcmp(f->path, staging[j].path))
                return false;
    }
    return root;
}
typedef struct {
    char magic[8];
    uint32_t version, generation, bytes, hash;
    uint8_t pad[488];
} Snapshot;
_Static_assert(sizeof(Snapshot) == 512, "snapshot sector");
static bool load_slot_on(unsigned device, uint32_t base, uint32_t *gen) {
    Snapshot h;
    if (!disk_read_on(device, base, &h) || strncmp(h.magic, "LUNAFS1", 8) || h.version != 1 ||
        h.bytes != sizeof(files))
        return false;
    uint8_t sec[512];
    uint8_t *p = (uint8_t *)staging;
    size_t off = 0;
    while (off < sizeof(staging)) {
        if (!disk_read_on(device, base + 1 + off / 512, sec))
            return false;
        size_t n = sizeof(staging) - off;
        if (n > 512)
            n = 512;
        memcpy(p + off, sec, n);
        off += n;
    }
    if (checksum(staging, sizeof(staging)) != h.hash || !valid_files())
        return false;
    *gen = h.generation;
    return true;
}
bool fs_load(void) {
    uint32_t best = 0, g;
    bool found = false;
    for (int i = 0; i < 2; i++)
        if (load_slot_on(disk_boot_index(), 8192 + i * 4096, &g) && (!found || g > best)) {
            memcpy(files, staging, sizeof(files));
            best = g;
            found = true;
        }
    generation = best;
    fs_dirty = false;
    return found;
}
bool fs_save_to(unsigned destination) {
    uint32_t next = generation + 1, base = 8192 + (next & 1) * 4096;
    Snapshot h;
    memset(&h, 0, sizeof(h));
    if (!disk_write_on(destination, base, &h))
        return false;
    uint8_t sec[512];
    const uint8_t *p = (const uint8_t *)files;
    size_t off = 0;
    while (off < sizeof(files)) {
        memset(sec, 0, 512);
        size_t n = sizeof(files) - off;
        if (n > 512)
            n = 512;
        memcpy(sec, p + off, n);
        if (!disk_write_on(destination, base + 1 + off / 512, sec))
            return false;
        off += n;
    }
    strcpy(h.magic, "LUNAFS1");
    h.version = 1;
    h.generation = next;
    h.bytes = sizeof(files);
    h.hash = checksum(files, sizeof(files));
    if (!disk_write_on(destination, base, &h))
        return false;
    if (!disk_flush(destination))
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
    uint32_t generation;
    for (unsigned i = 0; i < 2; i++)
        if (load_slot_on(device, 8192 + i * 4096, &generation) &&
            !memcmp(files, staging, sizeof(files)))
            return true;
    return false;
}
