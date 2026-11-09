/* lunaSH developer tools: author and run apps and virtual drivers in LunaFS. */
#include "devkit.h"
#include "install.h"
#include "luna.h"
#include "web.h"
#include "lunasdk_hardware.h"
#include "heap.h"
#include "python.h"

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
/* Parse numeric PCI arguments as decimal or 0x-prefixed hexadecimal. */
static bool numeric(const char *text, unsigned *number) {
    if (!text || !*text || !number) return false;
    unsigned base = 10, value = 0, digits = 0;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text += 2; base = 16;
    }
    for (; *text; text++) {
        char c = *text;
        int digit = c >= '0' && c <= '9' ? c - '0' :
                    c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                    c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0 || (unsigned)digit >= base ||
            value > (UINT32_MAX - (unsigned)digit) / base) return false;
        digits++; value = value * base + (unsigned)digit;
    }
    if (!digits) return false;
    *number = value;
    return true;
}
static void show_num(char *out, size_t cap, unsigned value) {
    char n[16];
    number(n, value);
    put(out, cap, n);
}
static int hardware_command(int argc, char **argv, char *out, size_t cap) {
    if (argc == 3 && !strcmp(argv[2], "pci")) {
        unsigned total = 0;
        if (lunasdk_pci_count(&total)) {
            put(out, cap, "hardware: PCI bus scan failed\n"); return 1;
        }
        put(out, cap, "PCI devices found: ");
        show_num(out, cap, total); put(out, cap, "\n");
        /* Bound terminal output: query individual entries from JS as needed. */
        for (unsigned i = 0; i < total && i < 48; i++) {
            LunaPciInfo d;
            if (lunasdk_pci_get(i, &d)) break;
            show_num(out, cap, d.bus); put(out, cap, ":");
            show_num(out, cap, d.slot); put(out, cap, ".");
            show_num(out, cap, d.function);
            put(out, cap, " vendor:device=");
            char h[16]; hex(h, (unsigned)d.vendor << 16 | d.device);
            put(out, cap, h); put(out, cap, " class=");
            hex(h, d.class_code); put(out, cap, h); put(out, cap, "\n");
        }
        if (total > 48) put(out, cap, "(remaining devices: use luna.hardware.pci.get(index) in JS)\n");
        return 0;
    }
    if (argc == 8 && !strcmp(argv[2], "read")) {
        unsigned v[5];
        for (unsigned i = 0; i < 5; i++)
            if (!numeric(argv[i + 3], &v[i])) {
                put(out, cap, "hardware: invalid PCI numeric argument\n"); return 2;
            }
        uint32_t value;
        if (lunasdk_pci_read(v[0], v[1], v[2], v[3], v[4], &value)) {
            put(out, cap, "hardware: PCI device unavailable or invalid read\n"); return 1;
        }
        put(out, cap, "PCI config: ");
        char h[16]; hex(h, value); put(out, cap, h); put(out, cap, "\n");
        return 0;
    }
    put(out, cap, "dev hardware pci\n");
    put(out, cap, "dev hardware read BUS SLOT FUNCTION OFFSET WIDTH(1|2|4)\n");
    put(out, cap, "Read-only. PCI writes, DMA, MMIO and IRQ require native C drivers.\n");
    return 2;
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
        if (!file || file->dir || !fs_materialize(file)) {
            put(out, cap, "driver: file not found\n");
            return 1;
        }
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
    if (argc >= 2 && !strcmp(argv[1], "hardware"))
        return hardware_command(argc, argv, out, cap);
    if ((argc == 3 || argc == 4) && !strcmp(argv[1], "new")) {
        const char *kind = argc == 4 ? argv[3] : "js";
        const char *ext = !strcmp(kind, "js") ? ".ljs" :
                          !strcmp(kind, "py") ? ".py" :
                          !strcmp(kind, "c") ? ".c" : 0;
        char path[PATH_CAP];
        if (!ext) {
            put(out, cap, "dev: type must be js, py or c\n"); return 2;
        }
        if (!home_filename(argv[2], ext, path) || fs_find(path)) {
            put(out, cap, "dev: invalid name or file already exists\n"); return 1;
        }
        const char *code = !strcmp(kind, "js") ?
            "// LunaSDK v2 (JS) - edit with Notes; run with dev run FILE.ljs\n"
            "console.log('Hello from LunaOS!', luna.version);\n"
            "console.log('Physical PCI devices:', luna.hardware.pci.count());\n"
            "console.log('First PCI device:', luna.hardware.pci.get(0));\n"
            "luna.fs.write('hello.txt', 'Made inside LunaOS');\n" :
            !strcmp(kind, "py") ?
            "# MicroPython on LunaOS\n"
            "import luna\n"
            "print('Hello from LunaOS')\n"
            "print(luna.hardware.pci.count())\n"
            "print(luna.hardware.pci.get(0))\n" :
            "/* LunaSDK C: native driver source for host CMake cross-build.\n"
            " * Edit in Notes, export to a host, then build as freestanding i386 C.\n"
            " * The guest currently has no C compiler or CMake runtime. */\n"
            "#include \"lunasdk_hardware.h\"\n"
            "int sdk_example_probe(LunaPciInfo *device) {\n"
            "    return lunasdk_pci_get(0, device);\n"
            "}\n";
        if (fs_write(path, code, false)) {
            put(out, cap, "dev: could not write app\n"); return 1;
        }
        put(out, cap, "Created "); put(out, cap, kind);
        put(out, cap, " source: "); put(out, cap, path); put(out, cap, "\n");
        if (strcmp(kind, "c")) {
            put(out, cap, "Edit with Notes, then execute: dev run ");
            put(out, cap, path); put(out, cap, "\n");
        } else {
            put(out, cap, "CMake is host-only. See sdk/native/CMakeLists.txt\n");
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "run")) {
        bool js = extension(argv[2], ".ljs");
        bool py = extension(argv[2], ".py");
        if (!in_home(argv[2]) || (!js && !py)) {
            put(out, cap, "dev: only .ljs or .py programs within your home may run\n"); return 2;
        }
        if (py)
            return python_run_file(argv[2], 0, NULL, out, cap);
        File *f = fs_find(argv[2]);
        if (!f || f->dir) { put(out, cap, "dev: program not found\n"); return 1; }
        /* Copy before execution: the script is allowed to overwrite itself. */
        /* Scripts may now be larger than the old 8 KiB LunaFS record.
         * Use kernel-owned scratch and release it after evaluation. */
        char *script = heap_allocate_owned((size_t)f->size + 1u, 1);
        if (!script) { put(out, cap, "dev: insufficient memory for program\n"); return 1; }
        if (!fs_read_text(f, script, (size_t)f->size + 1u)) {
            heap_release(script);
            put(out, cap, "dev: disk read failed\n");
            return 1;
        }
        int result = javascript_eval_sdk(script, f->path, out, cap);
        heap_release(script);
        return result ? 1 : 0;
    }
    if (argc == 2 && !strcmp(argv[1], "list")) {
        const char *home = account_home();
        size_t n = strlen(home);
        for (int i = 0; i < fs_file_capacity; i++)
            if (files[i].used && !files[i].dir && !strncmp(files[i].path, home, n) &&
                files[i].path[n] == '/' && !strchr(files[i].path + n + 1, '/') &&
                (extension(files[i].path, ".ljs") || extension(files[i].path, ".ldrv") ||
                 extension(files[i].path, ".py") || extension(files[i].path, ".c"))) {
                put(out, cap, files[i].path); put(out, cap, "\n");
            }
        return 0;
    }
    put(out, cap, "LunaSDK v2 / native hardware bridge\n");
    put(out, cap, "dev new NAME [js|py|c]  create source in your home\n");
    put(out, cap, "dev run FILE.ljs|.py   execute JS or MicroPython\n");
    put(out, cap, "dev hardware pci      enumerate real PCI devices\n");
    put(out, cap, "dev hardware read ... read PCI configuration safely\n");
    put(out, cap, "dev driver            manage virtual and native device endpoints\n");
    put(out, cap, "dev list              list local development sources\n");
    put(out, cap, "CMake/C native drivers: host toolchain only (no guest ELF loader yet).\n");
    return argc == 1 ? 0 : 2;
}
