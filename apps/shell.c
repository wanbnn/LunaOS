#include "clock.h"
#include "../drivers/rtw8822be_diag.h"
#include "../drivers/iwl_pcie.h"
#include "memory.h"
#include "../drivers/rtw8822be_pcie.h"
#include "driver.h"
#include "devkit.h"
#include "install.h"
#include "luna.h"
#include "settings.h"
#include "task.h"
#include "web.h"
char term_lines[160][180], shell_line[256];
int term_count, shell_cursor;
static char history[32][256];
static int history_count, history_pos, status, depth;
static struct {
    char name[32], value[256];
} vars[16];
static const char *commands[] = {
    "help",   "ls",    "cd",      "pwd",      "cat",     "echo",    "mkdir",    "touch", "write",
    "rm",     "rmdir", "cp",      "mv",       "head",    "tail",    "wc",       "grep",  "clear",
    "uname",  "free",  "uptime",  "date",     "pci",     "net",     "ifconfig", "ping",  "wget",
    "browse", "audio", "beep",    "sync",     "history", "source",  "set",      "env",   "true",
    "false",  "sleep", "reboot",  "poweroff", "about",   "ps",      "tasks",    "kill",  "ringtest",
    "js",     "dev",   "bench", "drvinfo", "whoami",   "setup",   "drvtest", "fwinfo",   "wifidiag", "memtest", 0};
#define CAP 8192
static void emit(char *out, const char *s) {
    append(out, s, CAP);
}
static void num(char *out, unsigned n) {
    char s[16];
    number(s, n);
    emit(out, s);
}
void shell_print(const char *s) {
    serial_write(s);
    int col = 0;
    if (!term_count)
        term_count = 1;
    col = strlen(term_lines[term_count - 1]);
    while (*s) {
        if (*s == '\r') {
            s++;
            continue;
        }
        if (*s == '\n' || col >= 118) {
            if (term_count == 160) {
                memmove(term_lines, term_lines + 1, 159 * 180);
                term_count = 159;
            }
            term_lines[term_count++][0] = 0;
            col = 0;
            if (*s == '\n') {
                s++;
                continue;
            }
        }
        term_lines[term_count - 1][col++] = *s++;
        term_lines[term_count - 1][col] = 0;
    }
    gui_invalidate_app(APP_SHELL);
}
static const char *value(const char *name) {
    static char code[16];
    if (!strcmp(name, "PWD"))
        return cwd;
    if (!strcmp(name, "HOME"))
        return account_home();
    if (!strcmp(name, "?")) {
        number(code, status);
        return code;
    }
    for (int i = 0; i < 16; i++)
        if (!strcmp(name, vars[i].name))
            return vars[i].value;
    return "";
}
static int tokenize(const char *line, char tok[96][512]) {
    int n = 0, k = 0;
    char quote = 0;
    bool have = false;
    memset(tok, 0, 96 * 512);
    for (int i = 0;; i++) {
        char c = line[i];
        if (!c) {
            if (quote)
                return -1;
            if (have)
                n++;
            return n;
        }
        if (!quote && (c == ' ' || c == '\t')) {
            if (have) {
                n++;
                k = 0;
                have = false;
            }
            if (n >= 95)
                return -2;
            continue;
        }
        if (!quote && (c == '|' || c == '>' || c == '<' || c == ';' || c == '&')) {
            if (have) {
                n++;
                k = 0;
                have = false;
            }
            if (n >= 94)
                return -2;
            tok[n][0] = 1;
            tok[n][1] = c;
            if (line[i + 1] == c && c != ';' && c != '<')
                tok[n][2] = line[++i];
            n++;
            continue;
        }
        if (!quote && c == '#' && !have) {
            if (have)
                n++;
            return n;
        }
        if ((c == '\'' || c == '"') && (!quote || quote == c)) {
            have = true;
            if (quote)
                quote = 0;
            else
                quote = c;
            continue;
        }
        if (c == '\\' && quote != '\'') {
            if (line[i + 1])
                c = line[++i];
            else
                return -1;
        } else if (c == '$' && quote != '\'') {
            char name[32];
            int z = 0;
            if (line[i + 1] == '?') {
                name[z++] = '?';
                i++;
            } else
                while (z < 31 && ((line[i + 1] >= 'A' && line[i + 1] <= 'Z') ||
                                  (line[i + 1] >= 'a' && line[i + 1] <= 'z') ||
                                  (line[i + 1] >= '0' && line[i + 1] <= '9') || line[i + 1] == '_'))
                    name[z++] = line[++i];
            name[z] = 0;
            const char *v = z ? value(name) : "$";
            while (*v) {
                if (k >= 511)
                    return -2;
                tok[n][k++] = *v++;
            }
            have = true;
            continue;
        }
        if (k >= 511 || n >= 95)
            return -2;
        tok[n][k++] = c;
        have = true;
    }
}
static const char *content(int argc, char **argv, const char *input, char *out) {
    if (argc > 1) {
        File *f = fs_find(argv[1]);
        if (!f || f->dir) {
            emit(out, "error: file not found\n");
            return 0;
        }
        return f->data;
    }
    return input ? input : "";
}
static bool direct_child(const char *p) {
    size_t n = strlen(cwd);
    if (!strcmp(cwd, "/"))
        return p[0] == '/' && p[1] && !strchr(p + 1, '/');
    return !strncmp(p, cwd, n) && p[n] == '/' && p[n + 1] && !strchr(p + n + 1, '/');
}
static int execute(int argc, char **a, const char *input, char *out) {
    out[0] = 0;
    if (!argc)
        return 0;
    const char *c = a[0];
    if (!strcmp(c, "help")) {
        emit(out, "lunaSH / native command interpreter\n");
        for (int i = 0; commands[i]; i++) {
            emit(out, commands[i]);
            emit(out, (i % 7 == 6) ? "\n" : "  ");
        }
        emit(out, "\nSyntax: quotes, variables, |, >, >>, <, ;, &&, ||\nSuper: app search | "
                  "Alt+Tab: switch | Alt+F4: close | Tab: complete\n");
    } else if (!strcmp(c, "echo")) {
        for (int i = 1; i < argc; i++) {
            if (i > 1)
                emit(out, " ");
            emit(out, a[i]);
        }
        emit(out, "\n");
    } else if (!strcmp(c, "pwd")) {
        emit(out, cwd);
        emit(out, "\n");
    } else if (!strcmp(c, "cd")) {
        File *f = fs_find(argc > 1 ? a[1] : account_home());
        if (!f || !f->dir) {
            emit(out, "cd: directory not found\n");
            return 1;
        }
        strcpy(cwd, f->path);
    } else if (!strcmp(c, "ls")) {
        char old[PATH_CAP];
        strcpy(old, cwd);
        if (argc > 1) {
            File *f = fs_find(a[1]);
            if (!f) {
                emit(out, "ls: path not found\n");
                return 1;
            }
            if (!f->dir) {
                emit(out, f->path);
                emit(out, "\n");
                return 0;
            }
            strcpy(cwd, f->path);
        }
        for (int i = 0; i < MAX_FILES; i++)
            if (files[i].used && direct_child(files[i].path)) {
                char *p = files[i].path;
                char *base = p;
                while (*p) {
                    if (*p == '/')
                        base = p + 1;
                    p++;
                }
                emit(out, files[i].dir ? "[dir]  " : "       ");
                emit(out, base);
                if (!files[i].dir) {
                    emit(out, "  ");
                    num(out, files[i].size);
                    emit(out, " B");
                }
                emit(out, "\n");
            }
        strcpy(cwd, old);
    } else if (!strcmp(c, "cat")) {
        if (argc == 1)
            emit(out, input ? input : "");
        for (int i = 1; i < argc; i++) {
            File *f = fs_find(a[i]);
            if (!f || f->dir) {
                emit(out, "cat: file not found\n");
                return 1;
            }
            emit(out, f->data);
        }
    } else if (!strcmp(c, "mkdir") || !strcmp(c, "touch")) {
        if (argc != 2) {
            emit(out, "usage: mkdir|touch PATH\n");
            return 2;
        }
        File *f = fs_find(a[1]);
        if (!strcmp(c, "touch") && f && !f->dir)
            return 0;
        if (fs_create(a[1], !strcmp(c, "mkdir"))) {
            emit(out, "error: cannot create path (parent, capacity or existing entry)\n");
            return 1;
        }
    } else if (!strcmp(c, "rm") || !strcmp(c, "rmdir")) {
        if (argc != 2 || fs_remove(a[1])) {
            emit(out, "error: cannot remove path (missing, protected or nonempty)\n");
            return 1;
        }
    } else if (!strcmp(c, "write")) {
        if (argc < 3) {
            emit(out, "usage: write FILE TEXT\n");
            return 2;
        }
        char b[4096] = "";
        for (int i = 2; i < argc; i++) {
            if (i > 2)
                append(b, " ", sizeof(b));
            append(b, a[i], sizeof(b));
        }
        append(b, "\n", sizeof(b));
        if (fs_write(a[1], b, false)) {
            emit(out, "write: failed\n");
            return 1;
        }
    } else if (!strcmp(c, "cp") || !strcmp(c, "mv")) {
        File *f = argc == 3 ? fs_find(a[1]) : 0;
        if (!f || f->dir) {
            emit(out, "usage: cp|mv SOURCE_FILE DEST_FILE\n");
            return 2;
        }
        char p[PATH_CAP];
        if (!fs_path(a[2], p) || !strcmp(f->path, p)) {
            emit(out, "error: invalid or identical destination\n");
            return 1;
        }
        if (fs_write(p, f->data, false)) {
            emit(out, "error: cannot write destination\n");
            return 1;
        }
        if (!strcmp(c, "mv") && fs_remove(a[1])) {
            emit(out, "mv: source could not be removed\n");
            return 1;
        }
    } else if (!strcmp(c, "head") || !strcmp(c, "tail") || !strcmp(c, "wc") || !strcmp(c, "grep")) {
        const char *s;
        if (!strcmp(c, "grep")) {
            if (argc < 2) {
                emit(out, "usage: grep TEXT [FILE]\n");
                return 2;
            }
            if (argc > 2) {
                File *f = fs_find(a[2]);
                if (!f || f->dir) {
                    emit(out, "grep: file not found\n");
                    return 1;
                }
                s = f->data;
            } else
                s = input ? input : "";
            bool match = false;
            while (*s) {
                const char *end = s;
                while (*end && *end != '\n')
                    end++;
                bool hit = false;
                size_t pattern = strlen(a[1]);
                for (const char *p = s; p + pattern <= end; p++)
                    if (!strncmp(p, a[1], pattern)) {
                        hit = true;
                        break;
                    }
                if (hit) {
                    while (s < end) {
                        char b[2] = {*s++, 0};
                        emit(out, b);
                    }
                    emit(out, "\n");
                    match = true;
                }
                s = *end ? end + 1 : end;
            }
            return match ? 0 : 1;
        }
        s = content(argc, a, input, out);
        if (!s)
            return 1;
        if (!strcmp(c, "wc")) {
            unsigned lines = 0, words = 0, bytes = 0;
            bool inword = false;
            for (const char *p = s; *p; p++) {
                bytes++;
                if (*p == '\n')
                    lines++;
                if (*p == ' ' || *p == '\n' || *p == '\t')
                    inword = false;
                else if (!inword) {
                    words++;
                    inword = true;
                }
            }
            num(out, lines);
            emit(out, " lines  ");
            num(out, words);
            emit(out, " words  ");
            num(out, bytes);
            emit(out, " bytes\n");
        } else {
            if (!strcmp(c, "tail")) {
                int lines = 0;
                for (const char *p = s; *p; p++)
                    if (*p == '\n')
                        lines++;
                while (lines > 10 && *s)
                    if (*s++ == '\n')
                        lines--;
            }
            int lines = 0;
            while (*s && lines < 10) {
                char b[2] = {*s++, 0};
                emit(out, b);
                if (b[0] == '\n')
                    lines++;
            }
        }
    } else if (!strcmp(c, "clear")) {
        memset(term_lines, 0, sizeof(term_lines));
        term_count = 1;
    } else if (!strcmp(c, "uname")) {
        emit(out, "LunaOS " VERSION " i386 native-kernel\n");
    } else if (!strcmp(c, "about")) {
        emit(out, "LunaOS / independent OS research prototype\nNative C kernel, BIOS boot, Inter "
                  "Tight and software glass compositor.\nNo Linux, Bun, libc or external shell "
                  "runs inside this image.\n");
    } else if (!strcmp(c, "free")) {
        memory_status(out,CAP);
    } else if (!strcmp(c, "memtest")) {
        uint32_t mib=4;
        if (argc>2) { emit(out,"usage: memtest [MiB] (1..4095)\n"); return 2; }
        if (argc==2) {
            mib=0;
            for (const char *p=a[1]; *p; p++) {
                if (*p<'0' || *p>'9' || mib>4095) { emit(out,"usage: memtest [MiB] (1..4095)\n"); return 2; }
                mib=mib*10+(unsigned)(*p-'0');
            }
            if (!mib || mib>4095) { emit(out,"usage: memtest [MiB] (1..4095)\n"); return 2; }
        }
        return memory_test(mib,out,CAP) ? 1 : 0;
    } else if (!strcmp(c, "uptime")) {
        num(out, ticks / 100);
        emit(out, " seconds since kernel timer start\n");
    } else if (!strcmp(c, "date")) {
        bool ok = true;
        if (argc == 4 && !strcmp(a[1], "set")) {
            DateTime d;
            ok = datetime_parse(a[2], a[3], &d) && clock_set(&d);
        } else if (argc == 3 && !strcmp(a[1], "zone")) {
            int z;
            ok = datetime_offset_parse(a[2], &z) && clock_zone(z);
        } else if (argc == 3 && !strcmp(a[1], "auto") &&
                   (!strcmp(a[2], "on") || !strcmp(a[2], "off")))
            clock_automatic(!strcmp(a[2], "on"));
        else if (argc == 2 && !strcmp(a[1], "sync"))
            clock_sync();
        else if ((argc == 3 || argc == 4) && !strcmp(a[1], "server")) {
            unsigned p = 123;
            if (argc == 4) {
                p = 0;
                for (const char *q = a[3]; *q; q++) {
                    if (*q < '0' || *q > '9' || p > 65535) {
                        p = 0;
                        break;
                    }
                    p = p * 10 + *q - '0';
                }
            }
            ok = clock_endpoint(a[2], p);
        } else if (argc != 1)
            ok = false;
        if (!ok) {
            emit(out, "usage: date [set YYYY-MM-DD HH:MM:SS | zone +/-HH:MM | auto on|off | sync | "
                      "server HOST [PORT]]\n");
            return 2;
        }
        DateTime d;
        char b[20], z[7];
        if (clock_local(&d)) {
            datetime_format(&d, b);
            emit(out, b);
        }
        datetime_offset_format(clock_offset(), z);
        emit(out, " UTC");
        emit(out, z);
        emit(out, "\n");
        emit(out, clock_status());
        emit(out, "\nNTP: ");
        emit(out, clock_server());
        emit(out, clock_auto() ? " (automatic)\n" : " (manual)\n");
    } else if (!strcmp(c, "pci"))
        pci_list(out, CAP);
    else if (!strcmp(c, "net") || !strcmp(c, "ifconfig"))
        net_status(out, CAP);
    else if (!strcmp(c, "ping")) {
        if (argc != 2) {
            emit(out, "usage: ping HOST\n");
            return 2;
        }
        return net_ping(a[1], out, CAP) ? 0 : 1;
    } else if (!strcmp(c, "wget")) {
        if (argc != 3) {
            emit(out, "usage: wget http://HOST/PATH OUTPUT_FILE\n");
            return 2;
        }
        char body[8192], err[256];
        int n = net_http(a[1], body, sizeof(body), err, sizeof(err));
        if (n < 0) {
            emit(out, "wget: ");
            emit(out, err);
            emit(out, "\n");
            return 1;
        }
        if (fs_write(a[2], body, false)) {
            emit(out, "wget: cannot save file\n");
            return 1;
        }
        emit(out, "Saved ");
        num(out, n);
        emit(out, " bytes to ");
        emit(out, a[2]);
        emit(out, "\n");
    } else if (!strcmp(c, "browse")) {
        browser_open(argc > 1 ? a[1] : "luna://welcome");
        gui_open(3);
    } else if (!strcmp(c, "beep")) {
        unsigned hz = argc > 1 ? atoi(a[1]) : 660;
        if (hz < 20 || hz > 20000) {
            emit(out, "beep: frequency must be 20..20000 Hz\n");
            return 2;
        }
        audio_beep(hz, 20);
        emit(out, "Tone started: ");
        emit(out, audio_name());
        emit(out, "\n");
    } else if (!strcmp(c, "audio"))
        audio_status(out, CAP);
    else if (!strcmp(c, "sync")) {
        if (!fs_save()) {
            emit(out, "sync: ATA write failed; files remain in RAM\n");
            return 1;
        }
        emit(out, "LunaFS: snapshot committed to disk\n");
    } else if (!strcmp(c, "history")) {
        for (int i = 0; i < history_count; i++) {
            num(out, i + 1);
            emit(out, "  ");
            emit(out, history[i]);
            emit(out, "\n");
        }
    } else if (!strcmp(c, "source")) {
        File *f = argc == 2 ? fs_find(a[1]) : 0;
        if (!f || f->dir || depth >= 8) {
            emit(out, "source: file unavailable or recursion limit\n");
            return 1;
        }
        char script[LUNA_TEXT_BUFFER];
        if (f->size >= sizeof(script)) {
            emit(out, "source: script exceeds shell execution buffer; split the script\n");
            return 1;
        }
        strcpy(script, f->data);
        depth++;
        char *p = script;
        while (*p) {
            char *start = p;
            while (*p && *p != '\n')
                p++;
            char sep = *p;
            *p = 0;
            shell_run(start);
            if (!sep)
                break;
            p++;
        }
        depth--;
        return status;
    } else if (!strcmp(c, "set")) {
        if (argc != 3 || strlen(a[1]) > 31 || strlen(a[2]) > 255) {
            emit(out, "usage: set NAME VALUE\n");
            return 2;
        }
        for (const char *p = a[1]; *p; p++)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p == '_') ||
                  (p > a[1] && *p >= '0' && *p <= '9'))) {
                emit(out, "set: invalid variable name\n");
                return 2;
            }
        for (int i = 0; i < 16; i++)
            if (!strcmp(vars[i].name, a[1]) || !vars[i].name[0]) {
                strcpy(vars[i].name, a[1]);
                strcpy(vars[i].value, a[2]);
                return 0;
            }
        emit(out, "set: variable limit reached\n");
        return 1;
    } else if (!strcmp(c, "env")) {
        emit(out, "HOME=");
        emit(out, account_home());
        emit(out, "\nUSER=");
        emit(out, account_name());
        emit(out, "\nPWD=");
        emit(out, cwd);
        emit(out, "\n");
        for (int i = 0; i < 16; i++)
            if (vars[i].name[0]) {
                emit(out, vars[i].name);
                emit(out, "=");
                emit(out, vars[i].value);
                emit(out, "\n");
            }
    } else if (!strcmp(c, "true"))
        return 0;
    else if (!strcmp(c, "false"))
        return 1;
    else if (!strcmp(c, "sleep")) {
        int s = argc == 2 ? atoi(a[1]) : -1;
        if (s < 0 || s > 60) {
            emit(out, "sleep: range 0..60 seconds\n");
            return 2;
        }
        sleep_ticks(s * 100);
    } else if (!strcmp(c, "js")) {
        if (argc != 2) {
            emit(out, "usage: js 'JavaScript source'\n");
            return 2;
        }
        return javascript_eval(a[1], out, CAP) == 0 ? 0 : 1;
    } else if (!strcmp(c, "dev")) {
        return developer_command(argc, a, out, CAP);
    } else if (!strcmp(c, "setup")) {
        if (fs_find("/etc/setup-complete")) {
            emit(out, "Already installed. Boot installation media to install on another disk.\n");
            return 1;
        }
        setup_init();
        return 0;
    } else if (!strcmp(c, "whoami")) {
        emit(out, account_name());
        emit(out, "\n");
        return 0;
    } else if (!strcmp(c, "wifidiag")) {
        if (argc == 2 && !strcmp(a[1], "intel")) return wifi_intel_diag(out, CAP) ? 1 : 0;
        if (rtw8822be_present()) { rtw8822be_status(out, CAP); return 0; }
        if (argc == 1) return rtw8822be_diag(out, CAP) ? 1 : 0;
        if (argc == 2 && !strcmp(a[1], "power"))
            return rtw8822be_power_test(out, CAP) ? 1 : 0;
        emit(out, "usage: wifidiag [power|intel] (MAC power cycle only; no scan/connect)\n");
        return 2;
    } else if (!strcmp(c, "drvinfo")) {
        driver_services_status(out, CAP);
        return 0;
    } else if (!strcmp(c, "drvtest")) {
        return driver_services_test(out, CAP) ? 1 : 0;
    } else if (!strcmp(c, "fwinfo")) {
        if (argc != 2) {
            emit(out, "usage: fwinfo relative/firmware.bin\n");
            return 2;
        }
        DrvFirmware fw = {0};
        int r = drv_firmware_request(a[1], &fw);
        if (r) {
            emit(out, "firmware error: -");
            num(out, -r);
            emit(out, "\n");
            return 1;
        }
        emit(out, "Firmware bytes: ");
        num(out, fw.size);
        emit(out, "\nFNV32: ");
        char h[16];
        hex(h, fw.checksum);
        emit(out, h);
        emit(out, "\n");
        drv_firmware_release(&fw);
        return 0;
    } else if (!strcmp(c, "bench")) {
        gui_benchmark(out, CAP);
        return 0;
    } else if (!strcmp(c, "ps"))
        task_list(out, CAP);
    else if (!strcmp(c, "tasks"))
        gui_open(APP_TASKS);
    else if (!strcmp(c, "kill")) {
        if (argc != 2 || !(task_current()->capabilities & CAP_TASK_CONTROL) ||
            !task_kill(atoi(a[1]))) {
            emit(out, "kill: task not found or protected\n");
            return 1;
        }
        emit(out, "Task terminated.\n");
    } else if (!strcmp(c, "ringtest"))
        task_ring_tests(out, CAP);
    else if (!strcmp(c, "reboot") || !strcmp(c, "poweroff")) {
        if (!(task_current()->capabilities & CAP_SYSTEM)) {
            emit(out, "Permission denied: ring-0 system capability required.\n");
            return 1;
        }
        if (!strcmp(c, "reboot"))
            reboot();
        else
            poweroff();
    } else {
        emit(out, "lunaSH: unknown command: ");
        emit(out, c);
        emit(out, "\n");
        return 127;
    }
    return 0;
}
static bool is_operator(const char *token, const char *op) {
    return token[0] == 1 && !strcmp(token + 1, op);
}
void shell_run(const char *line) {
    if (strlen(line) > 4095) {
        shell_print("lunaSH: line too long\n");
        status = 2;
        return;
    }
    char tok[96][512];
    int n = tokenize(line, tok);
    if (n < 0) {
        shell_print("lunaSH: unclosed quote or parser capacity exceeded\n");
        status = 2;
        return;
    }
    char input[CAP] = "", out[CAP] = "";
    int pos = 0;
    bool run = true;
    while (pos < n) {
        char *argv[64];
        int argc = 0;
        char *dest = 0, *src = 0;
        bool add = false;
        while (pos < n && !is_operator(tok[pos], "|") && !is_operator(tok[pos], ";") &&
               !is_operator(tok[pos], "&&") && !is_operator(tok[pos], "||")) {
            if (is_operator(tok[pos], ">") || is_operator(tok[pos], ">>") ||
                is_operator(tok[pos], "<")) {
                bool reading = is_operator(tok[pos], "<");
                if (!reading)
                    add = is_operator(tok[pos], ">>");
                pos++;
                if (pos >= n || !tok[pos][0] || tok[pos][0] == 1) {
                    shell_print("lunaSH: missing redirection path\n");
                    status = 2;
                    return;
                }
                if (reading)
                    src = tok[pos++];
                else
                    dest = tok[pos++];
            } else {
                if (argc >= 63) {
                    shell_print("lunaSH: too many arguments\n");
                    status = 2;
                    return;
                }
                if (tok[pos][0] == 1) {
                    shell_print("lunaSH: unsupported operator\n");
                    status = 2;
                    return;
                }
                argv[argc++] = tok[pos++];
            }
        }
        if (!argc) {
            shell_print("lunaSH: empty command\n");
            status = 2;
            return;
        }
        if (run) {
            bool valid = true;
            if (src) {
                File *f = fs_find(src);
                if (!f || f->dir) {
                    strcpy(out, "lunaSH: input file not found\n");
                    valid = false;
                    status = 1;
                } else
                    if (f->size >= sizeof(input)) {
                        strcpy(out, "lunaSH: input exceeds pipeline buffer\n");
                        valid = false;
                        status = 1;
                    } else strcpy(input, f->data);
            }
            if (valid)
                status = execute(argc, argv, input, out);
            if (dest && valid) {
                if (fs_write(dest, out, add)) {
                    strcpy(out, "lunaSH: redirection failed\n");
                    status = 1;
                } else
                    out[0] = 0;
            }
        }
        const char *sep = pos < n ? tok[pos++] : "";
        if (is_operator(sep, "|")) {
            if (pos == n) {
                shell_print("lunaSH: missing pipeline command\n");
                status = 2;
                return;
            }
            if (run)
                strcpy(input, out);
        } else {
            if (run)
                shell_print(out);
            input[0] = out[0] = 0;
            if (is_operator(sep, "&&"))
                run = status == 0;
            else if (is_operator(sep, "||"))
                run = status != 0;
            else
                run = true;
        }
    }
}
static bool last_tab;
static void replace_span(int begin, int end, const char *word) {
    int length = strlen(shell_line), n = strlen(word);
    if (length - (end - begin) + n > 255)
        return;
    memmove(shell_line + begin + n, shell_line + end, length - end + 1);
    memcpy(shell_line + begin, word, n);
    shell_cursor = begin + n;
}
static void complete(void) {
    int begin = shell_cursor;
    while (begin > 0 && shell_line[begin - 1] != ' ' && shell_line[begin - 1] != '\t' &&
           shell_line[begin - 1] != '|' && shell_line[begin - 1] != ';')
        begin--;
    char prefix[256];
    int length = shell_cursor - begin;
    memcpy(prefix, shell_line + begin, length);
    prefix[length] = 0;
    bool command = true;
    for (int i = 0; i < begin; i++)
        if (shell_line[i] != ' ' && shell_line[i] != '\t')
            command = false;
    char matches[MAX_FILES + 64][256];
    int count = 0;
    if (command) {
        for (int i = 0; commands[i] && count < MAX_FILES + 64; i++)
            if (!strncmp(commands[i], prefix, length))
                strcpy(matches[count++], commands[i]);
    } else {
        char directory[PATH_CAP], fragment[256], lead[256] = "";
        int slash = -1;
        for (int i = 0; i < length; i++)
            if (prefix[i] == '/')
                slash = i;
        if (slash >= 0) {
            memcpy(lead, prefix, slash + 1);
            lead[slash + 1] = 0;
            strcpy(fragment, prefix + slash + 1);
            if (!fs_path(lead, directory))
                return;
        } else {
            strcpy(directory, cwd);
            strcpy(fragment, prefix);
        }
        size_t dn = strlen(directory);
        bool only_dirs = !strncmp(shell_line, "cd ", 3);
        for (int i = 0; i < MAX_FILES && count < MAX_FILES + 64; i++) {
            File *f = &files[i];
            if (!f->used || (only_dirs && !f->dir))
                continue;
            const char *name = 0;
            if (!strcmp(directory, "/")) {
                if (f->path[0] == '/' && f->path[1])
                    name = f->path + 1;
            } else if (!strncmp(f->path, directory, dn) && f->path[dn] == '/')
                name = f->path + dn + 1;
            if (!name || strchr(name, '/') || strncmp(name, fragment, strlen(fragment)))
                continue;
            char *m = matches[count++];
            strcpy(m, lead);
            strcat(m, name);
            if (f->dir)
                strcat(m, "/");
        }
    }
    if (!count) {
        last_tab = false;
        return;
    }
    unsigned common = strlen(matches[0]);
    for (int i = 1; i < count; i++) {
        unsigned j = 0;
        while (j < common && matches[0][j] == matches[i][j])
            j++;
        common = j;
    }
    while (common > 0 && ((unsigned char)matches[0][common] & 0xc0) == 0x80)
        common--;
    char result[256];
    memcpy(result, matches[0], common);
    result[common] = 0;
    if (count == 1) {
        strcpy(result, matches[0]);
        int n = strlen(result);
        if (n && result[n - 1] != '/' && begin + length == (int)strlen(shell_line) && n < 254)
            strcat(result, " ");
        replace_span(begin, shell_cursor, result);
        last_tab = false;
    } else if (common > (unsigned)length) {
        replace_span(begin, shell_cursor, result);
        last_tab = true;
    } else {
        if (last_tab) {
            shell_print("\n");
            for (int i = 0; i < count; i++) {
                shell_print(matches[i]);
                shell_print(i % 4 == 3 ? "\n" : "    ");
            }
            shell_print("\n");
        }
        last_tab = true;
    }
}
void shell_key(int key) {
    int c = key & 0xfff, len = strlen(shell_line);
    if ((key & MOD_CTRL) && (c == 'c' || c == 'C')) {
        shell_print("^C\n");
        shell_line[0] = 0;
        shell_cursor = 0;
        gui_invalidate_app(APP_SHELL);
        return;
    }
    if ((key & MOD_CTRL) && (c == 'l' || c == 'L')) {
        memset(term_lines, 0, sizeof(term_lines));
        term_count = 1;
        gui_invalidate_app(APP_SHELL);
        return;
    }
    if ((key & MOD_CTRL) && (c == 'a' || c == 'A'))
        c = KEY_HOME;
    if ((key & MOD_CTRL) && (c == 'e' || c == 'E'))
        c = KEY_END;
    if (c != 9)
        last_tab = false;
    if (c == 10 || c == 13) {
        shell_print("luna ");
        shell_print(cwd);
        shell_print(" > ");
        shell_print(shell_line);
        shell_print("\n");
        if (len) {
            if (history_count == 32) {
                memmove(history, history + 1, 31 * 256);
                history_count = 31;
            }
            strcpy(history[history_count++], shell_line);
            shell_run(shell_line);
        }
        shell_line[0] = 0;
        shell_cursor = 0;
        history_pos = history_count;
        serial_write("luna> ");
    } else if (c == 8 || c == 127) {
        if (shell_cursor > 0) {
            int start = shell_cursor - 1;
            while (start > 0 && ((unsigned char)shell_line[start] & 0xc0) == 0x80)
                start--;
            memmove(shell_line + start, shell_line + shell_cursor, len - shell_cursor + 1);
            shell_cursor = start;
            serial_write("\b \b");
        }
    } else if (c == KEY_DELETE) {
        if (shell_cursor < len) {
            int end = shell_cursor + 1;
            while (end < len && ((unsigned char)shell_line[end] & 0xc0) == 0x80)
                end++;
            memmove(shell_line + shell_cursor, shell_line + end, len - end + 1);
        }
    } else if (c == KEY_LEFT) {
        if (shell_cursor > 0) {
            shell_cursor--;
            while (shell_cursor > 0 && ((unsigned char)shell_line[shell_cursor] & 0xc0) == 0x80)
                shell_cursor--;
        }
    } else if (c == KEY_RIGHT) {
        if (shell_cursor < len) {
            shell_cursor++;
            while (shell_cursor < len && ((unsigned char)shell_line[shell_cursor] & 0xc0) == 0x80)
                shell_cursor++;
        }
    } else if (c == KEY_HOME)
        shell_cursor = 0;
    else if (c == KEY_END)
        shell_cursor = len;
    else if (c == KEY_UP) {
        if (history_pos > 0)
            strcpy(shell_line, history[--history_pos]);
        shell_cursor = strlen(shell_line);
    } else if (c == KEY_DOWN) {
        if (history_pos < history_count - 1)
            strcpy(shell_line, history[++history_pos]);
        else {
            history_pos = history_count;
            shell_line[0] = 0;
        }
        shell_cursor = strlen(shell_line);
    } else if (c == 9)
        complete();
    else if (c >= 32 && c <= 255 && c != KEY_SUPER && c != KEY_ALT_RELEASE && c != KEY_HOME &&
             c != KEY_END && c != KEY_DELETE && !(key & (MOD_ALT | MOD_CTRL))) {
        char added[3];
        int n = 1;
        added[0] = c;
        added[1] = 0;
        if (c >= 128) {
            added[0] = 0xc0 | (c >> 6);
            added[1] = 0x80 | (c & 63);
            added[2] = 0;
            n = 2;
        }
        if (len + n < 256) {
            memmove(shell_line + shell_cursor + n, shell_line + shell_cursor,
                    len - shell_cursor + 1);
            memcpy(shell_line + shell_cursor, added, n);
            shell_cursor += n;
            serial_write(added);
        }
    }
    gui_invalidate_app(APP_SHELL);
}
void shell_init(void) {
    memset(term_lines, 0, sizeof(term_lines));
    memset(shell_line, 0, sizeof(shell_line));
    memset(history, 0, sizeof(history));
    memset(vars, 0, sizeof(vars));
    history_count = history_pos = status = depth = 0;
    term_count = 1;
    shell_cursor = 0;
    last_tab = false;
    shell_print("lunaSH " VERSION " / your space to build.\n");
    shell_print("Type help to explore. Files persist when you run sync.\n\n");
    serial_write("luna> ");
}
