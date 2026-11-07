/* lunaSH developer tools: author and run apps and virtual drivers in LunaFS. */
#include "devkit.h"
#include "install.h"
#include "luna.h"
#include "web.h"

static void put(char *out, size_t cap, const char *s) {
    append(out, s, cap);
}
static bool in_home(const char *path) {
    char full[PATH_CAP];
    if (!fs_path(path, full)) return false;
    const char *home = account_home();
    size_t n = strlen(home);
    return !strncmp(full, home, n) && (full[n] == '/' || full[n] == 0);
}
static bool extension(const char *path, const char *suffix) {
    size_t a = strlen(path), b = strlen(suffix);
    return a > b && !strcmp(path + a - b, suffix);
}
static bool home_filename(const char *name, const char *extension, char out[PATH_CAP]) {
    if (!ldev_valid_name(name)) return false;
    size_t n = strlen(account_home()) + strlen(name) + strlen(extension) + 2;
    if (n >= PATH_CAP) return false;
    strcpy(out, account_home());
    strcat(out, "/");
    strcat(out, name);
    strcat(out, extension);
    return true;
}
static int driver_command(int argc, char **argv, char *out, size_t cap) {
    if (argc == 3 && !strcmp(argv[2], "list")) {
        char devices[512];
        ldev_list(devices, sizeof(devices));
        put(out, cap, *devices ? devices : "(no virtual devices loaded)\n");
        return 0;
    }
    if ((argc == 4 || argc == 5) && !strcmp(argv[2], "new")) {
        const char *kind = argc == 5 ? argv[4] : "register";
        if (strcmp(kind, "register") && strcmp(kind, "upper") && strcmp(kind, "counter")) {
            put(out, cap, "driver: type must be register, upper or counter\n"); return 2;
        }
        char path[PATH_CAP];
        if (!home_filename(argv[3], ".ldrv", path) || fs_find(path)) {
            put(out, cap, "driver: invalid name or file already exists\n"); return 1;
        }
        char source[256] = "LDEV1\nname=";
        append(source, argv[3], sizeof(source));
        append(source, "\ntype=", sizeof(source));
        append(source, kind, sizeof(source));
        append(source, "\nvalue=", sizeof(source));
        append(source, !strcmp(kind, "counter") ? "0\n" : "hello\n", sizeof(source));
        if (fs_write(path, source, false)) {
            put(out, cap, "driver: cannot create file\n"); return 1;
        }
        put(out, cap, "Created virtual device descriptor: ");
        put(out, cap, path); put(out, cap, "\nEdit it using Notes, then load with: dev driver load ");
        put(out, cap, path); put(out, cap, "\n");
        return 0;
    }
    if (argc == 4 && !strcmp(argv[2], "load")) {
        if (!in_home(argv[3]) || !extension(argv[3], ".ldrv")) {
            put(out, cap, "driver: only .ldrv files in your home are supported\n"); return 2;
        }
        File *file = fs_find(argv[3]);
        if (!file || file->dir) { put(out, cap, "driver: file not found\n"); return 1; }
        if (ldev_load(file->data)) { put(out, cap, "driver: invalid or duplicate descriptor / registry full\n"); return 1; }
        put(out, cap, "Virtual device loaded (RAM only; no PCI/MMIO/IRQ).\n");
        return 0;
    }
    if (argc == 4 && !strcmp(argv[2], "unload")) {
        if (ldev_unload(argv[3])) { put(out, cap, "driver: device not found\n"); return 1; }
        put(out, cap, "Virtual device unloaded.\n"); return 0;
    }
    if (argc == 4 && !strcmp(argv[2], "read")) {
        char value[128];
        if (ldev_read(argv[3], value, sizeof(value))) {
            put(out, cap, "driver: device not found or read failed\n"); return 1;
        }
        put(out, cap, value); put(out, cap, "\n");
        return 0;
    }
    if (argc == 5 && !strcmp(argv[2], "write")) {
        if (ldev_write(argv[3], argv[4])) {
            put(out, cap, "driver: unknown device or invalid value\n"); return 1;
        }
        put(out, cap, "Device updated.\n"); return 0;
    }
    put(out, cap, "usage: dev driver new NAME [register|upper|counter]\n");
    put(out, cap, "       dev driver load FILE.ldrv | list | read NAME | write NAME VALUE | unload NAME\n");
    put(out, cap, "Native hardware drivers must be compiled into the kernel; see docs/developer-api.md\n");
    return 2;
}
int developer_command(int argc, char **argv, char *out, size_t cap) {
    if (argc >= 2 && !strcmp(argv[1], "driver"))
        return driver_command(argc, argv, out, cap);
    if (argc == 3 && !strcmp(argv[1], "new")) {
        char path[PATH_CAP];
        if (!home_filename(argv[2], ".ljs", path) || fs_find(path)) {
            put(out, cap, "dev: invalid name or file already exists\n"); return 1;
        }
        const char *code =
            "// LunaSDK v1 - edit with Notes, then: dev run FILE.ljs\n"
            "console.log('Hello from LunaOS!', luna.version);\n"
            "console.log('Uptime:', luna.uptime(), 'ms');\n"
            "luna.fs.write('hello.txt', 'Made inside LunaOS');\n"
            "console.log(luna.fs.read('hello.txt'));\n";
        if (fs_write(path, code, false)) {
            put(out, cap, "dev: could not write app\n"); return 1;
        }
        put(out, cap, "Created JavaScript app: "); put(out, cap, path); put(out, cap, "\n");
        put(out, cap, "Open it with Notes, then run: dev run ");
        put(out, cap, path); put(out, cap, "\n");
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "run")) {
        if (!in_home(argv[2]) || !extension(argv[2], ".ljs")) {
            put(out, cap, "dev: only .ljs programs under your home directory may run\n"); return 2;
        }
        File *f = fs_find(argv[2]);
        if (!f || f->dir) { put(out, cap, "dev: program not found\n"); return 1; }
        /* Copy before execution: the script is allowed to overwrite itself. */
        char script[FILE_CAP];
        memcpy(script, f->data, f->size + 1);
        return javascript_eval_sdk(script, f->path, out, cap) ? 1 : 0;
    }
    if (argc == 2 && !strcmp(argv[1], "list")) {
        const char *home = account_home();
        size_t n = strlen(home);
        for (int i = 0; i < MAX_FILES; i++)
            if (files[i].used && !files[i].dir && !strncmp(files[i].path, home, n) &&
                files[i].path[n] == '/' && !strchr(files[i].path + n + 1, '/') &&
                (extension(files[i].path, ".ljs") || extension(files[i].path, ".ldrv"))) {
                put(out, cap, files[i].path); put(out, cap, "\n");
            }
        return 0;
    }
    put(out, cap, "LunaSDK v1 / developer tools\n");
    put(out, cap, "dev new NAME          create a .ljs app in your home\n");
    put(out, cap, "dev run FILE.ljs      run an app with the luna API\n");
    put(out, cap, "dev list              show local .ljs / .ldrv files\n");
    put(out, cap, "dev driver            manage bounded software/virtual devices\n");
    put(out, cap, "Hardware drivers: native C only, built into the kernel.\n");
    return argc == 1 ? 0 : 2;
}
