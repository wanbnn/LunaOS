#include "update.h"
#include "calendar.h"
#include "wifi_profiles.h"
#include "acpi.h"
#include "settings.h"
#include "window.h"
#define WHITE 0xf2f7ff
#define MUTED 0xb9cee5
static int page, slider = -1;
static bool path_edit, resolution_open;
static char status[192] = "Changes apply immediately. Save to keep them after restart.";
static const char *pages[] = {"Keyboard",   "Mouse",   "Displays", "Sound",
                              "Appearance", "Network", "System", "Date & time"};
static const char *pages_pt[] = {"Teclado",   "Mouse", "Monitores", "Som",
                                 "Aparência", "Rede",  "Sistema", "Data e hora"};
static const char *page_name(int i) {
    return system_text(pages[i], pages_pt[i]);
}
static const int page_icons[] = {ICON_COMMAND, ICON_CURSOR,   ICON_SUN,    ICON_VOLUME,
                                 ICON_MOON,    ICON_ETHERNET, ICON_SLIDERS, ICON_SUN};
static uint32_t accents[] = {0x518bea, 0x7e76e7, 0x359e99, 0xd279a5, 0xc58d51, 0x8296ab};
uint32_t settings_accent(void) {
    return accents[prefs.accent % 6];
}
static bool in(int x, int y, int a, int b, int w, int h) {
    return x >= a && y >= b && x < a + w && y < b + h;
}
static void label(int x, int y, const char *s) {
    text(x, y, s, 13, MUTED);
}
static void num(int x, int y, unsigned v) {
    char b[24];
    number(b, v);
    text(x, y, b, 16, WHITE);
}
static void toggle(int x, int y, bool on) {
    round_rect(x, y, 44, 25, 12, on ? settings_accent() : 0x9ab0c7, on ? 235 : 45);
    circle(x + (on ? 31 : 13), y + 12, 9, WHITE);
}
static void range(int x, int y, int width, int value, int lo, int hi) {
    round_rect(x, y + 9, width, 6, 3, 0xb6cae2, 35);
    int fill = (value - lo) * width / (hi - lo);
    if (fill < 0)
        fill = 0;
    if (fill > width)
        fill = width;
    round_rect(x, y + 9, fill, 6, 3, settings_accent(), 255);
    circle(x + fill, y + 12, 8, WHITE);
}
static void row(int x, int y, int w, const char *name, const char *value) {
    round_rect(x, y, w, 55, 12, 0xffffff, 16);
    static const char *en[] = {"Resolution",
                               "Compositor target",
                               "Higher refresh modes",
                               "Layout",
                               "Repeat delay",
                               "Repeat rate",
                               "Pointer speed",
                               "Left-handed buttons",
                               "Pointer acceleration",
                               "Output device",
                               "Mute",
                               "Dark glass",
                               "Typeface",
                               "Language / Idioma",
                               0};
    static const char *pt[] = {"Resolução",
                               "Meta do compositor",
                               "Taxas de atualização",
                               "Layout",
                               "Atraso de repetição",
                               "Taxa de repetição",
                               "Velocidade do cursor",
                               "Botões para canhotos",
                               "Aceleração do cursor",
                               "Dispositivo de saída",
                               "Silenciar",
                               "Vidro escuro",
                               "Tipografia",
                               "Idioma"};
    if (prefs.language)
        for (int i = 0; en[i]; i++)
            if (!strcmp(name, en[i])) {
                name = pt[i];
                break;
            }
    text(x + 17, y + 16, name, 16, WHITE);
    text(x + w - text_width(value, 13) - 18, y + 18, value, 13, MUTED);
}
static void button(int x, int y, int w, const char *s) {
    round_rect(x, y, w, 32, 13, settings_accent(), 210);
    text(x + (w - text_width(s, 13)) / 2, y + 7, s, 13, WHITE);
}
static WifiAp network_aps[WIFI_MAX_APS], network_selected;
static unsigned network_count, network_first;
static bool network_password;
static char network_secret[WIFI_MAX_PASSWORD + 1];
static const char *network_message = "";
static const char *network_state(void) {
    WifiState state;
    if (wifi_state(&state)) return system_text("Wi-Fi unavailable", "Wi-Fi indisponível");
    if (state.link == WIFI_SCANNING) return system_text("Scanning...", "Buscando...");
    if (state.link == WIFI_ASSOCIATING) return system_text("Connecting...", "Conectando...");
    if (state.error) return system_text("Connection failed. Retry.", "Conexão falhou. Tente novamente.");
    if (state.link == WIFI_ASSOCIATED) return system_text("Waiting for IP...", "Aguardando IP...");
    if (state.link == WIFI_ONLINE) return system_text("Connected", "Conectado");
    if (!state.can_connect) return system_text("Passive scan only", "Somente busca passiva");
    return system_text("Disconnected", "Desconectado");
}
static void network_scan(void) {
    network_password = false;
    memset(network_secret, 0, sizeof(network_secret));
    WifiState state;
    if (!wifi_state(&state) && state.link >= WIFI_ASSOCIATING) {
        network_message = system_text("Scan paused while connected or connecting.",
                                      "Busca pausada durante a conexão.");
        return;
    }
    int rc = wifi_user_scan();
    network_message = rc ? system_text("Scan unavailable or busy. Retry.", "Busca indisponível ou ocupada. Tente novamente.") : "";
    network_first = 0;
}
static void network_connect(void) {
    int rc = wifi_user_connect(&network_selected, network_secret);
    memset(network_secret, 0, sizeof(network_secret));
    network_password = false;
    network_message = rc ? system_text("Cannot connect. Check password (8-63 characters) and retry.",
                                       "Falha ao conectar. Verifique a senha (8-63 caracteres).") : "";
}
static bool refresh_network_results(void) {
    WifiState state;
    if (wifi_state(&state) || state.link == WIFI_SCANNING)
        return false;
    WifiAp results[WIFI_MAX_APS] = {0};
    unsigned count = 0;
    if (wifi_scan_results(results, WIFI_MAX_APS, &count))
        return false;
    if (count == network_count && !memcmp(results, network_aps, count * sizeof(WifiAp)))
        return false;
    memcpy(network_aps, results, count * sizeof(WifiAp));
    network_count = count;
    if (network_first >= network_count)
        network_first = 0;
    return true;
}
void settings_network_refresh(void) {
    bool settings_visible = page == 5 && windows[APP_SETTINGS].open &&
                            !windows[APP_SETTINGS].minimized;
    if (!settings_visible && !controls_open)
        return;
    static const char *previous_state;
    static char previous_ssid[WIFI_MAX_SSID + 1];
    static bool previous_net;
    const char *state = network_state(), *ssid = wifi_connection_ssid();
    bool changed = state != previous_state || strcmp(ssid, previous_ssid);
    previous_state = state;
    strcpy(previous_ssid, ssid);
    if (settings_visible && (refresh_network_results() || changed))
        gui_invalidate_app(APP_SETTINGS);
    if (controls_open && (changed || net_ready != previous_net))
        gui_damage_quick();
    previous_net = net_ready;
}
static void network_draw(int a, int b, int width) {
    WifiState state;
    bool ready = !wifi_state(&state);
    row(a, b + 83, width, system_text("DNS (click to change)", "DNS (clique para trocar)"),
        *prefs.dns_server ? prefs.dns_server : system_text("Automatic (DHCP)", "Automático (DHCP)"));
    row(a, b + 146, width, *wifi_connection_ssid() ? wifi_connection_ssid() : "Wi-Fi", network_state());
    button(a, b + 215, 150, system_text("Scan networks", "Buscar redes"));
    if (network_count > 3) {
        button(a + width - 170, b + 215, 80, "<");
        button(a + width - 80, b + 215, 80, ">");
    }
    refresh_network_results();
    for (unsigned i = 0; i < 3 && network_first + i < network_count; i++) {
        WifiAp *ap = &network_aps[network_first + i];
        row(a, b + 258 + i * 55, width, ap->ssid,
            ap->security == WIFI_OPEN ? system_text("Open", "Aberta") :
            ap->security == WIFI_WPA2_PSK ? "WPA2" : system_text("Unsupported security", "Segurança incompatível"));
    }
    if (!network_count) label(a + 12, b + 277, ready ? system_text("No networks found. Scan again.", "Nenhuma rede encontrada. Busque novamente.") : system_text("No operational Wi-Fi adapter.", "Nenhum adaptador Wi-Fi operacional."));
    if (network_password) {
        char masked[WIFI_MAX_PASSWORD + 1];
        unsigned n = strlen(network_secret);
        memset(masked, '*', n); masked[n] = 0;
        const char *prompt = wifi_profile_known(&network_selected)
            ? system_text("Blank: use saved password", "Em branco: usar senha salva")
            : system_text("Password", "Senha");
        row(a, b + 431, width - 125, prompt, masked);
        button(a + width - 115, b + 442, 115, system_text("Connect", "Conectar"));
    } else label(a + 8, b + 440, network_message);
}
void settings_draw(int x, int y, int w, int h) {
    round_rect(x, y, 176, h, 15, 0x13283e, 95);
    text(x + 17, y + 14, system_text("Settings", "Configurações"), 20, WHITE);
    for (int i = 0; i < 8; i++) {
        int yy = y + 62 + i * 47;
        round_rect(x + 8, yy, 160, 37, 11, 0xffffff, i == page ? 35 : 5);
        icon(x + 17, yy + 9, page_icons[i], 20, WHITE);
        text(x + 48, yy + 10, page_name(i), 13, WHITE);
    }
    button(x + 12, y + h - 46, 152, system_text("Save settings", "Salvar ajustes"));
    int a = x + 198, b = y, width = w - 198;
    text(a, b + 3, page_name(page), 28, WHITE);
    label(a, b + 47, page == 5 ? system_text("Select a Wi-Fi network to connect.", "Selecione uma rede Wi-Fi para conectar.") : status);
    if (page == 7) { time_settings_draw(a,b,width); return; }
    if (page == 0) {
        row(a, b + 83, width, "Layout",
            prefs.keyboard_layout ? "Portuguese / ABNT2 basics" : "English / US QWERTY");
        label(a + 16, b + 145, "Click the layout row to change mapping.");
        row(a, b + 180, width, "Repeat delay",
            prefs.repeat_delay == 250   ? "250 ms"
            : prefs.repeat_delay == 500 ? "500 ms"
            : prefs.repeat_delay == 750 ? "750 ms"
                                        : "1000 ms");
        row(a, b + 246, width, "Repeat rate",
            prefs.repeat_rate == 30 ? "30 keys / second" : "15 keys / second");
        row(a, b + 320, width, "Window shortcuts", "Alt+Tab / Alt+Shift+Tab / Alt+F4");
        label(a + 16, b + 391, "Super: app search. Ctrl+Alt+T: terminal.");
    } else if (page == 1) {
        row(a, b + 83, width, "Pointer speed", "25% - 200% / linear");
        range(a + 22, b + 150, width - 84, prefs.mouse_speed, 25, 200);
        num(a + width - 50, b + 149, prefs.mouse_speed);
        row(a, b + 207, width, "Left-handed buttons", "");
        toggle(a + width - 67, b + 222, prefs.left_handed);
        row(a, b + 286, width, "Pointer acceleration", "Disabled / direct PS/2 movement");
        label(a + 16, b + 363, "The cursor updates independently of application rendering.");
        label(a + 16, b + 387, "Hotspot follows the tip of the Lucide pointer.");
    } else if (page == 2) {
        row(a, b + 83, width, "Resolution", gfx_mode_name(display_mode));
        row(a, b + 150, width, "Compositor target", "60 fps / 16.7 ms frame budget");
        row(a, b + 217, width, "Higher refresh modes", "Unavailable with this scanout driver");
        label(a + 15, b + 287, "QEMU: modes supplied by the virtual display adapter.");
        label(a + 15, b + 311, "Rendering at native resolution / interface scale 100%.");
        char buf[100] = "Last composition: ", n[16];
        number(n, compositor_render_ms);
        strcat(buf, n);
        strcat(buf, " ms");
        if (prefs.developer)
            label(a + 15, b + 348, buf);
        row(a, b + 377, width, "Scanout",
            gfx_fast_scanout() ? "32-bit native framebuffer / cached windows"
                               : "VBE fallback / no fixed-rate guarantee");
    } else if (page == 3) {
        row(a, b + 83, width, "Output device", audio_name());
        text(a + 17, b + 156, "Output volume", 16, WHITE);
        range(a + 22, b + 185, width - 90, prefs.volume, 0, 100);
        num(a + width - 52, b + 184, prefs.volume);
        text(a + 17, b + 237, "Balance", 16, WHITE);
        range(a + 22, b + 265, width - 70, prefs.balance, -100, 100);
        row(a, b + 318, width, "Mute", "");
        toggle(a + width - 67, b + 333, prefs.muted);
        button(a + 10, b + 391, 150, "Test sound");
        label(a + 179, b + 400, "Microphone input is not implemented.");
    } else if (page == 4) {
        row(a, b + 83, width, "Dark glass", "");
        toggle(a + width - 67, b + 98, prefs.dark);
        row(a, b + 140, width, "Typeface", graphics_font_family_name(prefs.font_family));
        text(a + 12, b + 206, "Accent color", 16, WHITE);
        for (int i = 0; i < 6; i++) {
            int xx = a + 24 + i * 51;
            circle(xx, b + 250, 15, accents[i]);
            if (i == prefs.accent)
                icon(xx - 8, b + 242, ICON_CHEVRON_RIGHT, 16, WHITE);
        }
        text(a + 12, b + 280, "Wallpaper", 16, WHITE);
        int tw = (width - 36) / 4;
        for (int i = 0; i < 4; i++) {
            int xx = a + 8 + i * (tw + 6);
            gfx_draw_wall_preview(xx, b + 310, tw, 56, i);
            if (prefs.wallpaper == i)
                round_rect(xx, b + 368, tw, 4, 2, settings_accent(), 255);
        }
        button(a + 10, b + 388, 170, "Load PPM from file");
        round_rect(a + 192, b + 387, width - 200, 35, 12, 0x102238, 80);
        text(a + 204, b + 396, *prefs.wallpaper_path ? prefs.wallpaper_path : "/home/luna/wall.ppm",
             13, MUTED);
        label(a + 12, b + 436, "PPM P3 wallpaper: max 64 x 64 pixels.");
    } else if (page == 6) {
        row(a, b + 83, width, "Language / Idioma",
            prefs.language ? "Português (Brasil)" : "English");
        row(a, b + 150, width, prefs.language ? "Modo de desenvolvedor" : "Developer mode", "");
        toggle(a + width - 67, b + 165, prefs.developer);
        label(a + 14, b + 230,
              prefs.language ? "Idioma aplicado aos controles do sistema disponíveis."
                             : "Language applies to available system controls.");
        char revision[96] = "";
        append(revision, system_text("Installed commit: ", "Commit instalado: "), sizeof(revision));
        append(revision, update_revision(), sizeof(revision));
        label(a + 14, b + 291, revision);
        button(a + 10, b + 318, 210, system_text("Check for updates", "Verificar atualizações"));
        label(a + 14, b + 365, *update_status() ? update_status() : system_text("Updates from the official LunaOS repository.", "Atualizações do repositório oficial do LunaOS."));
        label(a + 14, b + 390, update_detail());
        if (update_busy()) range(a + 14, b + 420, width - 40, update_progress(), 0, 100);
        label(a + 14, b + 264,
              prefs.developer ? "Diagnostics enabled: composition timing and system details."
                              : "Enable developer mode to show rendering diagnostics.");
    } else {
        network_draw(a, b, width);
    }
    if (page == 2 && resolution_open) {
        int yy = b + 142;
        round_rect(a, yy, width, gfx_mode_count() * 36 + 8, 12, 0x16283f, 255);
        for (int i = 0; i < gfx_mode_count(); i++) {
            if (i == display_mode)
                round_rect(a + 4, yy + 4 + i * 36, width - 8, 34, 8, settings_accent(), 180);
            text(a + 16, yy + 12 + i * 36, gfx_mode_name(i), 13, WHITE);
        }
    }
}
void settings_key(int key) {
    if(page==7){time_settings_key(key);return;}
    int code = key & 0xfff;
    if (page == 5 && network_password) {
        unsigned n = strlen(network_secret);
        if (code == 27) { network_password = false; memset(network_secret, 0, sizeof(network_secret)); }
        else if (code == 10) network_connect();
        else if (code == 8 && n) network_secret[n - 1] = 0;
        else if (code >= 32 && code < 127 && n < WIFI_MAX_PASSWORD) {
            network_secret[n] = code; network_secret[n + 1] = 0;
        }
        gui_invalidate_app(APP_SETTINGS);
        return;
    }
    if (!path_edit)
        return;
    int n = strlen(prefs.wallpaper_path);
    if (code == 8 && n)
        prefs.wallpaper_path[n - 1] = 0;
    else if (code == 27)
        path_edit = false;
    else if (code == 10) {
        bool ok = gfx_load_wallpaper(prefs.wallpaper_path);
        strcpy(status, ok ? "Wallpaper loaded. Save to keep this choice."
                          : "Cannot load wallpaper: expected a small valid PPM P3 file.");
        if (ok) {
            prefs.wallpaper = 4;
            gfx_rebuild_wallpaper();
            gui_invalidate_all();
        }
        path_edit = false;
    } else if (code >= 32 && code < 127 && n < PATH_CAP - 1) {
        prefs.wallpaper_path[n] = code;
        prefs.wallpaper_path[n + 1] = 0;
    }
    gui_invalidate_app(APP_SETTINGS);
}
static void apply_slider(int x, int width) {
    int v = x - 198 - 22, span = width - 198 - 84;
    if (span < 1)
        return;
    if (v < 0)
        v = 0;
    if (v > span)
        v = span;
    if (slider == 0)
        prefs.mouse_speed = 25 + v * 175 / span;
    if (slider == 1)
        prefs.volume = v * 100 / span;
    if (slider == 2)
        prefs.balance = v * 200 / span - 100;
    settings_changed();
}
void settings_click(int x, int y, int width, int height) {
    path_edit = false;
    slider = -1;
    if (page == 2 && resolution_open) {
        resolution_open = false;
        if (x >= 198 && y >= 146 && y < 146 + gfx_mode_count() * 36) {
            bool ok = gfx_change_mode((y - 146) / 36);
            strcpy(status,
                   ok ? "Native resolution applied. Interface scale: 100%." : "Mode unavailable.");
        }
        gui_invalidate_app(APP_SETTINGS);
        return;
    }
    if (page == 2 && x >= 198 && y >= 83 && y < 138) {
        resolution_open = true;
        gui_invalidate_app(APP_SETTINGS);
        return;
    }
    if (x < 176) {
        if (y >= 62 && y < 438)
            { page = (y - 62) / 47; if(page==7)time_settings_load(); if(page==5)network_scan(); else { network_password=false; memset(network_secret,0,sizeof(network_secret)); } }
        if (y >= height - 46) {
            bool ok = settings_save();
            strcpy(status, ok ? "Settings saved to disk."
                              : "Save failed; preferences remain active in memory.");
        }
        gui_invalidate_app(APP_SETTINGS);
        return;
    }
    if(page==7){time_settings_click(x-198,y);return;}
    int a = x - 198, w = width - 198;
    if (page == 5 && a >= 0 && a < w) {
        if (y >= 83 && y < 138) {
            const char *next=!strcmp(prefs.dns_server,"1.1.1.1") ? "8.8.8.8" :
                             !strcmp(prefs.dns_server,"8.8.8.8") ? "" : "1.1.1.1";
            strcpy(prefs.dns_server,next);
            settings_changed();
            network_message=system_text("DNS applied. Save settings to keep it.",
                                        "DNS aplicado. Salve as configurações para manter.");
        } else if (y >= 215 && y < 247) {
            if (a < 150) network_scan();
            else if (a >= w - 80 && network_first + 3 < network_count) network_first += 3;
            else if (a >= w - 170 && a < w - 90 && network_first >= 3) network_first -= 3;
        } else if (y >= 258 && y < 423) {
            unsigned i = network_first + (y - 258) / 55;
            WifiState state;
            if (i < network_count && !wifi_state(&state) && state.can_connect && network_aps[i].security != WIFI_OTHER) {
                network_selected = network_aps[i];
                memset(network_secret, 0, sizeof(network_secret));
                network_password = network_selected.security == WIFI_WPA2_PSK;
                network_message = "";
                if (!network_password) network_connect();
            }
        } else if (y >= 442 && y < 474 && a >= w - 115 && network_password) network_connect();
        gui_invalidate_app(APP_SETTINGS);
        return;
    }
    if (page == 6) {
        if (y >= 318 && y < 350 && a >= 10 && a < 220) { update_run(); return; }
        if (y >= 83 && y < 138)
            prefs.language = !prefs.language;
        if (y >= 150 && y < 205)
            prefs.developer = !prefs.developer;
    }
    if (page == 0) {
        if (y >= 83 && y < 138)
            prefs.keyboard_layout = !prefs.keyboard_layout;
        if (y >= 180 && y < 235)
            prefs.repeat_delay = prefs.repeat_delay == 1000 ? 250 : prefs.repeat_delay + 250;
        if (y >= 246 && y < 301)
            prefs.repeat_rate = prefs.repeat_rate == 30 ? 15 : 30;
    }
    if (page == 1) {
        if (y >= 140 && y < 180)
            slider = 0;
        if (y >= 207 && y < 262)
            prefs.left_handed = !prefs.left_handed;
    }
    if (page == 3) {
        if (y >= 175 && y < 220)
            slider = 1;
        if (y >= 255 && y < 299)
            slider = 2;
        if (y >= 318 && y < 373)
            prefs.muted = !prefs.muted;
        if (y >= 391 && y < 425)
            audio_beep(660, 25);
    }
    if (page == 4) {
        if (y >= 83 && y < 138)
            prefs.dark = !prefs.dark;
        if (y >= 140 && y < 195) {
            prefs.font_family = (prefs.font_family + 1) % graphics_font_family_count();
            settings_changed();
        }
        if (y >= 230 && y < 270 && a >= 0 && a < 320)
            prefs.accent = (a + 1) / 51 % 6;
        if (y >= 310 && y < 372) {
            int tw = (w - 36) / 4;
            int choice = (a - 8) / (tw + 6);
            if (choice >= 0 && choice < 4) {
                prefs.wallpaper = choice;
                gfx_rebuild_wallpaper();
            }
        }
        if (y >= 387 && y < 424) {
            if (a < 184) {
                bool ok = gfx_load_wallpaper(prefs.wallpaper_path);
                strcpy(status,
                       ok ? "Wallpaper loaded." : "Enter a PPM path in the field and press Enter.");
                if (ok) {
                    prefs.wallpaper = 4;
                    gfx_rebuild_wallpaper();
                }
            } else {
                path_edit = true;
                prefs.wallpaper_path[0] = 0;
            }
        }
    }
    if (slider >= 0)
        apply_slider(x, width);
    else
        settings_changed();
}
void settings_drag(int x, int y, int width, int height) {
    (void)y;
    (void)height;
    if (slider >= 0)
        apply_slider(x, width);
}
void quick_draw(void) {
    gfx_set_origin(W - GFX_BOOT_W, 0);
    glass(688, 58, 316, 448, 25, 1);
    text(710, 78, system_text("Control Center", "Central de controle"), 20, WHITE);
    char battery[64];
    acpi_battery_text(battery, sizeof(battery));
    text(710, 103, battery, 13, MUTED);
    row(706, 122, 278, "Ethernet", net_ready ? "Connected" : "Unavailable");
    icon(716, 195, ICON_WIFI, 24, MUTED);
    text(753, 199, *wifi_connection_ssid() ? wifi_connection_ssid() : "Wi-Fi", 13, WHITE);
    text(753, 218, network_state(), 13, MUTED);
    icon(716, 237, ICON_BLUETOOTH, 24, MUTED);
    text(753, 241, "Bluetooth: not implemented", 13, MUTED);
    text(710, 291, "Brightness", 16, WHITE);
    range(711, 321, 261, prefs.brightness, 30, 100);
    text(710, 366, "Volume", 16, WHITE);
    range(711, 395, 261, prefs.volume, 0, 100);
    button(710, 452, 269, "Open Settings");
}
bool quick_click(int x, int y) {
    x -= W - GFX_BOOT_W;
    if (!in(x, y, 688, 58, 316, 448))
        return false;
    if (in(x, y, 706, 182, 278, 54)) {
        page = 5;
        network_scan();
        controls_open = false;
        gui_open(APP_SETTINGS);
        gui_invalidate_app(APP_SETTINGS);
    }
    if (y >= 452 && y < 486) {
        controls_open = false;
        gui_open(APP_SETTINGS);
    }
    return true;
}
void quick_pointer(int x, int y, bool pressed, bool released) {
    x -= W - GFX_BOOT_W;
    static int quick_slider = -1;
    if (released) {
        quick_slider = -1;
        return;
    }
    if (pressed) {
        if (!quick_click(x + W - GFX_BOOT_W, y)) {
            controls_open = false;
            gui_damage_quick();
            return;
        }
        quick_slider = y >= 310 && y < 348 ? 0 : y >= 384 && y < 425 ? 1 : -1;
    }
    if (quick_slider >= 0) {
        int v = x - 711;
        if (v < 0)
            v = 0;
        if (v > 261)
            v = 261;
        int *setting = quick_slider == 0 ? &prefs.brightness : &prefs.volume;
        int value = quick_slider == 0 ? 30 + v * 70 / 261 : v * 100 / 261;
        if (*setting != value) {
            *setting = value;
            settings_changed();
            gui_damage_quick();
        }
    }
}
void quick_key(int key) {
    (void)key;
}

void settings_open_time(void){page=7;time_settings_load();gui_invalidate_app(APP_SETTINGS);}

void settings_clock_refresh(void){if(page==7&&windows[APP_SETTINGS].open)gui_invalidate_app(APP_SETTINGS);}
