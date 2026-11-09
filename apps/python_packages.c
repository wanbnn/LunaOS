#include "python_wheel.h"
#include "heap.h"
#include <string.h>

#define INFO_SUFFIX ".dist-info"

static bool child_path(char path[PATH_CAP], const char *parent, const char *child) {
    if (strlen(parent) + 1 + strlen(child) > 120)
        return false;
    strcpy(path, parent);
    strcat(path, "/");
    strcat(path, child);
    return true;
}

static bool package_info(const char *library, const File *dir, char *name, char *version) {
    size_t root = strlen(library);
    if (!dir->used || !dir->dir || strncmp(dir->path, library, root) || dir->path[root] != '/')
        return false;
    const char *leaf = dir->path + root + 1, *sep = strchr(leaf, '-');
    size_t len = strlen(leaf);
    if (!sep || strchr(leaf, '/') || len <= sizeof(INFO_SUFFIX) ||
        strcmp(leaf + len - (sizeof(INFO_SUFFIX) - 1), INFO_SUFFIX))
        return false;
    size_t version_len = leaf + len - (sizeof(INFO_SUFFIX) - 1) - (sep + 1);
    if (!version_len || version_len >= PATH_CAP ||
        !python_package_normalize(leaf, sep - leaf, name, PATH_CAP))
        return false;
    char manifest[PATH_CAP];
    if (!child_path(manifest, dir->path, PYTHON_INSTALL_RECORD))
        return false;
    const File *file = fs_find(manifest);
    if (!file || file->dir)
        return false;
    memcpy(version, sep + 1, version_len);
    version[version_len] = 0;
    return true;
}

bool python_package_find(const char *library, const char *project, char *dir, char *version) {
    char name[PATH_CAP];
    for (int i = 0; i < fs_file_capacity; ++i)
        if (package_info(library, &files[i], name, version) && !strcmp(name, project)) {
            strcpy(dir, files[i].path);
            return true;
        }
    return false;
}

void python_package_list(const char *library, bool freeze, char *out, size_t cap) {
    char name[PATH_CAP], version[PATH_CAP];
    bool found = false;
    for (int i = 0; i < fs_file_capacity; ++i)
        if (package_info(library, &files[i], name, version)) {
            found = true;
            append(out, name, cap);
            append(out, freeze ? "==" : " ", cap);
            append(out, version, cap);
            append(out, "\n", cap);
        }
    if (!found && !freeze)
        append(out, "(no packages installed)\n", cap);
}

static void remove_empty_parents(char *path, size_t root_len) {
    char *slash = strrchr(path, '/');
    while (slash && (size_t)(slash - path) > root_len) {
        *slash = 0;
        if (fs_remove(path))
            break;
        slash = strrchr(path, '/');
    }
}

static bool remove_members(const char *library, char *record) {
    for (char *line = record; *line;) {
        char *end = strchr(line, '\n');
        if (!end)
            return false;
        *end = 0;
        char path[PATH_CAP];
        if (!python_package_path(line, end - line) || !child_path(path, library, line))
            return false;
        const File *file = fs_find(path);
        if (file && (file->dir || fs_remove(path)))
            return false;
        remove_empty_parents(path, strlen(library));
        line = end + 1;
    }
    return true;
}

bool python_package_remove(const char *library, const char *dir, char *out, size_t cap) {
    char path[PATH_CAP];
    if (!child_path(path, dir, PYTHON_INSTALL_RECORD))
        return false;
    File *file = fs_find(path);
    if (!file || file->dir || !file->size || file->size > 64u * 1024) {
        append(out, "pip: installation record is missing or invalid\n", cap);
        return false;
    }

    size_t bytes = file->size;
    char *record = heap_allocate(bytes + 1);
    FileState backup = {0};
    if (!record || !fs_read_text(file, record, bytes + 1) || strlen(record) != bytes ||
        fs_clone(&backup, files)) {
        heap_release(record);
        append(out, "pip: cannot read or stage the installation record\n", cap);
        return false;
    }

    bool was_dirty = fs_dirty;
    bool ok = remove_members(library, record) && !fs_remove(path) && !fs_remove(dir);
    heap_release(record);
    if (!ok) {
        fs_restore_state(&backup);
        fs_dirty = was_dirty;
        append(out, "pip: cannot remove all package files\n", cap);
        return false;
    }
    fs_discard_state(&backup);
    return true;
}
