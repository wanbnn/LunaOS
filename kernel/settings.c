#include "clock.h"
#include "settings.h"
Preferences prefs = {.mouse_speed = 100,
                     .repeat_delay = 500,
                     .repeat_rate = 30,
                     .brightness = 100,
                     .volume = 70,
                     .frame_hz = 60,
                     .dns_server = "1.1.1.1"};
static void assign(const char *k, int v) {
#define OPT(name, lo, hi)                                                                          \
    if (!strcmp(k, #name)) {                                                                       \
        if (v >= lo && v <= hi)                                                                    \
            prefs.name = v;                                                                        \
        return;                                                                                    \
    }
    OPT(language, 0, 1)
    OPT(developer, 0, 1) OPT(output_mode, 0, 4) OPT(mouse_speed, 25, 200) OPT(left_handed, 0, 1)
        OPT(keyboard_layout, 0, 1) OPT(repeat_delay, 250, 1000) OPT(repeat_rate, 15, 30)
            OPT(brightness, 30, 100) OPT(night_light, 0, 1) OPT(volume, 0, 100)
                OPT(balance, -100, 100) OPT(muted, 0, 1) OPT(wallpaper, 0, 4) OPT(dark, 0, 1)
                    OPT(accent, 0, 5)
#undef OPT
}
void settings_init(void) {
    net_set_dns(prefs.dns_server);
    File *f = fs_find("/etc/luna-settings");
    if (!f)
        return;
    char buf[LUNA_TEXT_BUFFER];
    if (f->size >= sizeof(buf)) return;
    if (!fs_read_text(f, buf, sizeof(buf)))
        return;
    char *p = buf;
    while (*p) {
        char *k = p;
        while (*p && *p != '=' && *p != '\n')
            p++;
        if (*p != '=') {
            if (*p)
                p++;
            continue;
        }
        *p++ = 0;
        char *v = p;
        while (*p && *p != '\n')
            p++;
        char stop = *p;
        *p = 0;
        if (!strcmp(k, "dns_server")) {
            if (net_set_dns(*v ? v : "auto"))
                strcpy(prefs.dns_server,v);
        } else if (!strcmp(k, "wallpaper_path")) {
            if (strlen(v) < PATH_CAP)
                strcpy(prefs.wallpaper_path, v);
        } else
            assign(k, atoi(v));
        if (!stop)
            break;
        p++;
    }
}
bool settings_write(void) {
    char out[2048] = "", b[16];
#define STORE(name)                                                                                \
    append(out, #name "=", sizeof(out));                                                           \
    number(b, prefs.name < 0 ? -prefs.name : prefs.name);                                          \
    if (prefs.name < 0)                                                                            \
        append(out, "-", sizeof(out));                                                             \
    append(out, b, sizeof(out));                                                                   \
    append(out, "\n", sizeof(out));
    STORE(language)
    STORE(developer) STORE(output_mode) STORE(mouse_speed) STORE(left_handed) STORE(keyboard_layout)
        STORE(repeat_delay) STORE(repeat_rate) STORE(brightness) STORE(night_light) STORE(volume)
            STORE(balance) STORE(muted) STORE(wallpaper) STORE(dark) STORE(accent)
#undef STORE
    append(out, "dns_server=", sizeof(out));
    append(out, prefs.dns_server, sizeof(out));
    append(out, "\n", sizeof(out));
                append(out, "wallpaper_path=", sizeof(out));
    append(out, prefs.wallpaper_path, sizeof(out));
    append(out, "\n", sizeof(out));
    return fs_write("/etc/luna-settings", out, false) == 0;
}
void settings_changed(void) {
    net_set_dns(*prefs.dns_server ? prefs.dns_server : "auto");
    static int dark = -1, accent = -1, wallpaper = -1;
    static int brightness = 100, night_light;
    input_apply_settings();
    audio_apply_settings();
    if (brightness != prefs.brightness || night_light != prefs.night_light) {
        brightness = prefs.brightness;
        night_light = prefs.night_light;
        gfx_transform_changed();
        gui_damage_all();
    }
    if (dark != prefs.dark || accent != prefs.accent || wallpaper != prefs.wallpaper) {
        dark = prefs.dark;
        accent = prefs.accent;
        wallpaper = prefs.wallpaper;
        gui_invalidate_all();
    } else
        gui_invalidate_app(4);
}

const char *system_text(const char *en, const char *pt) {
    return prefs.language ? pt : en;
}

bool settings_save(void) {
    return settings_write() && clock_write() && fs_save();
}
