/* Native Git foundation for LunaFS2. Not an embedded Unix Git executable.
 * Scope: repository initialization, local config, worktree discovery,
 * untracked initial status, branch name and Git-compatible blob hashing. */
#include "luna.h"
#include "git.h"
#include "bearssl_hash.h"

static void say(char *out, size_t cap, const char *s) {
    append(out, s, cap);
}
static void decimal(char *out, size_t cap, uint32_t n) {
    char b[16];
    number(b, n);
    say(out, cap, b);
}
static bool join(char dest[PATH_CAP], const char *base, const char *tail) {
    size_t a = strlen(base), b = strlen(tail);
    if (a + b + 1u >= PATH_CAP)
        return false;
    strcpy(dest, base);
    strcat(dest, tail);
    return true;
}
static bool mkdir_once(const char *path) {
    File *f = fs_find(path);
    return f ? !!f->dir : fs_create(path, true) == 0;
}
static bool write_once(const char *path, const char *value) {
    File *f = fs_find(path);
    return f ? !f->dir : fs_write(path, value, false) == 0;
}
static bool gitdir(char path[PATH_CAP], const char *root, const char *entry) {
    char base[PATH_CAP];
    return join(base, root, !strcmp(root, "/") ? ".git" : "/.git") &&
           join(path, base, entry);
}
static bool read_text(const char *path, char *out, size_t cap) {
    File *file = fs_find(path);
    return file && !file->dir && file->size < cap && fs_read_text(file, out, cap);
}
static bool repository(char root[PATH_CAP]) {
    if (!fs_path(cwd, root))
        return false;
    for (;;) {
        char head[PATH_CAP];
        if (gitdir(head, root, "/HEAD") && fs_find(head))
            return true;
        if (!strcmp(root, "/"))
            break;
        size_t n = strlen(root);
        while (n > 1 && root[n - 1] != '/')
            n--;
        if (n > 1)
            n--;
        root[n] = 0;
    }
    return false;
}
static bool head_name(const char *root, char name[64]) {
    char path[PATH_CAP], contents[PATH_CAP];
    if (!gitdir(path, root, "/HEAD") || !read_text(path, contents, sizeof(contents)))
        return false;
    const char *prefix = "ref: refs/heads/";
    if (strncmp(contents, prefix, strlen(prefix)))
        return false;
    const char *s = contents + strlen(prefix);
    size_t n = 0;
    while (s[n] && s[n] != '\n' && s[n] != '\r' && n < 63)
        n++;
    if (!n || n >= 63)
        return false;
    memcpy(name, s, n);
    name[n] = 0;
    return true;
}
static bool valid_config_value(const char *s) {
    if (!*s || strlen(s) > 96)
        return false;
    for (; *s; s++)
        if ((unsigned char)*s < 32 || (unsigned char)*s == 127)
            return false;
    return true;
}
static bool config_get(const char *root, const char *name, char *dst, size_t cap) {
    char path[PATH_CAP], config[4096];
    if (!gitdir(path, root, "/config") || !read_text(path, config, sizeof(config)))
        return false;
    bool user = false, found = false;
    const char *p = config;
    while (*p) {
        char line[256];
        size_t k = 0;
        while (*p && *p != '\n') {
            if (k + 1 < sizeof(line))
                line[k++] = *p;
            p++;
        }
        if (*p == '\n')
            p++;
        line[k] = 0;
        char *t = line;
        while (*t == ' ' || *t == '\t')
            t++;
        if (*t == '[') {
            user = !strncmp(t, "[user]", 6);
            continue;
        }
        if (!user)
            continue;
        size_t n = strlen(name);
        if (strncmp(t, name, n))
            continue;
        char *eq = t + n;
        while (*eq == ' ' || *eq == '\t')
            eq++;
        if (*eq != '=')
            continue;
        eq++;
        while (*eq == ' ' || *eq == '\t')
            eq++;
        size_t len = strlen(eq);
        while (len && (eq[len - 1] == ' ' || eq[len - 1] == '\r' || eq[len - 1] == '\t'))
            len--;
        if (!len || len >= cap)
            continue;
        memcpy(dst, eq, len);
        dst[len] = 0;
        found = true;
    }
    return found;
}
static int do_config(int argc, char **argv, const char *root, char *out, size_t cap) {
    const char *key = 0, *val = 0;
    bool get = false;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--local"))
            continue;
        if (!strcmp(argv[i], "--get"))
            get = true;
        else if (!key)
            key = argv[i];
        else if (!val)
            val = argv[i];
        else
            return 2;
    }
    if (!key || (strcmp(key, "user.name") && strcmp(key, "user.email")) || (get && val)) {
        say(out, cap, "usage: git config [--local] [--get] user.name|user.email [VALUE]\n");
        return 2;
    }
    const char *field = key + 5;
    if (!val) {
        char found[128];
        if (!config_get(root, field, found, sizeof(found)))
            return 1;
        say(out, cap, found);
        say(out, cap, "\n");
        return 0;
    }
    if (!valid_config_value(val)) {
        say(out, cap, "git config: invalid value (up to 96 printable characters)\n");
        return 2;
    }
    char path[PATH_CAP], entry[256] = "[user]\n\t";
    if (!gitdir(path, root, "/config"))
        return 1;
    strcat(entry, field);
    strcat(entry, " = ");
    strcat(entry, val);
    strcat(entry, "\n");
    if (fs_write(path, entry, true)) {
        say(out, cap, "git config: cannot write local config\n");
        return 1;
    }
    return 0;
}
static int do_init(int argc, char **argv, char *out, size_t cap) {
    if (argc > 3 || (argc == 3 && argv[2][0] == '-')) {
        say(out, cap, "usage: git init [DIRECTORY]\n");
        return 2;
    }
    char root[PATH_CAP];
    if (!fs_path(argc == 3 ? argv[2] : cwd, root) ||
        !strcmp(root, "/")) {
        say(out, cap, "git init: choose a project directory other than /\n");
        return 1;
    }
    if (!mkdir_once(root)) {
        say(out, cap, "git init: directory does not exist or cannot be created\n");
        return 1;
    }
    char path[PATH_CAP];
    const char *dirs[] = { "", "/objects", "/objects/info", "/objects/pack",
                           "/refs", "/refs/heads", "/refs/tags", 0 };
    for (int i = 0; dirs[i]; i++)
        if (!gitdir(path, root, dirs[i]) || !mkdir_once(path)) {
            say(out, cap, "git init: cannot create repository metadata\n");
            return 1;
        }
    if (!gitdir(path, root, "/HEAD") ||
        !write_once(path, "ref: refs/heads/main\n") ||
        !gitdir(path, root, "/config") ||
        !write_once(path, "[core]\n\trepositoryformatversion = 0\n\tfilemode = false\n\tbare = false\n\tlogallrefupdates = true\n") ||
        !gitdir(path, root, "/description") ||
        !write_once(path, "Unnamed repository; edit this file 'description' to name the repository.\n")) {
        say(out, cap, "git init: could not write metadata\n");
        return 1;
    }
    say(out, cap, "Initialized Git repository in ");
    say(out, cap, root);
    say(out, cap, !strcmp(root, "/") ? ".git/\n" : "/.git/\n");
    say(out, cap, "Ready for local config; add/commit/network operations are not yet available.\n");
    return 0;
}
static bool in_project(const char *root, const char *path, const char **relative) {
    size_t length = strlen(root);
    if (!strcmp(root, "/")) {
        if (*path != '/')
            return false;
        *relative = path + 1;
        return true;
    }
    if (strncmp(path, root, length) || path[length] != '/')
        return false;
    *relative = path + length + 1;
    return true;
}
static int do_status(int argc, char **argv, const char *root, char *out, size_t cap) {
    bool short_form = false;
    if (argc == 3 && (!strcmp(argv[2], "-s") || !strcmp(argv[2], "--short") ||
                      !strcmp(argv[2], "--porcelain")))
        short_form = true;
    else if (argc != 2) {
        say(out, cap, "usage: git status [--short]\n");
        return 2;
    }
    char name[64], path[PATH_CAP];
    if (!head_name(root, name)) {
        say(out, cap, "git status: detached/invalid HEAD not supported\n");
        return 2;
    }
    if (!gitdir(path, root, "/index"))
        return 1;
    if (fs_find(path)) {
        say(out, cap, "git status: index detected; tracked-file comparison is not implemented yet\n");
        return 2;
    }
    char ref[PATH_CAP], suffix[90] = "/refs/heads/";
    strcat(suffix, name);
    if (!gitdir(ref, root, suffix))
        return 1;
    if (fs_find(ref)) {
        say(out, cap, "git status: committed history detected; tree comparison is not implemented yet\n");
        return 2;
    }
    if (!short_form) {
        say(out, cap, "On branch ");
        say(out, cap, name);
        say(out, cap, "\n\nNo commits yet\n");
    }
    unsigned count = 0;
    for (int i = 0; i < fs_file_capacity; i++) {
        File *file = &files[i];
        if (!file->used || file->dir)
            continue;
        const char *relative = 0;
        if (!in_project(root, file->path, &relative) ||
            !*relative || !strncmp(relative, ".git/", 5) || !strcmp(relative, ".git"))
            continue;
        if (!count && !short_form)
            say(out, cap, "\nUntracked files:\n");
        if (count < 30) {
            say(out, cap, short_form ? "?? " : "  ");
            say(out, cap, relative);
            say(out, cap, "\n");
        }
        count++;
    }
    if (count > 30 && !short_form) {
        say(out, cap, "  ... and ");
        decimal(out, cap, count - 30);
        say(out, cap, " more\n");
    }
    if (!short_form && !count)
        say(out, cap, "\nNo files to track yet.\n");
    if (!short_form && count)
        say(out, cap, "\nStaging and committing will be enabled in the next Git phase.\n");
    return 0;
}
static int do_hash_object(int argc, char **argv, char *out, size_t cap) {
    if (argc != 3 || argv[2][0] == '-') {
        say(out, cap, "usage: git hash-object FILE\n");
        return 2;
    }
    File *file = fs_find(argv[2]);
    if (!file || file->dir) {
        say(out, cap, "git hash-object: file not found\n");
        return 1;
    }
    br_sha1_context ctx;
    br_sha1_init(&ctx);
    char preamble[40] = "blob ";
    number(preamble + 5, file->size);
    size_t header_len = strlen(preamble) + 1;
    br_sha1_update(&ctx, preamble, header_len);
    uint8_t block[1024];
    uint32_t offset = 0;
    while (offset < file->size) {
        size_t n = file->size - offset;
        if (n > sizeof(block))
            n = sizeof(block);
        if (!fs_read_bytes(file, offset, block, n)) {
            say(out, cap, "git hash-object: cannot read file\n");
            return 1;
        }
        br_sha1_update(&ctx, block, n);
        offset += (uint32_t)n;
    }
    uint8_t digest[20];
    br_sha1_out(&ctx, digest);
    static const char digits[] = "0123456789abcdef";
    char hexed[42];
    for (int i = 0; i < 20; i++) {
        hexed[2 * i] = digits[digest[i] >> 4];
        hexed[2 * i + 1] = digits[digest[i] & 15];
    }
    hexed[40] = '\n';
    hexed[41] = 0;
    say(out, cap, hexed);
    return 0;
}
int luna_git_command(int argc, char **argv, char *out, size_t cap) {
    if (!out || cap < 2)
        return 2;
    out[0] = 0;
    if (argc < 2 || !strcmp(argv[1], "help") || !strcmp(argv[1], "--help")) {
        say(out, cap, "Git foundation for lunaSH (not the full Git CLI)\n");
        say(out, cap, "  git init [DIR]            Initialize standard .git metadata\n");
        say(out, cap, "  git status [--short]      Initial untracked worktree (unborn branch)\n");
        say(out, cap, "  git config user.name VAL  Configure local author name\n");
        say(out, cap, "  git config user.email VAL Configure local author email\n");
        say(out, cap, "  git config --get KEY      Read local user.name/user.email\n");
        say(out, cap, "  git branch --show-current Show current branch\n");
        say(out, cap, "  git rev-parse --show-toplevel|--git-dir|--is-inside-work-tree\n");
        say(out, cap, "  git hash-object FILE     Compute Git-compatible blob SHA-1\n");
        say(out, cap, "Not yet supported: add, commit, log, clone, fetch, pull, push, checkout.\n");
        return argc < 2 ? 2 : 0;
    }
    if (!strcmp(argv[1], "--version") || !strcmp(argv[1], "version")) {
        say(out, cap, "LunaOS native Git foundation 0.1 (subset; not Git upstream)\n");
        return 0;
    }
    if (!strcmp(argv[1], "init"))
        return do_init(argc, argv, out, cap);
    if (!strcmp(argv[1], "hash-object"))
        return do_hash_object(argc, argv, out, cap);
    char root[PATH_CAP];
    if (!repository(root)) {
        say(out, cap, "fatal: not a git repository (or any parent directory)\n");
        return 128;
    }
    if (!strcmp(argv[1], "status"))
        return do_status(argc, argv, root, out, cap);
    if (!strcmp(argv[1], "config"))
        return do_config(argc, argv, root, out, cap);
    if (!strcmp(argv[1], "branch")) {
        if (argc != 3 || strcmp(argv[2], "--show-current")) {
            say(out, cap, "git branch: only --show-current is supported\n");
            return 2;
        }
        char b[64];
        if (!head_name(root, b))
            return 1;
        say(out, cap, b);
        say(out, cap, "\n");
        return 0;
    }
    if (!strcmp(argv[1], "rev-parse")) {
        if (argc != 3) {
            say(out, cap, "usage: git rev-parse --show-toplevel|--git-dir|--is-inside-work-tree\n");
            return 2;
        }
        if (!strcmp(argv[2], "--show-toplevel"))
            say(out, cap, root);
        else if (!strcmp(argv[2], "--git-dir")) {
            if (!gitdir(root, root, ""))
                return 1;
            say(out, cap, root);
        } else if (!strcmp(argv[2], "--is-inside-work-tree"))
            say(out, cap, "true");
        else
            return 2;
        say(out, cap, "\n");
        return 0;
    }
    say(out, cap, "git: subcommand not implemented in native LunaOS Git foundation: ");
    say(out, cap, argv[1]);
    say(out, cap, "\n");
    return 2;
}
