#include "python.h"
#include "python_wheel.h"
#include "heap.h"
#include <string.h>
#include "install.h"
#include "update.h"

#define CACHE_DIR "/.pip-cache"
#define REQUIREMENTS_BYTES (16u * 1024)

typedef struct {
    const char *library, *index;
    bool no_deps, no_cache;
    char *out;
    size_t cap;
} Pip;

static bool wheel_suffix(const char *name) {
    size_t n = strlen(name);
    return n > 4 && !strcmp(name + n - 4, ".whl");
}

static bool is_url(const char *spec) {
    return !strncmp(spec, "https://", 8) || !strncmp(spec, "http://", 7);
}

static const char *basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void cache_dir(char path[PATH_CAP]) {
    strcpy(path, account_home());
    strcat(path, CACHE_DIR);
}

static bool cache_path(const char *name, char path[PATH_CAP]) {
    cache_dir(path);
    if (strlen(path) + 1 + strlen(name) > 120)
        return false;
    strcat(path, "/");
    strcat(path, name);
    return true;
}

static char *read_file(const char *path, size_t limit, size_t *size) {
    File *f = fs_find(path);
    if (!f || f->dir || f->size > limit)
        return NULL;
    *size = f->size;
    char *data = heap_allocate(*size + 1);
    if (data && !fs_read_text(f, data, *size + 1)) {
        heap_release(data);
        return NULL;
    }
    return data;
}

static char *cached_wheel(const char *name, const uint8_t *digest, size_t *size) {
    char path[PATH_CAP];
    if (!cache_path(name, path))
        return NULL;
    char *data = read_file(path, PYTHON_WHEEL_BYTES, size);
    if (data && !update_digest_valid(data, *size, digest)) {
        heap_release(data);
        return NULL;
    }
    return data;
}

static void cache_wheel(const char *name, const char *data, size_t size) {
    char dir[PATH_CAP], path[PATH_CAP];
    cache_dir(dir);
    if (cache_path(name, path) && (fs_find(dir) || !fs_create(dir, true)))
        fs_write_bytes(path, data, size, false);
}

/* Packages in the index are pinned to the downloaded file's digest. */
static int install(Pip *pip, const char *spec, const char *source_dir) {
    char url[512], path[PATH_CAP], info[PATH_CAP];
    uint8_t digest[PYTHON_DIGEST_BYTES];
    bool remote = is_url(spec), indexed = !remote && !wheel_suffix(spec);
    const char *location = spec;
    if (indexed) {
        if (!python_index_find(spec, pip->index, url, sizeof(url), digest, pip->out, pip->cap))
            return 1;
        location = url;
        remote = true;
    }
    const char *name = basename(location);
    if (!python_wheel_name(name, info, sizeof(info))) {
        append(pip->out, "pip: expected a py3-none-any or py2.py3-none-any wheel\n", pip->cap);
        return 1;
    }

    char project[PATH_CAP], installed[PATH_CAP], version[PATH_CAP];
    const char *dash = strchr(info, '-');
    if (!python_package_normalize(info, dash - info, project, sizeof(project))) {
        append(pip->out, "pip: wheel has an invalid distribution name\n", pip->cap);
        return 1;
    }
    if (python_package_find(pip->library, project, installed, version)) {
        if (!strcmp(basename(installed), info)) {
            append(pip->out, "Already installed: ", pip->cap);
            append(pip->out, name, pip->cap);
            append(pip->out, "\n", pip->cap);
            return 0;
        }
        append(pip->out, "pip: uninstall the existing version before installing another\n", pip->cap);
        return 1;
    }

    size_t size = 0;
    char *data = NULL;
    bool cache = indexed && !pip->no_cache;
    bool hit = cache && (data = cached_wheel(name, digest, &size));
    if (hit) {
        append(pip->out, "Using cached wheel\n", pip->cap);
    } else if (remote) {
        if (python_fetch(location, &data, &size, pip->out, pip->cap))
            return 1;
    } else {
        if (source_dir && location[0] != '/') {
            if (strlen(source_dir) + 1 + strlen(location) > 120) {
                append(pip->out, "pip: wheel path is too long\n", pip->cap);
                return 1;
            }
            strcpy(path, source_dir);
            strcat(path, "/");
            strcat(path, location);
            location = path;
        }
        data = read_file(location, PYTHON_WHEEL_BYTES, &size);
    }
    if (!data) {
        append(pip->out, "pip: cannot read wheel\n", pip->cap);
        return 1;
    }
    if (indexed && !hit && !update_digest_valid(data, size, digest)) {
        heap_release(data);
        append(pip->out, "pip: wheel SHA-256 mismatch\n", pip->cap);
        return 1;
    }
    bool ok = python_wheel_install(name, (const uint8_t *)data, size, pip->library,
                                   pip->no_deps, pip->out, pip->cap);
    if (ok && cache && !hit)
        cache_wheel(name, data, size);
    heap_release(data);
    return ok ? 0 : 1;
}

static bool blank(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

static int requirements(Pip *pip, const char *path) {
    char dir[PATH_CAP];
    size_t size;
    char *text = read_file(path, REQUIREMENTS_BYTES, &size);
    if (!text || strlen(text) != size || !fs_path(path, dir)) {
        heap_release(text);
        append(pip->out, "pip: cannot read requirements file\n", pip->cap);
        return 1;
    }
    char *slash = strrchr(dir, '/');
    *slash = 0; /* Root is represented by an empty prefix when joining paths. */

    int result = 0;
    unsigned line_no = 0;
    for (char *line = text; *line;) {
        ++line_no;
        char *end = strchr(line, '\n');
        char *next = end ? end + 1 : line + strlen(line);
        if (end)
            *end = 0;
        while (blank(*line))
            ++line;
        char *comment = strchr(line, '#');
        if (comment && (comment == line || blank(comment[-1])))
            *comment = 0;
        end = line + strlen(line);
        while (end > line && blank(end[-1]))
            *--end = 0;
        if (*line) {
            result = install(pip, line, dir);
            if (result) {
                char count[16];
                number(count, line_no);
                append(pip->out, "pip: stopped at requirements line ", pip->cap);
                append(pip->out, count, pip->cap);
                append(pip->out, "\n", pip->cap);
                break;
            }
        }
        line = next;
    }
    heap_release(text);
    return result;
}

static int install_command(Pip *pip, int argc, char **argv) {
    /* Read the options first so they apply to every entry, including -r. */
    bool files[64] = {0}, items[64] = {0};
    unsigned count = 0;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--no-deps")) {
            pip->no_deps = true;
        } else if (!strcmp(argv[i], "--no-cache-dir")) {
            pip->no_cache = true;
        } else if (!strcmp(argv[i], "--index-url") && i + 1 < argc && is_url(argv[i + 1])) {
            pip->index = argv[++i];
        } else if ((!strcmp(argv[i], "-r") || !strcmp(argv[i], "--requirement")) && i + 1 < argc) {
            files[++i] = true;
            ++count;
        } else if (argv[i][0] != '-') {
            items[i] = true;
            ++count;
        } else {
            append(pip->out, "pip: unknown or incomplete install option\n", pip->cap);
            return 2;
        }
    }
    if (!count) {
        append(pip->out, "pip: install requires a package, wheel or -r FILE\n", pip->cap);
        return 2;
    }
    for (int i = 2; i < argc; ++i) {
        int rc = files[i] ? requirements(pip, argv[i]) : items[i] ? install(pip, argv[i], NULL) : 0;
        if (rc)
            return rc;
    }
    return 0;
}

static int package_command(Pip *pip, const char *name, bool uninstall) {
    char project[PATH_CAP], dir[PATH_CAP], version[PATH_CAP];
    if (!python_package_normalize(name, strlen(name), project, sizeof(project))) {
        append(pip->out, "pip: invalid package name\n", pip->cap);
        return 2;
    }
    if (!python_package_find(pip->library, project, dir, version)) {
        append(pip->out, "pip: package is not installed in this environment\n", pip->cap);
        return 1;
    }
    if (uninstall) {
        if (!python_package_remove(pip->library, dir, pip->out, pip->cap))
            return 1;
        append(pip->out, "Uninstalled ", pip->cap);
        append(pip->out, project, pip->cap);
        append(pip->out, "\n", pip->cap);
    } else {
        append(pip->out, "Name: ", pip->cap);
        append(pip->out, project, pip->cap);
        append(pip->out, "\nVersion: ", pip->cap);
        append(pip->out, version, pip->cap);
        append(pip->out, "\nLocation: ", pip->cap);
        append(pip->out, pip->library, pip->cap);
        append(pip->out, "\n", pip->cap);
    }
    return 0;
}

static int cache_command(Pip *pip, int argc, char **argv) {
    char dir[PATH_CAP];
    cache_dir(dir);
    if (argc == 3 && !strcmp(argv[2], "dir")) {
        append(pip->out, dir, pip->cap);
        append(pip->out, "\n", pip->cap);
        return 0;
    }
    bool list = argc == 3 && !strcmp(argv[2], "list");
    bool info = argc == 3 && !strcmp(argv[2], "info");
    bool purge = argc == 3 && !strcmp(argv[2], "purge");
    bool remove = argc == 4 && !strcmp(argv[2], "remove");
    char filter[PATH_CAP];
    if (remove && !python_package_normalize(argv[3], strlen(argv[3]), filter, sizeof(filter)))
        return 2;
    if (!list && !info && !purge && !remove) {
        append(pip->out, "usage: pip cache dir | info | list | purge | remove NAME\n", pip->cap);
        return 2;
    }

    unsigned count = 0;
    size_t root = strlen(dir);
    for (int i = 0; i < fs_file_capacity; ++i) {
        File *file = &files[i];
        if (!file->used || file->dir || strncmp(file->path, dir, root) || file->path[root] != '/')
            continue;
        const char *name = file->path + root + 1;
        char dist[PATH_CAP], project[PATH_CAP];
        if (strchr(name, '/') || !python_wheel_name(name, dist, sizeof(dist)) ||
            !python_package_normalize(dist, strchr(dist, '-') - dist, project, sizeof(project)) ||
            (remove && strcmp(filter, project)))
            continue;
        if (purge || remove) {
            char path[PATH_CAP];
            strcpy(path, file->path);
            if (fs_remove(path)) {
                append(pip->out, "pip: cache entry could not be removed\n", pip->cap);
                return 1;
            }
        } else if (list) {
            append(pip->out, name, pip->cap);
            append(pip->out, "\n", pip->cap);
        }
        ++count;
    }
    char n[16];
    number(n, count);
    append(pip->out, purge || remove ? "Files removed: " : "Cached wheels: ", pip->cap);
    append(pip->out, n, pip->cap);
    append(pip->out, "\n", pip->cap);
    return 0;
}

int python_pip_command(int argc, char **argv, char *out, size_t cap) {
    if (argc == 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))) {
        append(out, "LunaOS pip 1 (MicroPython wheels)\n", cap);
        return 0;
    }
    char lib[PATH_CAP];
    Pip pip = {.library = lib, .out = out, .cap = cap};
    if (argc >= 2 && !strcmp(argv[1], "cache"))
        return cache_command(&pip, argc, argv);
    if (!python_library_path(lib)) {
        append(out, "pip: active environment is missing. Use venv deactivate.\n", cap);
        return 1;
    }
    if (argc >= 2 && !strcmp(argv[1], "install"))
        return install_command(&pip, argc, argv);
    if (argc == 2 && (!strcmp(argv[1], "list") || !strcmp(argv[1], "freeze"))) {
        python_package_list(lib, !strcmp(argv[1], "freeze"), out, cap);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "show"))
        return package_command(&pip, argv[2], false);
    if (argc == 3 && !strcmp(argv[1], "uninstall"))
        return package_command(&pip, argv[2], true);
    if (argc == 4 && !strcmp(argv[1], "uninstall") && !strcmp(argv[2], "-y"))
        return package_command(&pip, argv[3], true);
    append(out, "usage: pip install NAME[==VERSION] | WHEEL | -r FILE\n", cap);
    append(out, "       pip uninstall [-y] NAME | list | freeze | show NAME | cache | --version\n", cap);
    return argc == 1 || (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "help"))) ? 0 : 2;
}
