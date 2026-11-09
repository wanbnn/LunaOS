#include "python_wheel.h"
#include "heap.h"

#define STBI_NO_STDIO
#include "../third_party/stb/stb_image.h"

#define LOCAL_HEADER 30u
#define CENTRAL_HEADER 46u
#define END_HEADER 22u
/* LunaFS writes up to this size in RAM, so an install can roll back without
 * committing intermediate changes to disk. */
#define WHEEL_ENTRY_BYTES (64u * 1024)
#define WHEEL_ENTRIES 512u

static bool suffix(const char *name, const char *ending) {
    size_t len = strlen(name), n = strlen(ending);
    return len >= n && !strcmp(name + len - n, ending);
}

static bool name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '+';
}

bool python_wheel_name(const char *name, char *dist_info, size_t cap) {
    const char *tag = suffix(name, "-py2.py3-none-any.whl") ? "-py2.py3-none-any.whl" :
                      "-py3-none-any.whl";
    if (!suffix(name, tag))
        return false;
    size_t stem_len = strlen(name) - strlen(tag);
    const char *version = strchr(name, '-');
    if (!version || version == name || (size_t)(version - name + 1) >= stem_len)
        return false;
    const char *build = strchr(version + 1, '-');
    size_t info_len = build && (size_t)(build - name) < stem_len ?
                      (size_t)(build - name) : stem_len;
    for (size_t i = 0; i < stem_len; ++i)
        if (!name_char(name[i]))
            return false;
    if (info_len <= (size_t)(version - name + 1) || info_len + sizeof(".dist-info") > cap)
        return false;
    memcpy(dist_info, name, info_len);
    strcpy(dist_info + info_len, ".dist-info");
    return true;
}

static unsigned le16(const uint8_t *p) {
    return p[0] | (unsigned)p[1] << 8;
}

static uint32_t le32(const uint8_t *p) {
    return le16(p) | (uint32_t)le16(p + 2) << 16;
}

static uint32_t crc32(const uint8_t *data, size_t n) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < n; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    return ~crc;
}

static const uint8_t *end_record(const uint8_t *zip, size_t size) {
    if (size < END_HEADER || size > PYTHON_WHEEL_BYTES)
        return NULL;
    for (size_t comment = 0; comment <= UINT16_MAX && comment <= size - END_HEADER; ++comment) {
        const uint8_t *p = zip + size - END_HEADER - comment;
        if (le32(p) == 0x06054b50 && le16(p + 20) == comment)
            return p;
    }
    return NULL;
}

bool python_package_path(const char *path, size_t len) {
    for (size_t start = 0, i = 0; i <= len; ++i) {
        if (i != len && path[i] != '/') {
            if (!name_char(path[i]))
                return false;
            continue;
        }
        size_t part = i - start;
        if (!part || (part == 1 && path[start] == '.') ||
            (part == 2 && !memcmp(path + start, "..", 2)) ||
            (part >= 5 && !memcmp(path + i - 5, ".data", 5)))
            return false;
        start = i + 1;
    }
    return true;
}

static bool mkdir_if_missing(const char *path) {
    const File *file = fs_find(path);
    return file ? file->dir : fs_create(path, true) == 0;
}

static bool create_parents(char *path, size_t parent_len) {
    for (char *p = path + parent_len; *p; ++p) {
        if (*p != '/')
            continue;
        *p = 0;
        bool ok = mkdir_if_missing(path);
        *p = '/';
        if (!ok)
            return false;
    }
    return true;
}

/* Header matching is length-aware because archive entries can contain NULs. */
static bool metadata_has(const uint8_t *data, size_t len, const char *header, bool exact) {
    size_t width = strlen(header);
    for (size_t start = 0; start < len;) {
        size_t end = start;
        while (end < len && data[end] != '\n')
            ++end;
        size_t line = end - start;
        if (line && data[end - 1] == '\r')
            --line;
        if (!line)
            break;
        if (line >= width && (!exact || line == width) && !memcmp(data + start, header, width))
            return true;
        start = end + 1;
    }
    return false;
}

typedef struct {
    const uint8_t *zip, *central;
    size_t offset, data_limit, expanded;
    unsigned remaining;
} Wheel;

typedef struct {
    char path[PATH_CAP];
    const uint8_t *data;
    unsigned method;
    size_t bytes, packed;
    uint32_t crc;
    bool directory;
} Entry;

static bool next_entry(Wheel *wheel, Entry *entry) {
    const uint8_t *p = wheel->zip + wheel->offset;
    if ((size_t)(wheel->central - p) < CENTRAL_HEADER || le32(p) != 0x02014b50)
        return false;
    unsigned flags = le16(p + 8), name_len = le16(p + 28);
    size_t record = CENTRAL_HEADER + name_len + le16(p + 30) + le16(p + 32);
    unsigned mode = le16(p + 40) & 0170000;
    entry->method = le16(p + 10);
    entry->crc = le32(p + 16);
    entry->packed = le32(p + 20);
    entry->bytes = le32(p + 24);
    if (record > (size_t)(wheel->central - p) || !name_len || name_len >= PATH_CAP ||
        flags & ~0x080eu || le16(p + 34) ||
        (entry->method != 0 && entry->method != 8) ||
        (!entry->method && entry->packed != entry->bytes) ||
        entry->bytes > WHEEL_ENTRY_BYTES || entry->bytes > PYTHON_WHEEL_BYTES - wheel->expanded)
        return false;

    const char *name = (const char *)p + CENTRAL_HEADER;
    entry->directory = name[name_len - 1] == '/';
    size_t len = name_len - entry->directory;
    if (!python_package_path(name, len) || (mode && mode != (entry->directory ? 0040000 : 0100000)) ||
        (entry->directory && entry->bytes))
        return false;
    memcpy(entry->path, name, len);
    entry->path[len] = 0;
    if (suffix(entry->path, ".so") || suffix(entry->path, ".pyd") ||
        suffix(entry->path, ".dll") || suffix(entry->path, ".pyc"))
        return false;

    size_t local_offset = le32(p + 42), limit = wheel->data_limit;
    if (local_offset > limit || limit - local_offset < LOCAL_HEADER)
        return false;
    const uint8_t *local = wheel->zip + local_offset;
    size_t payload = LOCAL_HEADER + le16(local + 26) + le16(local + 28);
    if (le32(local) != 0x04034b50 || le16(local + 6) != flags ||
        le16(local + 8) != entry->method || le16(local + 26) != name_len ||
        payload > limit - local_offset || entry->packed > limit - local_offset - payload ||
        memcmp(local + LOCAL_HEADER, name, name_len))
        return false;

    entry->data = local + payload;
    wheel->expanded += entry->bytes;
    wheel->offset += record;
    --wheel->remaining;
    return true;
}

static bool extract(const Entry *entry, uint8_t *data) {
    if (entry->method == 8) {
        if (stbi_zlib_decode_noheader_buffer((char *)data, entry->bytes,
                                            (const char *)entry->data, entry->packed)
            != (int)entry->bytes)
            return false;
    } else {
        memcpy(data, entry->data, entry->bytes);
    }
    return crc32(data, entry->bytes) == entry->crc;
}

static const char *install_entries(Wheel *wheel, const char *library, const char *info,
                                   uint8_t *data, char *paths, bool no_deps) {
    bool header = false, metadata = false, record = false;
    size_t lib_len = strlen(library), info_len = strlen(info), used = 0;
    if (!mkdir_if_missing(library))
        return "cannot create library directory";

    while (wheel->remaining) {
        Entry entry;
        if (!next_entry(wheel, &entry) || !extract(&entry, data))
            return "invalid or unsupported ZIP member";
        if (entry.directory)
            continue;
        if (!strncmp(entry.path, info, info_len) && entry.path[info_len] == '/') {
            const char *file = entry.path + info_len + 1;
            if (!strcmp(file, PYTHON_INSTALL_RECORD))
                return "wheel contains a reserved installer record";
            if (!strcmp(file, "WHEEL")) {
                header = metadata_has(data, entry.bytes, "Wheel-Version: 1.", false) &&
                         metadata_has(data, entry.bytes, "Root-Is-Purelib: true", true) &&
                         metadata_has(data, entry.bytes, "Tag: py3-none-any", true);
                if (!header)
                    return "wheel is not compatible with this installer";
            } else if (!strcmp(file, "METADATA")) {
                metadata = true;
                if (!no_deps && metadata_has(data, entry.bytes, "Requires-Dist:", false))
                    return "wheel declares dependencies; install them separately with --no-deps";
            } else if (!strcmp(file, "RECORD")) {
                record = true;
            }
        }

        char path[PATH_CAP];
        if (lib_len + 1 + strlen(entry.path) > 120)
            return "wheel member path exceeds the LunaFS limit";
        strcpy(path, library);
        strcat(path, "/");
        strcat(path, entry.path);
        if (fs_find(path))
            return "wheel would overwrite an existing file";
        if (!create_parents(path, lib_len + 1) || fs_write_bytes(path, data, entry.bytes, false))
            return "could not write wheel member to LunaFS";
        size_t len = strlen(entry.path);
        memcpy(paths + used, entry.path, len);
        used += len;
        paths[used++] = '\n';
    }

    if (wheel->zip + wheel->offset != wheel->central || !header || !metadata || !record)
        return "wheel metadata is incomplete";
    char manifest[PATH_CAP];
    if (lib_len + info_len + sizeof(PYTHON_INSTALL_RECORD) + 1 > 120)
        return "installation record path exceeds the LunaFS limit";
    strcpy(manifest, library);
    strcat(manifest, "/");
    strcat(manifest, info);
    strcat(manifest, "/" PYTHON_INSTALL_RECORD);
    if (fs_write_bytes(manifest, paths, used, false))
        return "could not write installation record";
    return NULL;
}

bool python_wheel_install(const char *name, const uint8_t *zip, size_t size,
                          const char *library, bool no_deps, char *out, size_t cap) {
    char info[PATH_CAP];
    const uint8_t *end = end_record(zip, size);
    if (!python_wheel_name(name, info, sizeof(info)) || !end) {
        append(out, "pip: expected a Python 3 pure wheel\n", cap);
        return false;
    }
    size_t offset = le32(end + 16), bytes = le32(end + 12), boundary = end - zip;
    unsigned entries = le16(end + 10);
    if (!entries || entries > WHEEL_ENTRIES || le16(end + 4) || le16(end + 6) ||
        le16(end + 8) != entries || offset > boundary || bytes != boundary - offset) {
        append(out, "pip: invalid or unsupported ZIP directory\n", cap);
        return false;
    }

    uint8_t *data = heap_allocate(2 * WHEEL_ENTRY_BYTES);
    FileState backup = {0};
    if (!data || fs_clone(&backup, files)) {
        heap_release(data);
        append(out, "pip: cannot stage package installation\n", cap);
        return false;
    }
    bool was_dirty = fs_dirty;
    Wheel wheel = {.zip = zip, .central = end, .offset = offset,
                   .data_limit = offset, .remaining = entries};
    const char *error = install_entries(&wheel, library, info, data,
                                        (char *)data + WHEEL_ENTRY_BYTES, no_deps);
    heap_release(data);
    if (error) {
        fs_restore_state(&backup);
        fs_dirty = was_dirty;
        append(out, "pip: ", cap);
        append(out, error, cap);
        append(out, "\n", cap);
        return false;
    }
    fs_discard_state(&backup);
    append(out, "Installed ", cap);
    append(out, name, cap);
    append(out, "\n", cap);
    return true;
}
