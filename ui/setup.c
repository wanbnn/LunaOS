#include "boot.h"
#include "install.h"
#include "settings.h"
#include "task.h"
#include "web.h"
#include "wifi.h"
#include "storage.h"
#define INK 0x20344f
#define SECONDARY 0x586d85
#define BLUE 0x316ade
#define RED 0xb83b4d
bool setup_active;
int setup_page;
static int wizard_language = 1, field_index, selected_disk = -1, selected_region;
static uint32_t page_started, greeting_clock, reboot_at;
static int greeting, previous_buttons, install_phase;
static uint32_t install_sector, source_bytes;
static bool detected, hashing, installed, login_busy;
static bool eligible[STORAGE_DEVICE_COUNT];
static unsigned storage_page, storage_visible_count;
static unsigned storage_visible[STORAGE_DEVICE_COUNT];
static char username[25], password[65], confirmation[65], erase_text[32], layout_test[64];
static char radio[512], message[192];
static WifiAp wifi_aps[WIFI_MAX_APS];
static WifiState wifi_link;
static unsigned wifi_count, wifi_first;
static int wifi_selected = -1, wifi_scan_rc, wifi_connect_rc, wifi_state_rc;
static char wifi_password[WIFI_MAX_PASSWORD + 1];
static uint32_t wifi_last_refresh;
static uint8_t boot_sector[512], transfer[512], verify_sector[512];
static uint8_t copy_buffer[64 * 512], readback_buffer[64 * 512];
static FileState file_backup;
static bool backup_valid;
static const char *greetings[] = {"Olá.", "Hello.", "Hola.", "Bonjour.", "Hallo.", "Ciao."};
static const char *countries[] = {"Brasil",      "Portugal", "United States", "United Kingdom",
                                  "Deutschland", "France",   "India",         "Japan"};
static const char *country_codes[] = {"BR", "PT", "US", "GB", "DE", "FR", "IN", "JP"};
static const char *zones[] = {"America/Sao_Paulo", "Europe/Lisbon", "America/New_York",
                              "Europe/London",     "Europe/Berlin", "Europe/Paris",
                              "Asia/Kolkata",      "Asia/Tokyo"};
static const char *tr(const char *pt, const char *en) {
    return wizard_language ? pt : en;
}
static bool hit(int x, int y, int a, int b, int w, int h) {
    return x >= a && y >= b && x < a + w && y < b + h;
}
static void secret_clear(char *s, size_t n) {
    volatile char *p = s;
    while (n--)
        *p++ = 0;
}
static void center(int y, const char *s, int size, uint32_t color) {
    text((GFX_BOOT_W - text_width(s, size)) / 2, y, s, size, color);
}
static void button(int x, int y, int w, const char *s, bool enabled, bool danger) {
    round_rect(x, y, w, 38, 13, enabled ? (danger ? RED : BLUE) : 0x8090a3, enabled ? 255 : 70);
    text(x + (w - text_width(s, 13)) / 2, y + 10, s, 13, enabled ? 0xffffff : SECONDARY);
}
static void heading(const char *pt, const char *en, const char *subpt, const char *suben) {
    text(158, 190, tr(pt, en), 28, INK);
    text(160, 236, tr(subpt, suben), 13, SECONDARY);
}
static void entry(int y, const char *label, const char *value, int field, bool secret, int width) {
    text(160, y - 23, label, 13, SECONDARY);
    round_rect(158, y, width, 42, 12, 0xffffff, 225);
    if (field_index == field)
        round_rect(158, y + 39, width, 3, 1, BLUE, 255);
    char dots[65];
    const char *shown = value;
    if (secret) {
        unsigned n = strlen(value);
        memset(dots, '*', n);
        dots[n] = 0;
        shown = dots;
    }
    text(173, y + 11, shown, 16, INK);
    if (field_index == field)
        rect(174 + text_width(shown, 16), y + 10, 1, 21, BLUE);
}
static void row(int x, int y, int w, const char *title, const char *detail, bool selected,
                bool enabled) {
    round_rect(x, y, w, 48, 12, selected ? BLUE : 0xffffff, selected ? 255 : 160);
    text(x + 15, y + 7, title, 16, selected ? 0xffffff : enabled ? INK : 0x8796a6);
    text(x + 15, y + 29, detail, 13, selected ? 0xdbeaff : SECONDARY);
}
static void paragraph(int y, const char *s, uint32_t color) {
    char part[160];
    unsigned n = 0;
    for (;; s++) {
        if (!*s || *s == '\n' || n == 94) {
            part[n] = 0;
            text(160, y, part, 13, color);
            y += 23;
            n = 0;
            if (!*s)
                break;
            if (*s == '\n')
                continue;
        }
        part[n++] = *s;
    }
}
static void change_page(int page) {
    if (setup_page == 3 && page != 3)
        secret_clear(wifi_password, sizeof(wifi_password));
    setup_page = page;
    if (page == 3) {
        wifi_count = wifi_first = 0;
        wifi_selected = -1;
        wifi_password[0] = 0;
        wifi_connect_rc = 0;
        wifi_scan_rc = wifi_scan_start();
        wifi_state_rc = wifi_state(&wifi_link);
        wifi_last_refresh = 0;
    }
    page_started = frame_clock;
    message[0] = 0;
    field_index = 0;
    ui_dirty = true;
}
static void setup_wifi_connect(void) {
    if (wifi_selected < 0 || (unsigned)wifi_selected >= wifi_count)
        return;
    wifi_connect_rc = wifi_connect_ap(&wifi_aps[wifi_selected], wifi_password);
    secret_clear(wifi_password, sizeof(wifi_password));
    ui_dirty = true;
}
static unsigned storage_entry(unsigned row) {
    unsigned offset = storage_page * 4u + row;
    return offset < storage_visible_count ? storage_visible[offset] : STORAGE_DEVICE_COUNT;
}
static unsigned storage_page_for(unsigned index) {
    for (unsigned i = 0; i < storage_visible_count; i++)
        if (storage_visible[i] == index) return i / 4u;
    return 0;
}
static unsigned storage_pages(void) {
    return storage_visible_count ? (storage_visible_count + 3u) / 4u : 1u;
}
static bool source_valid(void) {
    uint8_t header[512];
    uint32_t version, bytes, sector;
    if (!disk_read(LUNA_IMAGE_INFO_SECTOR, header) || memcmp(header, "LUNAIMG1", 8))
        return false;
    memcpy(&version, header + 8, 4);
    memcpy(&bytes, header + 12, 4);
    memcpy(&source_bytes, header + 16, 4);
    memcpy(&sector, header + 20, 4);
    if (version != 1 || bytes != LUNA_IMAGE_BYTES || sector != LUNA_SOURCES_SECTOR ||
        source_bytes > LUNA_IMAGE_BYTES - (LUNA_SOURCES_SECTOR + 1) * 512)
        return false;
    return disk_read(LUNA_SOURCES_SECTOR, header) && !memcmp(header, "LUNASRC1", 8);
}
static void enumerate_targets(void) {
    storage_visible_count = 0;
    uint32_t source_id = 0;
    if (disk_read(0, transfer))
        memcpy(&source_id, transfer + 440, 4);
    for (unsigned i = 0; i < STORAGE_DEVICE_COUNT; i++) {
        const DiskInfo *d = i==4 ? 0 : disk_info(i);
        if (d && d->present) storage_visible[storage_visible_count++] = i;
        eligible[i] = d && d->present && d->supported && !d->optical &&
                      i != disk_boot_index() && d->sectors <= UINT32_MAX &&
                      d->sectors >= LUNA_IMAGE_BYTES / 512;
        if (eligible[i] && disk_read_on(i, 0, transfer)) {
            uint32_t id;
            memcpy(&id, transfer + 440, 4);
            if (source_id && source_id == id && transfer[510] == 0x55 && transfer[511] == 0xaa)
                eligible[i] = false;
        }
    }
    storage_page = 0;
    /* Reveal a usable destination without preselecting a destructive target. */
    for (unsigned i = 0; i < storage_visible_count; i++)
        if (eligible[storage_visible[i]]) {
            storage_page = i / 4u;
            break;
        }
    if (selected_disk >= 0 && !eligible[selected_disk]) selected_disk = -1;
}
void setup_init(void) {
    secret_clear(password, sizeof(password));
    secret_clear(confirmation, sizeof(confirmation));
    selected_disk = -1;
    storage_page = 0;
    selected_region = 0;
    previous_buttons = 0;
    detected = false;
    hashing = false;
    login_busy = false;
    username[0] = erase_text[0] = layout_test[0] = 0;
    File *done = fs_find("/etc/setup-complete");
    installed = done && !done->dir && done->size < 128 && fs_materialize(done) &&
                !strcmp(done->data, "LunaOS setup v1\n");
    setup_active = true;
    if (installed) {
        if (!account_load()) {
            change_page(11);
            strcpy(message, "Invalid account configuration. Boot recovery media.");
        } else {
            wizard_language = prefs.language == 1;
            change_page(10);
        }
    } else {
        account_load();
        change_page(0);
    }
    source_valid();
}
void setup_live(void) {
    if (installed || setup_page >= 8)
        return;
    secret_clear(password, sizeof(password));
    secret_clear(confirmation, sizeof(confirmation));
    account_load();
    setup_active = false;
    gui_open(APP_HOME);
    gui_damage_all();
    serial_write("SETUP: live desktop selected (installation not completed)\n");
}
static void fail(const char *s) {
    if (backup_valid) {
        fs_restore_state(&file_backup);
        backup_valid = false;
    }
    secret_clear(password, sizeof(password));
    secret_clear(confirmation, sizeof(confirmation));
    account_clear_secret();
    hashing = false;
    login_busy = false;
    change_page(11);
    strcpy(message, s);
    serial_write("SETUP: installation failed; no automatic retry\n");
}
static bool valid_identity(void) {
    if (!*username || username[0] < 'a' || username[0] > 'z')
        return false;
    for (const char *p = username; *p; p++)
        if ((*p < 'a' || *p > 'z') && (*p < '0' || *p > '9') && *p != '_' && *p != '-')
            return false;
    return strlen(password) >= 8 && !strcmp(password, confirmation);
}
static void erase_token(char *out) {
    strcpy(out, tr("apagar ", "erase "));
    char n[8];
    number(n, selected_disk);
    strcat(out, n);
}
static bool confirmed(void) {
    if (selected_disk < 0 || (unsigned)selected_disk >= STORAGE_DEVICE_COUNT || !eligible[selected_disk])
        return false;
    char token[32];
    erase_token(token);
    return !strcmp(erase_text, token);
}
static void start_install(void) {
    if (!confirmed() || !valid_identity() || !source_valid()) {
        strcpy(message, tr("Dados inválidos ou mídia de instalação incompleta.",
                           "Invalid data or incomplete installation media."));
        ui_dirty = true;
        return;
    }
    if (!account_begin(username, password, false)) {
        strcpy(message, tr("Não foi possível criar a senha: entropia indisponível.",
                           "Cannot create password: entropy unavailable."));
        ui_dirty = true;
        return;
    }
    secret_clear(password, sizeof(password));
    secret_clear(confirmation, sizeof(confirmation));
    hashing = true;
    install_phase = 0;
    install_sector = 1;
    change_page(8);
}
static void advance(void) {
    if (setup_page == 0)
        change_page(1);
    else if (setup_page == 1) {
        wizard_language = prefs.language == 1;
        change_page(2);
        detected = false;
    } else if (setup_page == 2 && detected)
        change_page(3);
    else if (setup_page == 3)
        change_page(4);
    else if (setup_page == 4)
        change_page(5);
    else if (setup_page == 5) {
        if (!valid_identity()) {
            strcpy(
                message,
                tr("Usuário: a-z, 0-9, _ ou -. Senha: 8+ caracteres e confirmação igual.",
                   "Username: a-z, 0-9, _ or -. Password: 8+ characters, matching confirmation."));
            ui_dirty = true;
        } else {
            enumerate_targets();
            change_page(6);
        }
    } else if (setup_page == 6) {
        if (selected_disk < 0 || !eligible[selected_disk]) {
            strcpy(message, tr("Selecione um disco de destino disponível.",
                               "Select an available destination disk."));
            ui_dirty = true;
        } else {
            erase_text[0] = 0;
            change_page(7);
        }
    } else if (setup_page == 7)
        start_install();
    else if (setup_page == 9)
        reboot();
    else if (setup_page == 10 && !login_busy) {
        if (account_begin(account_name(), password, true)) {
            secret_clear(password, sizeof(password));
            login_busy = true;
            strcpy(message, tr("Verificando senha...", "Verifying password..."));
            ui_dirty = true;
        } else {
            strcpy(message, tr("Digite sua senha (mínimo 8 caracteres).",
                               "Enter your password (at least 8 characters)."));
            ui_dirty = true;
        }
    }
}
void setup_draw(void) {
    gfx_wallpaper();
    gfx_unclip();
    gfx_occlusion_clear();
    gfx_set_origin((W - GFX_BOOT_W) / 2, (H - GFX_BOOT_H) / 2);
    round_rect(110, 90, 804, 582, 28, 0xf5f8fe, 240);
    icon(144, 119, ICON_MOON, 28, BLUE);
    text(181, 122, "LunaOS", 20, INK);
    if (setup_page <= 7)
        for (int i = 0; i < 8; i++)
            circle(744 + i * 17, 135, i == setup_page ? 4 : 3, i == setup_page ? BLUE : 0xc4cfdd);
    if (setup_page == 0) {
        icon(474, 220, ICON_MOON, 76, BLUE);
        center(331, greetings[greeting], 48, INK);
        center(408, tr("Seu espaço começa aqui.", "Your space starts here."), 20, SECONDARY);
        center(451, "Bem-vindo / Welcome / Bienvenido / Bienvenue", 13, SECONDARY);
        button(158, 606, 192, tr("Experimentar LunaOS", "Try LunaOS"), true, false);
    } else if (setup_page == 1) {
        heading("Vamos falar a mesma língua", "Let's speak your language",
                "Idioma do sistema e disposição do teclado.",
                "System language and keyboard layout.");
        row(158, 284, 344, "English", "System + setup", prefs.language == 0, true);
        row(522, 284, 344, "Português (Brasil)", "Sistema + assistente (tradução parcial)",
            prefs.language == 1, true);
        text(160, 357, tr("Teclado", "Keyboard"), 16, INK);
        row(158, 389, 344, "English (US)", "QWERTY", prefs.keyboard_layout == 0, true);
        row(522, 389, 344, "Português (ABNT2)", tr("Mapeamento básico", "Basic mapping"),
            prefs.keyboard_layout == 1, true);
        entry(482, tr("Teste seu teclado", "Test your keyboard"), layout_test, 0, false, 708);
    } else if (setup_page == 2) {
        heading("Conhecendo seu hardware", "Discovering your hardware",
                "Identificação PCI e estado do suporte de rede.",
                "PCI identification and network support status.");
        icon(160, 284, ICON_WIFI, 34, BLUE);
        paragraph(337,
                  detected ? radio : tr("Identificando adaptadores...", "Identifying adapters..."),
                  INK);
        paragraph(493,
                  source_bytes
                      ? tr("Fontes Wi-Fi Linux incluídos na mídia.\nFontes não são drivers "
                           "executáveis. RTL8822BE: busca passiva experimental.",
                           "Linux Wi-Fi sources included on this media.\nSources are not "
                           "executable drivers. RTL8822BE: experimental passive scan.")
                      : tr("Pacote de fontes indisponível.", "Source archive unavailable."),
                  SECONDARY);
    } else if (setup_page == 3) {
        heading("Conecte-se ao seu mundo", "Connect to your world",
                "Selecione uma rede Wi-Fi real ou continue offline.",
                "Select a real Wi-Fi network or continue offline.");
        row(158, 287, 708, "Ethernet / RTL8139",
            net_ready ? tr("Perfil IPv4 estático (QEMU)", "Static IPv4 profile (QEMU)") :
                        tr("Sem Ethernet compatível", "No compatible Ethernet"), false, net_ready);
        text(160, 359, tr("Redes Wi-Fi", "Wi-Fi networks"), 16, INK);
        if (wifi_count > 2) {
            button(460, 350, 84, tr("Acima", "Prev"), wifi_first > 0, false);
            button(554, 350, 84, tr("Abaixo", "Next"), wifi_first + 2 < wifi_count, false);
        }
        button(650, 350, 216, tr("Buscar redes", "Scan networks"), true, false);
        for (unsigned i = 0; i < 2 && wifi_first + i < wifi_count; i++) {
            const WifiAp *ap = &wifi_aps[wifi_first + i];
            const char *security = ap->security == WIFI_OPEN ? "Open" :
                                   ap->security == WIFI_WPA2_PSK ? "WPA2-PSK" :
                                   "Security unsupported";
            char detail[96] = {0};
            append(detail, wifi_channel_band(ap->channel) == WIFI_BAND_2GHZ ?
                   "2.4 GHz | " : "5 GHz | ", sizeof(detail));
            append(detail, security, sizeof(detail));
            row(158, 396 + (int)i * 55, 708, ap->ssid, detail,
                wifi_selected == (int)(wifi_first + i), ap->security != WIFI_OTHER);
        }
        if (wifi_state_rc == 0 && wifi_link.link == WIFI_ONLINE) {
            text(160, 518, tr("Wi-Fi conectado e IP configurado.", "Wi-Fi connected; IP configured."),
                 13, 0x237746);
        } else if (wifi_state_rc == 0 && wifi_link.link == WIFI_ASSOCIATING) {
            text(160, 518, tr("Autenticando com o ponto de acesso...",
                              "Authenticating with access point..."), 13, INK);
        } else if (wifi_state_rc == 0 && wifi_link.link == WIFI_ASSOCIATED) {
            text(160, 518, tr("Associado. Aguardando configuração IP...",
                              "Associated. Awaiting IP configuration..."), 13, INK);
        } else if (wifi_connect_rc || (wifi_state_rc == 0 && wifi_link.error)) {
            int error = wifi_connect_rc ? wifi_connect_rc : wifi_link.error;
            const char *reason = error == -110 ?
                tr("Tempo esgotado: sem resposta do roteador.", "Timeout: no access point response.") :
                error == -16 ?
                tr("Fila de transmissão ocupada. Tente novamente.", "TX queue busy. Retry.") :
                error == -22 ?
                tr("Parâmetros WPA2/RSN inválidos.", "Invalid WPA2/RSN parameters.") :
                error == -95 ?
                tr("Segurança da rede não suportada.", "Unsupported network security.") :
                tr("Conexão falhou. Consulte o wifidiag.", "Connection failed. Check wifidiag.");
            text(160, 518, reason, 13, RED);
        } else if (wifi_scan_rc) {
            text(160, 518, tr("Driver Wi-Fi não operacional. Não existem redes simuladas.",
                              "Wi-Fi driver unavailable; no fabricated networks."), 13, RED);
        } else if (wifi_state_rc == 0 && !wifi_link.can_connect) {
            text(160, 518, tr("Busca passiva experimental. Conexão ainda indisponível.",
                              "Experimental passive scan. Connection unavailable."), 13, SECONDARY);
        } else if (!wifi_count) {
            text(160, 518, tr("Aguardando resultados reais de varredura...",
                              "Waiting for actual scan results..."), 13, SECONDARY);
        } else {
            text(160, 518, tr("Selecione uma rede para conectar.",
                              "Select an access point to connect."), 13, SECONDARY);
        }
        if (wifi_selected >= 0 && (unsigned)wifi_selected < wifi_count) {
            const WifiAp *chosen = &wifi_aps[wifi_selected];
            if (chosen->security == WIFI_WPA2_PSK)
                entry(557, tr("Senha Wi-Fi", "Wi-Fi password"), wifi_password, 0, true, 475);
            else
                text(160, 565, tr("Esta rede não exige senha.", "This network requires no password."),
                     13, SECONDARY);
            button(650, 558, 216, tr("Conectar", "Connect"),
                   chosen->security != WIFI_OTHER && wifi_link.can_connect, false);
        }
    } else if (setup_page == 4) {
        heading("Sua região", "Your region", "Selecione o país e a zona de horário preferida.",
                "Select your country and preferred timezone.");
        for (int i = 0; i < 8; i++)
            row(158 + (i % 2) * 364, 282 + (i / 2) * 62, 344, countries[i], zones[i],
                selected_region == i, true);
    } else if (setup_page == 5) {
        heading("Um espaço só seu", "Make it yours",
                "Crie o usuário padrão e uma senha para entrar no LunaOS.",
                "Create the default user and a LunaOS sign-in password.");
        entry(295, tr("Nome de usuário", "Username"), username, 0, false, 708);
        entry(388, tr("Senha (8 a 64 caracteres)", "Password (8 to 64 characters)"), password, 1,
              true, 344);
        /* Draw confirmation separately in the right column. */
        round_rect(522, 388, 344, 42, 12, 0xffffff, 225);
        text(524, 365, tr("Confirme a senha", "Confirm password"), 13, SECONDARY);
        char dots[65];
        unsigned n = strlen(confirmation);
        memset(dots, '*', n);
        dots[n] = 0;
        text(537, 399, dots, 16, INK);
        if (field_index == 2) {
            round_rect(522, 427, 344, 3, 1, BLUE, 255);
            rect(538 + text_width(dots, 16), 398, 1, 21, BLUE);
        }
        paragraph(466,
                  tr("Você usará esta senha para entrar no LunaOS.\nEla protege a "
                     "entrada da sessão; não cifra o disco.",
                     "Use this password to sign in to LunaOS.\nIt protects session "
                     "sign-in; it does not encrypt the disk."),
                  SECONDARY);
    } else if (setup_page == 6) {
        heading("Onde o LunaOS vai morar?", "Where will LunaOS live?",
                "Selecione o disco inteiro. As partições existentes serão removidas.",
                "Select the whole disk. Existing partitions will be removed.");
        for (unsigned row_index = 0; row_index < 4; row_index++) {
            unsigned i=storage_entry(row_index);
            if (i >= STORAGE_DEVICE_COUNT) break;
            const DiskInfo *d=disk_info(i);
            char name[96],n[20],detail[128];
            strcpy(name,storage_disk_type(i));
            strcat(name," ");
            number(n,i);strcat(name,n);strcat(name,"  ");
            strcat(name,d && d->present ? d->model :
                  tr("Não detectado","Not detected"));
            if(!d || !d->present)strcpy(detail,"--");
            else {
                uint64_t mib=d->sectors/2048u;
                if(mib>UINT32_MAX)strcpy(n,">4 TiB");
                else number(n,(uint32_t)mib);
                strcpy(detail,n);strcat(detail," MiB / ");
                strcat(detail,i==disk_boot_index() ?
                       tr("mídia de boot (protegida)","boot media (protected)") :
                       eligible[i] ?
                       tr("disponível para instalação","available for install") :
                       !d->supported ?
                       tr("driver/formato não suportado","unsupported driver/format") :
                       tr("origem ou capacidade incompatível","source/capacity unsupported"));
            }
            row(158,279+row_index*65,708,name,detail,
                selected_disk==(int)i,eligible[i]);
        }
        bool available = false;
        for (unsigned i = 0; i < STORAGE_DEVICE_COUNT; i++)
            if (eligible[i]) available = true;
        if (!available && storage_visible_count < 4) {
            paragraph(279 + (storage_visible_count < 3 ? storage_visible_count : 3) * 65,
                      tr("Nenhum destino disponível. No modo live, execute storage\n"
                         "no Terminal para ver o diagnóstico dos controladores.",
                         "No destination available. In live mode, run storage\n"
                         "in Terminal to inspect controller diagnostics."), SECONDARY);
        }
        char page_info[80],n[20];
        strcpy(page_info,tr("Discos - página ","Disks - page "));
        number(n,storage_page+1);strcat(page_info,n);
        strcat(page_info,"/");number(n,storage_pages());strcat(page_info,n);
        strcat(page_info,tr("  (< / > para trocar)","  (< / > to switch)"));
        text(160,552,page_info,13,SECONDARY);
    } else if (setup_page == 7) {
        heading("Pronto para instalar?", "Ready to install?",
                "Confira o destino. Esta operação apaga as partições desse disco.",
                "Review the target. This operation erases its existing partitions.");
        char line[128], n[8];
        strcpy(line, storage_disk_type(selected_disk));
        strcat(line, " ");
        number(n, selected_disk);
        strcat(line, n);
        strcat(line, " / ");
        strcat(line, disk_info(selected_disk)->model);
        char detail[128] = "/home/";
        strcat(detail, username);
        strcat(detail, " / ");
        strcat(detail, countries[selected_region]);
        row(158, 286, 708, line, detail, true, true);
        paragraph(365,
                  tr("A mídia de origem e os outros discos serão preservados.\nO destino receberá "
                     "bootloader, kernel, LunaFS e configurações.",
                     "Installation media and other disks will be preserved.\nTarget receives "
                     "bootloader, kernel, LunaFS and settings."),
                  SECONDARY);
        char token[32], prompt[128];
        erase_token(token);
        strcpy(prompt, tr("Para confirmar, digite: ", "To confirm, type: "));
        strcat(prompt, token);
        entry(462, prompt, erase_text, 0, false, 708);
    } else if (setup_page == 8) {
        heading("Criando seu LunaOS", "Creating your LunaOS",
                "Mantenha o computador ligado durante a instalação.",
                "Keep the computer powered on during installation.");
        const char *phase =
            hashing ? tr("Protegendo sua senha...", "Hashing your password...")
            : install_phase == 1
                ? tr("Copiando e verificando o sistema...", "Copying and verifying the system...")
                : tr("Salvando e verificando configurações...", "Saving and verifying settings...");
        center(336, phase, 20, INK);
        unsigned progress =
            hashing ? account_progress() / 10 : 10 + install_sector * 85 / (LUNA_IMAGE_BYTES / 512);
        round_rect(205, 401, 614, 9, 4, 0xd0dbea, 255);
        round_rect(205, 401, progress * 614 / 100, 9, 4, BLUE, 255);
        char n[24];
        number(n, progress);
        strcat(n, "%");
        center(437, n, 20, SECONDARY);
    } else if (setup_page == 9) {
        heading("Tudo pronto.", "You're all set.",
                "O LunaOS foi instalado e verificado no disco selecionado.",
                "LunaOS is installed and verified on the selected disk.");
        icon(474, 300, ICON_MOON, 76, BLUE);
        paragraph(435,
                  tr("Retire a mídia de instalação ou ajuste a ordem de boot.\nO computador vai "
                     "reiniciar automaticamente em alguns segundos.",
                     "Remove installation media or adjust the boot order.\nThe computer will "
                     "restart automatically in a few seconds."),
                  SECONDARY);
    } else if (setup_page == 10) {
        heading("Bom te ver de novo.", "Welcome back.", "Digite sua senha para abrir sua sessão.",
                "Enter your password to open your session.");
        center(305, account_name(), 28, INK);
        entry(402, tr("Senha", "Password"), password, 0, true, 708);
    } else {
        heading("Não foi possível continuar", "Unable to continue",
                "O assistente parou sem tentar formatar outro disco.",
                "Setup stopped without attempting to format another disk.");
        paragraph(310, message, RED);
        paragraph(458,
                  tr("Reinicie pela mídia de instalação para tentar novamente.\nDiscos já "
                     "formatados não são restaurados automaticamente.",
                     "Restart from installation media to retry.\nAlready formatted disks are not "
                     "restored automatically."),
                  SECONDARY);
    }
    if (*message && setup_page != 11)
        paragraph(555, message, RED);
    if (setup_page > 0 && setup_page < 8)
        button(158, 606, 108, tr("Voltar", "Back"), true, false);
    if (setup_page < 8 || setup_page == 9 || setup_page == 10) {
        const char *label = setup_page == 7    ? tr("Apagar e instalar", "Erase and install")
                            : setup_page == 3  ? (wifi_state_rc == 0 && wifi_link.link == WIFI_ONLINE ?
                                                        tr("Continuar", "Continue") :
                                                        tr("Continuar offline", "Continue offline"))
                            : setup_page == 9  ? tr("Reiniciar agora", "Restart now")
                            : setup_page == 10 ? tr("Entrar", "Sign in")
                                               : tr("Continuar", "Continue");
        bool enabled =
            (setup_page != 2 || detected) && (setup_page != 7 || confirmed()) && !login_busy;
        button(650, 606, 216, label, enabled, setup_page == 7);
    }
    gfx_set_origin(0, 0);
    gfx_present();
    ui_dirty = false;
}
void setup_pointer(int x, int y, int buttons) {
    x -= (W - GFX_BOOT_W) / 2;
    y -= (H - GFX_BOOT_H) / 2;
    bool pressed = (buttons & 1) && !(previous_buttons & 1);
    previous_buttons = buttons;
    if (!pressed || setup_page == 8 || setup_page == 11 || login_busy)
        return;
    if (hit(x, y, 650, 606, 216, 38)) {
        advance();
        return;
    }
    if (setup_page == 0 && hit(x, y, 158, 606, 192, 38)) {
        setup_live();
        return;
    }
    if (setup_page > 0 && setup_page < 8 && hit(x, y, 158, 606, 108, 38)) {
        change_page(setup_page - 1);
        return;
    }
    if (setup_page == 1) {
        if (hit(x, y, 158, 284, 708, 48)) {
            prefs.language = x >= 522;
            wizard_language = prefs.language;
        }
        if (hit(x, y, 158, 389, 708, 48)) {
            prefs.keyboard_layout = x >= 522;
            input_apply_settings();
        }
    }
    if (setup_page == 3) {
        if (hit(x, y, 650, 350, 216, 38)) {
            wifi_scan_rc = wifi_scan_start();
            wifi_count = wifi_first = 0;
            wifi_selected = -1;
            wifi_connect_rc = 0;
            secret_clear(wifi_password, sizeof(wifi_password));
        } else if (wifi_count > 2 && hit(x, y, 460, 350, 84, 38)) {
            if (wifi_first) wifi_first--;
        } else if (wifi_count > 2 && hit(x, y, 554, 350, 84, 38)) {
            if (wifi_first + 2 < wifi_count) wifi_first++;
        } else {
            for (unsigned i = 0; i < 2 && wifi_first + i < wifi_count; i++) {
                if (hit(x, y, 158, 396 + (int)i * 55, 708, 48)) {
                    wifi_selected = (int)(wifi_first + i);
                    secret_clear(wifi_password, sizeof(wifi_password));
                    wifi_connect_rc = 0;
                }
            }
            if (wifi_selected >= 0 &&
                hit(x, y, 650, 558, 216, 38))
                setup_wifi_connect();
        }
        ui_dirty = true;
        return;
    }
    if (setup_page == 4)
        for (int i = 0; i < 8; i++)
            if (hit(x, y, 158 + (i % 2) * 364, 282 + (i / 2) * 62, 344, 48))
                selected_region = i;
    if (setup_page == 5) {
        if (y >= 295 && y < 337)
            field_index = 0;
        if (y >= 388 && y < 430)
            field_index = x >= 522 ? 2 : 1;
    }
    if (setup_page == 6) {
        if(hit(x,y,158,546,708,30)) {
            if(x<450 && storage_page)storage_page--;
            else if(x>=450 && storage_page+1<storage_pages())storage_page++;
        }
        for (unsigned row_index = 0; row_index < 4; row_index++) {
            unsigned i=storage_entry(row_index);
            if (i<STORAGE_DEVICE_COUNT &&
                hit(x, y, 158, 279 + row_index * 65, 708, 48) && eligible[i])
                selected_disk = i;
        }
    }
    ui_dirty = true;
}
void setup_key(int key) {
    int c = key & 0xfff;
    if (setup_page == 8 || setup_page == 9 || setup_page == 11 || login_busy)
        return;
    if (c == 27 && setup_page == 0) {
        setup_live();
        return;
    }
    if (c == 10) {
        if (setup_page == 3 && wifi_selected >= 0)
            setup_wifi_connect();
        else
            advance();
        return;
    }
    if (c == 9) {
        if (setup_page == 5)
            field_index = (field_index + ((key & MOD_SHIFT) ? 2 : 1)) % 3;
        ui_dirty = true;
        return;
    }
    if (setup_page == 4 && (c == KEY_DOWN || c == KEY_UP)) {
        selected_region = (selected_region + (c == KEY_UP ? 7 : 1)) % 8;
        ui_dirty = true;
        return;
    }
    if (setup_page == 6 && (c == KEY_LEFT || c == KEY_RIGHT)) {
        if(c==KEY_LEFT && storage_page)storage_page--;
        if(c==KEY_RIGHT && storage_page+1<storage_pages())storage_page++;
        ui_dirty=true;return;
    }
    if (setup_page == 6 && (c == KEY_DOWN || c == KEY_UP)) {
        int i = selected_disk < 0 ? (c==KEY_UP ? 0 : (int)STORAGE_DEVICE_COUNT-1)
                                  : selected_disk;
        for (unsigned n = 0; n < STORAGE_DEVICE_COUNT; n++) {
            i = (i + (c == KEY_UP ? (int)STORAGE_DEVICE_COUNT-1 : 1))
                % (int)STORAGE_DEVICE_COUNT;
            if (eligible[i]) {
                selected_disk = i;
                storage_page=storage_page_for((unsigned)i);
                break;
            }
        }
        ui_dirty = true;
        return;
    }
    if (setup_page == 3 && (c == KEY_UP || c == KEY_DOWN)) {
        if (wifi_count) {
            if (wifi_selected < 0)
                wifi_selected = 0;
            else if (c == KEY_DOWN && (unsigned)wifi_selected + 1 < wifi_count)
                wifi_selected++;
            else if (c == KEY_UP && wifi_selected > 0)
                wifi_selected--;
            if ((unsigned)wifi_selected < wifi_first)
                wifi_first = (unsigned)wifi_selected;
            if ((unsigned)wifi_selected >= wifi_first + 2)
                wifi_first = (unsigned)wifi_selected - 1;
            secret_clear(wifi_password, sizeof(wifi_password));
        }
        ui_dirty = true;
        return;
    }
    char *value = 0;
    unsigned max = 0;
    if (setup_page == 1) {
        value = layout_test;
        max = 63;
    }
    if (setup_page == 3 && wifi_selected >= 0 &&
        wifi_aps[wifi_selected].security == WIFI_WPA2_PSK) {
        value = wifi_password;
        max = WIFI_MAX_PASSWORD;
    }
    if (setup_page == 5) {
        value = field_index == 0 ? username : field_index == 1 ? password : confirmation;
        max = field_index == 0 ? 24 : 64;
    }
    if (setup_page == 7) {
        value = erase_text;
        max = 31;
    }
    if (setup_page == 10) {
        value = password;
        max = 64;
    }
    if (value) {
        unsigned n = strlen(value);
        if (c == 8 && n)
            value[n - 1] = 0;
        else if (c >= 32 && c < 127 && n < max) {
            value[n] = c;
            value[n + 1] = 0;
        }
        ui_dirty = true;
    }
}
void setup_poll(void) {
    if (!setup_active)
        return;
    if (setup_page == 0 && frame_clock - greeting_clock >= 1800) {
        greeting_clock = frame_clock;
        greeting = (greeting + 1) % 6;
        ui_dirty = true;
    }
    if (setup_page == 2 && !detected && frame_clock - page_started >= 500) {
        wireless_status(radio, sizeof(radio));
        detected = true;
        ui_dirty = true;
    }
    if (setup_page == 3 && frame_clock - wifi_last_refresh >= 350) {
        wifi_last_refresh = frame_clock;
        WifiState link = {0};
        int rc = wifi_state(&link);
        if (rc != wifi_state_rc || (rc == 0 && (link.link != wifi_link.link ||
                                                link.error != wifi_link.error))) {
            wifi_state_rc = rc;
            wifi_link = link;
            ui_dirty = true;
        }
        if (!wifi_scan_rc) {
            WifiAp results[WIFI_MAX_APS] = {0};
            unsigned count = 0;
            if (!wifi_scan_results(results, WIFI_MAX_APS, &count) &&
                (count != wifi_count || memcmp(wifi_aps, results, sizeof(results)))) {
                memcpy(wifi_aps, results, sizeof(results));
                wifi_count = count;
                wifi_first = 0;
                wifi_selected = -1;
                secret_clear(wifi_password, sizeof(wifi_password));
                ui_dirty = true;
            }
        }
    }
    if (setup_page == 10 && login_busy && account_poll(1024)) {
        login_busy = false;
        if (account_success()) {
            setup_active = false;
            strcpy(cwd, account_home());
            gui_open(APP_HOME);
            gui_damage_all();
            serial_write("LOGIN: session opened\n");
        } else {
            strcpy(message, tr("Senha incorreta.", "Incorrect password."));
            secret_clear(password, sizeof(password));
            ui_dirty = true;
        }
    }
    if (setup_page == 9 && (int32_t)(frame_clock - reboot_at) >= 0) {
        reboot();
        return;
    }
    if (setup_page != 8)
        return;
    if (hashing) {
        if (!account_poll(1024)) {
            ui_dirty = true;
            return;
        }
        hashing = false;
        if (!account_success() || selected_disk < 0 || !eligible[selected_disk] ||
            !disk_read(0, boot_sector)) {
            fail("Unable to prepare installation.");
            return;
        }
        memset(transfer, 0, 512);
        if (!disk_write_on(selected_disk, 0, transfer) || !disk_flush(selected_disk)) {
            fail("Unable to invalidate destination boot sector.");
            return;
        }
        const DiskInfo *d = disk_info(selected_disk);
        for (unsigned i = 0; i < 33; i++)
            if (!disk_write_on(selected_disk, (uint32_t)d->sectors - 1 - i, transfer)) {
                fail("Unable to clear old partition metadata.");
                return;
            }
        install_phase = 1;
    }
    if (install_phase == 1) {
        unsigned count = LUNA_IMAGE_BYTES / 512 - install_sector;
        if (count > 64)
            count = 64;
        if (count && (!disk_read_blocks(disk_boot_index(), install_sector, count, copy_buffer) ||
                      !disk_write_blocks(selected_disk, install_sector, count, copy_buffer) ||
                      !disk_read_blocks(selected_disk, install_sector, count, readback_buffer) ||
                      memcmp(copy_buffer, readback_buffer, count * 512))) {
            fail("Disk copy/read-back verification failed.");
            return;
        }
        install_sector += count;
        ui_dirty = true;
        if (install_sector < LUNA_IMAGE_BYTES / 512)
            return;
        install_phase = 2;
        return;
    }
    if (install_phase == 2) {
        memset(transfer, 0, 512);
        memcpy(transfer + 446, boot_sector + 446, 32);
        transfer[510] = 0x55;
        transfer[511] = 0xaa;
        if (!disk_write_on(selected_disk, 0, transfer) || !disk_flush(selected_disk)) {
            fail("Unable to prepare destination partitions.");
            return;
        }
        /* File data uses owned heap buffers; a shallow copy would dangle. */
        fs_discard_state(&file_backup);
        if (fs_clone(&file_backup, files)) {
            fail("Insufficient memory for filesystem backup.");
            return;
        }
        backup_valid = true;
        char region[256] = "country=";
        strcat(region, country_codes[selected_region]);
        strcat(region, "\ntimezone=");
        strcat(region, zones[selected_region]);
        strcat(region, "\nclock=UTC\n");
        if (!account_store() || !settings_write() || fs_write("/etc/region", region, false) ||
            fs_write("/etc/setup-complete", "LunaOS setup v1\n", false) ||
            !fs_save_to(selected_disk) || !fs_verify_on(selected_disk)) {
            fail("Unable to save/verify destination configuration.");
            return;
        }
        uint32_t disk_id;
        if (!random_bytes(&disk_id, 4)) {
            fail("No entropy for disk identity.");
            return;
        }
        if (!disk_id || disk_id == 0x4c554e41)
            disk_id ^= 0x6937a2f1;
        memset(boot_sector + 432, 0, 4); /* Installed HDD must not retain CD handoff metadata. */
        memset(boot_sector + 444, 0, 2);
        memcpy(boot_sector + 440, &disk_id, 4);
        if (!disk_read_on(selected_disk, 0, verify_sector)) {
            fail("Unable to read destination data partition.");
            return;
        }
        memcpy(boot_sector + 478, verify_sector + 478, 32);
        memset(boot_sector + 446, 0, 32);
        uint8_t *p = boot_sector + 446;
        p[0] = 0x80;
        p[1] = p[5] = 0xfe;
        p[2] = p[3] = p[6] = p[7] = 0xff;
        p[4] = 0xda;
        uint32_t start = LUNA_VOLUME_START, count = LUNA_ESP_SECTOR - start;
        memcpy(p + 8, &start, 4);
        memcpy(p + 12, &count, 4);
        p += 16;
        p[4] = 0xef;
        p[1] = p[5] = 0xfe;
        p[2] = p[3] = p[6] = p[7] = 0xff;
        start = LUNA_ESP_SECTOR;
        count = LUNA_ESP_BYTES / 512;
        memcpy(p + 8, &start, 4);
        memcpy(p + 12, &count, 4);
        if (!disk_write_on(selected_disk, 0, boot_sector) || !disk_flush(selected_disk) ||
            !disk_read_on(selected_disk, 0, verify_sector) ||
            memcmp(boot_sector, verify_sector, 512)) {
            fail("Unable to commit verified boot sector.");
            return;
        }
        fs_discard_state(&file_backup);
        backup_valid = false;
        change_page(9);
        reboot_at = frame_clock + 7000;
        serial_write("INSTALL: verified destination committed; reboot pending\n");
    }
}
