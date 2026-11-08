#include "install.h"
#include "settings.h"
#include "window.h"
#include "webengine.h"
#include "motion.h"
#include "memory.h"
#include "luna_build_version.h"
#define WHITE 0xf2f7ff
#define MUTED 0xb5cbdf
extern int display_brightness;
char browser_url[256] = "luna://welcome", browser_body[131072];
/* Linked stylesheets are much larger than a document, so they get their own
 * download buffer. */
#define CSS_CAP 524288
static char css_body[CSS_CAP];
static char pending_url[256], files_dir[PATH_CAP] = "/home/luna";
static bool browser_edit, browser_loading;
static int selected_file, file_indices[MAX_FILES], file_count;
static uint32_t selected_pid, graph_tick, cpu_history[48];
static unsigned graph_head;
static char task_search[32];
static int task_page, task_scroll;
static void label(int x, int y, const char *s) {
    text(x, y, s, 13, MUTED);
}
static void button(int x, int y, int w, const char *s, bool enabled) {
    round_rect(x, y, w, 34, 14, enabled ? settings_accent() : 0xc6d5e9, enabled ? 220 : 22);
    text(x + (w - text_width(s, 13)) / 2, y + 7, s, 13, enabled ? WHITE : MUTED);
}
static void number_text(int x, int y, unsigned n, int size, uint32_t color) {
    char b[16];
    number(b, n);
    text(x, y, b, size, color);
}
static void wrapped(int x, int y, int width, const char *s, int size, int max_lines, bool html) {
    char row[256];
    int n = 0, lines = 0;
    while (*s && lines < max_lines) {
        char c = *s++;
        if (c == '\r')
            continue;
        if (html && c == '<' &&
            (!strncmp(s, "script", 6) || !strncmp(s, "style", 5) || !strncmp(s, "SCRIPT", 6) ||
             !strncmp(s, "STYLE", 5))) {
            bool script = (*s == 's' && s[1] == 'c') || (*s == 'S' && s[1] == 'C');
            const char *closing = script ? "</script>" : "</style>";
            const char *upper = script ? "</SCRIPT>" : "</STYLE>";
            size_t size = strlen(closing);
            while (*s && strncmp(s, closing, size) && strncmp(s, upper, size))
                s++;
            if (*s)
                s += size;
            continue;
        }
        if (html && c == '<') {
            while (*s && *s != '>')
                s++;
            if (*s)
                s++;
            c = '\n';
        }
        if (c == '\n') {
            row[n] = 0;
            text(x, y, row, size, WHITE);
            n = 0;
            y += size + 8;
            lines++;
            continue;
        }
        if (n >= 240) {
            row[n] = 0;
            text(x, y, row, size, WHITE);
            n = 0;
            y += size + 8;
            lines++;
        }
        row[n++] = c;
        row[n] = 0;
        if (text_width(row, size) > width) {
            row[--n] = 0;
            text(x, y, row, size, WHITE);
            row[0] = c;
            n = 1;
            y += size + 8;
            lines++;
        }
    }
    if (n && lines < max_lines) {
        row[n] = 0;
        text(x, y, row, size, WHITE);
    }
}
static void list_files(void) {
    file_count = 0;
    size_t n = strlen(files_dir);
    for (int i = 0; i < MAX_FILES; i++)
        if (files[i].used) {
            char *p = files[i].path;
            bool child = !strcmp(files_dir, "/") ? p[0] == '/' && p[1] && !strchr(p + 1, '/')
                                                 : !strncmp(p, files_dir, n) && p[n] == '/' &&
                                                       p[n + 1] && !strchr(p + n + 1, '/');
            if (child)
                file_indices[file_count++] = i;
        }
    if (selected_file >= file_count)
        selected_file = file_count ? file_count - 1 : 0;
}
static const char *basename(const char *p) {
    const char *s = p;
    while (*p) {
        if (*p == '/')
            s = p + 1;
        p++;
    }
    return s;
}
static void files_up(void) {
    char raw[160], out[PATH_CAP];
    strcpy(raw, files_dir);
    strcat(raw, "/..");
    if (fs_path(raw, out))
        strcpy(files_dir, out);
    selected_file = 0;
}
static void open_file(void) {
    list_files();
    if (!file_count)
        return;
    File *f = &files[file_indices[selected_file]];
    if (f->dir) {
        strcpy(files_dir, f->path);
        selected_file = 0;
    } else {
        char url[256] = "file://";
        strcat(url, f->path);
        browser_open(url);
    }
}
/* Escapes a message so it can be placed inside an HTML document. */
static void html_escape(char *out, size_t cap, const char *text) {
    out[0] = 0;
    for (const char *p = text; *p && strlen(out) + 8 < cap; p++) {
        if (*p == '<')
            append(out, "&lt;", cap);
        else if (*p == '>')
            append(out, "&gt;", cap);
        else if (*p == '&')
            append(out, "&amp;", cap);
        else if (*p == '"')
            append(out, "&quot;", cap);
        else {
            char one[2] = {*p, 0};
            append(out, one, cap);
        }
    }
}
/* Turns a link target into an absolute address using the current page. */
static void copy_bounded(char *out, size_t cap, const char *text) {
    out[0] = 0;
    size_t n = strlen(text);
    if (n >= cap)
        n = cap - 1;
    memcpy(out, text, n);
    out[n] = 0;
}
static void resolve_link(const char *base, const char *href, char *out, size_t cap) {
    out[0] = 0;
    if (!href || !*href || !cap)
        return;
    if (!strncmp(href, "http://", 7) || !strncmp(href, "https://", 8) || !strncmp(href, "file://", 7) ||
        !strncmp(href, "luna://", 7) || !strncmp(href, "about:", 6)) {
        copy_bounded(out, cap, href);
        return;
    }
    if (href[0] == '/') {
        const char *scheme = "http://";
        const char *p = base;
        if (!strncmp(p, "https://", 8)) {
            scheme = "https://";
            p += 8;
        } else if (!strncmp(p, "http://", 7)) {
            scheme = "http://";
            p += 7;
        }
        if (href[1] == '/') {
            /* Protocol relative reference keeps the scheme of the base page. */
            copy_bounded(out, cap, scheme);
            if (strlen(out) + strlen(href) + 1 < cap)
                strcat(out, href);
            return;
        }
        char host[256] = "";
        size_t n = 0;
        while (p[n] && p[n] != '/' && p[n] != ':' && n < sizeof(host) - 1) {
            host[n] = p[n];
            n++;
        }
        host[n] = 0;
        copy_bounded(out, cap, scheme);
        if (strlen(out) + strlen(host) + strlen(href) + 1 < cap) {
            strcat(out, host);
            strcat(out, href);
        }
        return;
    }
    copy_bounded(out, cap, base);
    char *slash = 0;
    for (char *p = out; *p; p++)
        if (*p == '/')
            slash = p;
    if (slash)
        slash[1] = 0;
    if (strlen(out) + strlen(href) + 1 < cap)
        strcat(out, href);
}
static void browser_error_page(const char *message) {    char escaped[512];
    html_escape(escaped, sizeof(escaped), message);
    char page[3072];
    page[0] = 0;
    append(page,
           "<!doctype html><html><head><style>body{background:#f4f7fb;color:#1d2b3a;"
           "font-family:sans-serif;padding:20px}h1{font-size:28px;color:#0b3b6f}"
           ".card{background:#ffffff;border:1px solid #d6dfea;padding:14px;margin-top:8px}"
           "p{font-size:16px;line-height:22px}</style></head><body><h1>",
           sizeof(page));
    append(page, "This page could not be opened", sizeof(page));
    append(page, "</h1><div class=\"card\"><p>", sizeof(page));
    append(page, escaped, sizeof(page));
    append(page, "</p></div></body></html>", sizeof(page));
    web_load_document("about:error", page, strlen(page), 0, 0);
}
/* Downloads the stylesheets that the document links to and hands them over to
 * the engine, so that real pages keep their visual design. */
static void browser_load_stylesheets(const char *base) {
    char href[512];
    for (int i = 0; i < 6; i++) {
        if (!web_stylesheet_link(i, href, sizeof(href)))
            break;
        char target[512];
        resolve_link(base, href, target, sizeof(target));
        if (!target[0])
            continue;
        char error[192];
        error[0] = 0;
        int length = net_http(target, css_body, sizeof(css_body), error, sizeof(error));
        if (length > 0)
            web_add_stylesheet(css_body, (size_t)length);
    }
    /* Bound both transfers and decoded memory for this small native engine. */
    for (int i = 0; i < 8; i++) {
        if (!web_image_link(i, href, sizeof(href)))
            break;
        char target[512], error[192] = {0};
        resolve_link(base, href, target, sizeof(target));
        if (strncmp(target, "http://", 7) && strncmp(target, "https://", 8))
            continue;
        int length = net_http(target, css_body, sizeof(css_body), error, sizeof(error));
        if (length > 0)
            web_set_image(i, (const unsigned char *)css_body, (size_t)length);
    }
}

static void browser_load(const char *url) {
    char target[256];
    if (strlen(url) >= sizeof(target))
        return;
    strcpy(target, url);
    strcpy(browser_url, target);
    browser_edit = false;
    browser_loading = true;
    gui_invalidate_app(APP_BROWSER);
    if (!strcmp(target, "luna://welcome")) {
        web_reset();
        browser_body[0] = 0;
    } else if (!strncmp(target, "file://", 7)) {
        File *f = fs_find(target + 7);
        if (!f || f->dir) {
            browser_error_page("The file was not found in the Luna file system.");
        } else {
            char error[256];
            error[0] = 0;
            web_load_document(target, f->data, f->size, error, sizeof(error));
            if (error[0])
                browser_error_page(error);
            browser_load_stylesheets(target);
        }
    } else {
        char request[256];
        bool explicit_scheme = !strncmp(target, "http://", 7) || !strncmp(target, "https://", 8);
        if (explicit_scheme)
            strcpy(request, target);
        else {
            strcpy(request, "https://");
            strcat(request, target);
        }
        char error[256];
        error[0] = 0;
        int length = net_http(request, browser_body, sizeof(browser_body), error, sizeof(error));
        if (length < 0 && !explicit_scheme) {
            /* Plain HTTP is only tried when the secure request failed. */
            strcpy(request, "http://");
            strcat(request, target);
            length = net_http(request, browser_body, sizeof(browser_body), error, sizeof(error));
        }
        if (length < 0) {
            browser_error_page(error);
        } else {
            strcpy(browser_url, request);
            char page_error[256];
            page_error[0] = 0;
            web_load_document(request, browser_body, (size_t)length, page_error, sizeof(page_error));
            if (page_error[0])
                browser_error_page(page_error);
            browser_load_stylesheets(request);
        }
    }
    browser_loading = false;
    gui_invalidate_app(APP_BROWSER);
}
bool browser_is_loading(void) {
    return browser_loading;
}
void browser_open(const char *url) {
    if (strlen(url) >= sizeof(pending_url))
        return;
    char target[256];
    strcpy(target, url);
    gui_open(APP_BROWSER);
    strcpy(pending_url, target);
    task_post(windows[APP_BROWSER].pid, (AppEvent){EVENT_NAVIGATE, 0, 0});
}
void app_reset(int app) {
    if (app == APP_FILES) {
        strcpy(files_dir, account_home());
        selected_file = 0;
    } else if (app == APP_BROWSER) {
        strcpy(browser_url, "luna://welcome");
        browser_body[0] = 0;
        browser_edit = browser_loading = false;
        web_reset();
    } else if (app == APP_TASKS) {
        selected_pid = 0;
        task_search[0] = 0;
        task_page = task_scroll = 0;
    }
}
void app_close(int app) {
    if (app==APP_BROWSER) {
        web_close();
        browser_body[0]=0;
        browser_loading=false;
    }
}
static bool matches(const char *s, const char *query) {
    if (!*query)
        return true;
    for (; *s; s++) {
        const char *a = s, *b = query;
        while (*a && *b) {
            char ca = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a,
                 cb = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
            if (ca != cb)
                break;
            a++;
            b++;
        }
        if (!*b)
            return true;
    }
    return false;
}
static unsigned live_count(void) {
    unsigned n = 0;
    for (int i = 0; i < TASK_MAX; i++)
        if (tasks[i].state != TASK_UNUSED && tasks[i].state != TASK_EXITED)
            n++;
    return n;
}
static void kill_selected(void) {
    Task *t = task_by_pid(selected_pid);
    if (t && !t->system && (task_current()->capabilities & CAP_TASK_CONTROL))
        task_kill(t->pid);
}
void app_key(int app, int key) {
    if (app == APP_NOTES) {
        notes_key(key);
        return;
    }
    if (app == APP_SETTINGS) {
        settings_key(key);
        return;
    }
    if (app == APP_SHELL) {
        shell_key(key);
        return;
    }
    if (app == APP_FILES) {
        list_files();
        if (key == KEY_UP && selected_file > 0)
            selected_file--;
        if (key == KEY_DOWN && selected_file + 1 < file_count)
            selected_file++;
        if (key == 10)
            open_file();
        if (key == 8)
            files_up();
    } else if (app == APP_BROWSER) {
        int n = strlen(browser_url);
        if (key == 10 && !browser_loading)
            browser_load(browser_url);
        else if (key == KEY_DOWN && !browser_edit && !browser_loading) {
            web_scroll_by(96);
        } else if (key == KEY_UP && !browser_edit && !browser_loading) {
            web_scroll_by(-96);
        } else if (key == 8) {
            if (!browser_edit) {
                browser_url[0] = 0;
                browser_edit = true;
            } else if (n)
                browser_url[n - 1] = 0;
        } else if (key >= 32 && key < 127 && !browser_loading) {
            if (!browser_edit) {
                browser_url[0] = 0;
                browser_edit = true;
                n = 0;
            }
            if (n < 254) {
                browser_url[n] = key;
                browser_url[n + 1] = 0;
            }
        }
    } else if (app == APP_TASKS) {
        if (key == 127)
            kill_selected();
        else if (key == KEY_DOWN)
            task_scroll++;
        else if (key == KEY_UP && task_scroll > 0)
            task_scroll--;
        else if (key == 8) {
            int n = strlen(task_search);
            if (n)
                task_search[n - 1] = 0;
            task_scroll = 0;
        } else if (key >= 32 && key < 127) {
            int n = strlen(task_search);
            if (n < 30) {
                task_search[n] = key;
                task_search[n + 1] = 0;
                task_scroll = 0;
            }
        }
    }
    gui_invalidate_app(app);
}
void app_click(int app, int x, int y) {
    if (app == APP_NOTES) {
        notes_click(x, y);
        return;
    }
    Window *w = &windows[app];
    int width = w->w - 40, height = w->h - 66;
    if (app == APP_SETTINGS) {
        settings_click(x, y, width, height);
        return;
    }
    if (app == APP_HOME) {
        if (y >= height - 72 && y < height - 34) {
            if (x < 160)
                gui_open(APP_SHELL);
            else if (x < 330)
                gui_open(APP_FILES);
            else
                gui_open(APP_TASKS);
        }
    } else if (app == APP_FILES) {
        if (x < 138) {
            if (y >= 62 && y < 102)
                strcpy(files_dir, account_home());
            if (y >= 110 && y < 150)
                strcpy(files_dir, "/");
            if (y >= 158 && y < 198)
                strcpy(files_dir, "/www");
            if (y >= height - 48)
                fs_save();
            selected_file = 0;
        } else if (y < 42 && x >= width - 78)
            files_up();
        else if (x >= 155 && x < width / 2 + 70 && y >= 66) {
            int row = (y - 66) / 36;
            if (row < file_count) {
                if (selected_file == row)
                    open_file();
                else
                    selected_file = row;
            }
        }
    } else if (app == APP_BROWSER) {
        if (y >= 0 && y < 42 && !browser_loading) {
            browser_url[0] = 0;
            browser_edit = true;
        } else if (y >= height - 66 && y < height - 30 && !strcmp(browser_url, "luna://welcome")) {
            browser_load("file:///www/index.html");
        } else if (y >= 54 && !browser_loading && web_has_document()) {
            char href[256];
            href[0] = 0;
            if (web_click(x, y - 54 + web_scroll_offset(), href, sizeof(href)) && href[0]) {
                char target[256];
                resolve_link(browser_url, href, target, sizeof(target));
                browser_load(target);
            }
        }
    } else if (app == APP_SETTINGS) {
        if (y >= 280 && y < 318 && x >= 42) {
            display_brightness = (x - 42) * 100 / (width - 70);
            if (display_brightness < 30)
                display_brightness = 30;
            if (display_brightness > 100)
                display_brightness = 100;
        }
        if (y >= height - 65 && y < height - 29)
            audio_beep(660, 25);
    } else if (app == APP_TASKS) {
        if (x < 148) {
            if (y >= 65 && y < 105)
                task_page = 0;
            if (y >= 116 && y < 156)
                task_page = 1;
            if (y >= 167 && y < 207)
                task_page = 2;
        } else if (y >= height - 38 && x >= width - 158)
            kill_selected();
        else if (task_page == 0 && y >= 166 && y < height - 48) {
            int target = (y - 166) / 34 + task_scroll, n = 0;
            for (int i = 0; i < TASK_MAX; i++) {
                Task *t = &tasks[i];
                if (t->state == TASK_UNUSED || t->state == TASK_EXITED ||
                    !matches(t->name, task_search))
                    continue;
                if (n++ == target) {
                    selected_pid = t->pid;
                    break;
                }
            }
        }
    }
    gui_invalidate_app(app);
}
void app_event(int app, AppEvent e) {
    if (e.type == EVENT_DRAG && app == APP_SETTINGS) {
        settings_drag(e.a, e.b, windows[app].w - 40, windows[app].h - 66);
        return;
    }
    if (e.type == EVENT_INIT) {
        if (app == APP_SHELL)
            shell_init();
    } else if (e.type == EVENT_KEY)
        app_key(app, e.a);
    else if (e.type == EVENT_CLICK)
        app_click(app, e.a, e.b);
    else if (e.type == EVENT_NAVIGATE && app == APP_BROWSER)
        browser_load(pending_url);
    gui_invalidate_app(app);
}
static void home(int x, int y, int w, int h) {
    label(x + 12, y + 12, "INDEPENDENT BY DESIGN");
    text(x + 9, y + 52, "A little lighter.", 48, WHITE);
    text(x + 9, y + 110, "A little more you.", 48, WHITE);
    text(x + 13, y + 195, "Your own kernel. Your own space.", 20, WHITE);
    text(x + 13, y + 231, "Floating windows. Protected native app clients.", 16, MUTED);
    text(x + 13, y + 264, "Build commit: " LUNA_BUILD_COMMIT, 16, 0xa5d3ff);
    line(x + 12, y + h - 100, x + w - 12, y + h - 100, 0x829bb8);
    button(x + 12, y + h - 72, 144, "Open lunaSH", true);
    button(x + 171, y + h - 72, 144, "Explore files", true);
    button(x + 330, y + h - 72, 170, "Task Manager", true);
}
static void terminal(int x, int y, int w, int h) {
    int rows = (h - 35) / 21;
    if (rows < 1)
        rows = 1;
    int start = term_count > rows ? term_count - rows : 0;
    for (int i = start; i < term_count; i++)
        text(x + 6, y + (i - start) * 21, term_lines[i], 13, 0xe1edfa);
    char prompt[400] = "luna ";
    strcat(prompt, cwd);
    strcat(prompt, " > ");
    strcat(prompt, shell_line);
    char before[400];
    strcpy(before, "luna ");
    strcat(before, cwd);
    strcat(before, " > ");
    int base = strlen(before);
    memcpy(before + base, shell_line, shell_cursor);
    before[base + shell_cursor] = 0;
    int offset = 0;
    while (before[offset] && text_width(before + offset, 13) > w - 20) {
        offset++;
        while (((unsigned char)before[offset] & 0xc0) == 0x80)
            offset++;
    }
    int yy = y + (term_count - start) * 21;
    text(x + 6, yy, prompt + offset, 13, 0xa5d3ff);
    rect(x + 10 + text_width(before + offset, 13), yy + 3, 2, 15, WHITE);
}
static void file_view(int x, int y, int w, int h) {
    list_files();
    round_rect(x, y, 138, h, 14, 0x1c3554, 80);
    label(x + 15, y + 15, "PLACES");
    const char *labels[] = {"Home", "Root", "Web"};
    for (int i = 0; i < 3; i++) {
        round_rect(x + 8, y + 62 + i * 48, 122, 36, 12, 0xffffff, 25);
        icon(x + 17, y + 69 + i * 48, i == 0 ? ICON_HOME : ICON_FOLDER, 20, WHITE);
        text(x + 48, y + 70 + i * 48, labels[i], 16, WHITE);
    }
    label(x + 14, y + h - 80, fs_dirty ? "Unsaved changes" : "Snapshot saved");
    button(x + 8, y + h - 47, 122, "Save to disk", true);
    text(x + 159, y + 4, files_dir, 20, WHITE);
    button(x + w - 78, y, 74, "Up", true);
    int split = w / 2 + 70;
    for (int i = 0; i < file_count && i < (h - 72) / 36; i++) {
        File *f = &files[file_indices[i]];
        int yy = y + 66 + i * 36;
        if (i == selected_file)
            round_rect(x + 150, yy, split - 150, 34, 10, 0xffffff, 32);
        icon(x + 158, yy + 7, f->dir ? ICON_FOLDER : ICON_FILE, 20, WHITE);
        text(x + 187, yy + 7, basename(f->path), 16, WHITE);
    }
    line(x + split + 10, y + 62, x + split + 10, y + h - 12, 0x7893b1);
    if (file_count) {
        File *f = &files[file_indices[selected_file]];
        text(x + split + 28, y + 69, basename(f->path), 16, WHITE);
        label(x + split + 28, y + 106, f->dir ? "FOLDER" : "DOCUMENT PREVIEW");
        wrapped(x + split + 28, y + 140, w - split - 40,
                f->dir ? "Enter to open this folder." : f->data, 13, (h - 150) / 21, false);
    }
}
static void browser(int x, int y, int w, int h) {
    bool welcome = !strcmp(browser_url, "luna://welcome");
    if (!welcome && !browser_loading)
        web_render(x, y + 54, w, h - 54);
    round_rect(x, y, w, 42, 19, 0x1e3550, 70);
    icon(x + 12, y + 9, ICON_GLOBE, 24, WHITE);
    text(x + 49, y + 10, browser_url, 16, WHITE);
    if (browser_edit)
        rect(x + 51 + text_width(browser_url, 16), y + 10, 2, 22, WHITE);
    if (!welcome && browser_url[0]) {
        bool secure = !strncmp(browser_url, "https://", 8);
        const char *scheme = secure ? "HTTPS" : "HTTP";
        text(x + w - 12 - text_width(scheme, 13), y + 14, scheme, 13,
             secure ? 0x86e3ab : 0xffcf8f);
    }
    if (browser_loading) {
        /* Eight tiny primitives instead of frames, textures or trigonometry. */
        static const int8_t sx[8] = {0, 7, 10, 7, 0, -7, -10, -7};
        static const int8_t sy[8] = {-10, -7, 0, 7, 10, 7, 0, -7};
        unsigned phase = motion_spinner_phase(frame_clock);
        for (unsigned i = 0; i < 8; i++) {
            unsigned age = (phase + 8 - i) & 7u;
            round_rect(x + 33 + sx[i], y + 130 + sy[i], 4, 4, 2, WHITE,
                       42 + (int)age * 25);
        }
        text(x + 18, y + 89, "Opening document...", 28, WHITE);
        text(x + 18, y + 137, "Other apps keep running while this one waits.", 16, MUTED);
    } else if (welcome) {
        label(x + 16, y + 76, "LESS NOISE. MORE DISCOVERY.");
        text(x + 13, y + 111, "A quieter corner", 48, WHITE);
        text(x + 13, y + 168, "of the web.", 48, WHITE);
        text(x + 18, y + 251, "Native HTTPS with DOM, CSS and JavaScript.", 20, WHITE);
        label(x + 18, y + 291, "The engine renders real pages inside LunaOS");
        button(x + 18, y + h - 66, 228, "Read the welcome document", true);
    }
}
static unsigned cpu_total(void) {
    unsigned n = 0;
    for (int i = 0; i < TASK_MAX; i++)
        if (tasks[i].state != TASK_UNUSED && tasks[i].state != TASK_EXITED)
            n += tasks[i].cpu_permille;
    return n > 1000 ? 1000 : n;
}
static void percent(int x, int y, unsigned value) {
    char b[32], n[16];
    number(b, value / 10);
    strcat(b, ".");
    number(n, value % 10);
    strcat(b, n);
    strcat(b, "%");
    text(x, y, b, 13, WHITE);
}
static void memory_label(char out[32],uint64_t bytes) {
    char fraction[16];
    if (bytes<1024*1024) {
        number(out,(uint32_t)((bytes+1023)/1024)); strcat(out," KiB");
    } else {
        number(out,(uint32_t)(bytes/(1024*1024))); strcat(out,".");
        number(fraction,(uint32_t)((bytes%(1024*1024))*10/(1024*1024)));
        strcat(out,fraction); strcat(out," MiB");
    }
}
static void task_manager(int x, int y, int w, int h) {
    unsigned cpu = cpu_total();
    if (ticks - graph_tick >= 100) {
        cpu_history[graph_head++ % 48] = cpu;
        graph_tick = ticks;
    }
    round_rect(x, y, 146, h, 14, 0x10233b, 100);
    text(x + 15, y + 14, "Resources", 20, WHITE);
    const char *pages[] = {"Processes", "Memory", "System"};
    const int icons[] = {ICON_COMMAND, ICON_MEMORY, ICON_SETTINGS};
    for (int i = 0; i < 3; i++) {
        round_rect(x + 7, y + 65 + i * 51, 132, 38, 10, 0xffffff, task_page == i ? 35 : 8);
        icon(x + 16, y + 75 + i * 51, icons[i], 20, WHITE);
        text(x + 44, y + 75 + i * 51, pages[i], 13, WHITE);
    }
    label(x + 16, y + h - 137, "CPU UTILIZATION");
    percent(x + 16, y + h - 108, cpu);
    for (int i = 1; i < 48; i++) {
        int a = cpu_history[(graph_head + i - 1) % 48], b = cpu_history[(graph_head + i) % 48];
        line(x + 13 + (i - 1) * 120 / 47, y + h - 22 - a * 54 / 1000, x + 13 + i * 120 / 47,
             y + h - 22 - b * 54 / 1000, 0x82bbff);
    }
    int left = x + 166, width = w - 166;
    text(left, y + 2, pages[task_page], 28, WHITE);
    char summary[100], n[16];
    number(summary, live_count());
    strcat(summary, " tasks  /  ");
    number(n, memory_mb);
    strcat(summary, n);
    strcat(summary, " MiB firmware RAM");
    label(left, y + 46, summary);
    if (task_page == 0) {
        round_rect(left, y + 77, width, 34, 12, 0xffffff, 16);
        icon(left + 10, y + 84, ICON_SEARCH, 20, MUTED);
        text(left + 40, y + 85, *task_search ? task_search : "Type to search processes...", 13,
             MUTED);
        const char *cols[] = {"Process", "PID", "Ring", "CPU", "RAM", "State"};
        int pos[] = {0, 204, 250, 295, 360, 451};
        for (int i = 0; i < 6; i++)
            label(left + pos[i], y + 131, cols[i]);
        line(left, y + 157, left + width, y + 157, 0x506c89);
        int index = 0, row = 0, available = (h - 218) / 34;
        for (int i = 0; i < TASK_MAX; i++) {
            Task *t = &tasks[i];
            if (t->state == TASK_UNUSED || t->state == TASK_EXITED ||
                !matches(t->name, task_search))
                continue;
            if (index++ < task_scroll)
                continue;
            if (row >= available)
                break;
            int yy = y + 166 + row++ * 34;
            if (t->pid == selected_pid)
                round_rect(left - 5, yy, width + 5, 32, 8, 0x779ee5, 56);
            icon(left + 1, yy + 7,
                 t->app >= 0 ? (t->app == APP_SHELL     ? ICON_TERMINAL
                                : t->app == APP_FILES   ? ICON_FOLDER
                                : t->app == APP_BROWSER ? ICON_GLOBE
                                : t->app == APP_TASKS   ? ICON_MEMORY
                                                        : ICON_SLIDERS)
                             : ICON_SETTINGS,
                 20, WHITE);
            char name[25];
            memset(name, 0, sizeof(name));
            for (int j = 0; j < 23 && t->name[j]; j++)
                name[j] = t->name[j];
            text(left + 29, yy + 8, name, 13, WHITE);
            number_text(left + 204, yy + 8, t->pid, 13, WHITE);
            number_text(left + 250, yy + 8, t->observed_ring == 255 ? t->ring : t->observed_ring,
                        13, WHITE);
            percent(left + 295, yy + 8, t->cpu_permille);
            TaskMemoryStats memory; task_memory_stats(t,&memory);
            char mem[32]; memory_label(mem,memory.ram_bytes);
            text(left + 360, yy + 8, mem, 13, WHITE);
            text(left + 451, yy + 8, task_state_name(t->state), 13, MUTED);
        }
        Task *selected = task_by_pid(selected_pid);
        label(left, y + h - 27,
              selected && selected->system ? "System task: protected"
                                           : "RAM: heap pages + stacks + page tables");
        button(x + w - 154, y + h - 38, 154, "End task",
               selected && !selected->system && selected->state != TASK_EXITED);
    } else if (task_page == 1) {
        MemoryStats memory; memory_stats(&memory);
        number_text(left,y+98,(uint32_t)(memory.firmware_usable>>20),48,WHITE);
        text(left+208,y+125,"MiB usable firmware RAM",16,MUTED);
        const char *labels[]={"Addressable RAM", "Free physical pages", "Allocated heap + DMA", "Reserved kernel / firmware"};
        uint64_t values[]={memory.addressable_usable,memory.free_bytes,memory.allocated_bytes,memory.reserved_bytes};
        for (unsigned i=0;i<4;i++) {
            char amount[32]; memory_label(amount,values[i]);
            text(left,y+190+(int)i*31,labels[i],16,MUTED);
            text(left+width-130,y+190+(int)i*31,amount,16,WHITE);
        }
        wrapped(left,y+333,width,
                "Process RAM counts physical heap arenas, stacks and page tables once. "
                "Kernel RAM includes drivers and shared buffers. No fixed per-process RAM quota.",
                14,4,false);
    } else {
        const char *info =
            "Ring 0: kernel, drivers and compositor.\nRing 1: reserved service privilege.\nRing 2: "
            "reserved service privilege.\nRing 3: native application clients.\n\nEach client has "
            "private stack pages and a page directory. Kernel memory is supervisor-only. I/O "
            "requires a controlled system call.\n\nSuper opens app search. Drag title bars to move "
            "windows. The red, yellow and green controls close, minimize and maximize.";
        wrapped(left, y + 87, width, info, 16, (h - 90) / 24, false);
    }
}
void app_draw(int app, int x, int y, int w, int h) {
    Task *owner=task_for_app(app);
    uint32_t previous=task_memory_enter(owner ? owner->pid : 1);
    switch (app) {
    case APP_NOTES:
        notes_draw(x, y, w, h);
        break;
    case APP_HOME:
        home(x, y, w, h);
        break;
    case APP_SHELL:
        terminal(x, y, w, h);
        break;
    case APP_FILES:
        file_view(x, y, w, h);
        break;
    case APP_BROWSER:
        browser(x, y, w, h);
        break;
    case APP_SETTINGS:
        settings_draw(x, y, w, h);
        break;
    case APP_TASKS:
        task_manager(x, y, w, h);
        break;
    }
    task_memory_leave(previous);
}
