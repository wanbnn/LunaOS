#include "calendar.h"
#include "clock.h"
#include "luna.h"
#include "settings.h"
#include "window.h"
static bool open;
static int year, month, editing = -1;
static char fields[4][128];
static char form_status[96];
static const char *months[] = {"January",   "February", "March",    "April",
                               "May",       "June",     "July",     "August",
                               "September", "October",  "November", "December"};
static const char *months_pt[] = {"Janeiro",  "Fevereiro", "Março",    "Abril",
                                  "Maio",     "Junho",     "Julho",    "Agosto",
                                  "Setembro", "Outubro",   "Novembro", "Dezembro"};
static bool inside(int x, int y, int a, int b, int w, int h) {
    return x >= a && x < a + w && y >= b && y < b + h;
}
static void btn(int x, int y, int w, const char *s) {
    round_rect(x, y, w, 32, 10, settings_accent(), 210);
    text(x + 10, y + 7, s, 13, 0xffffff);
}
void time_settings_load(void) {
    DateTime d;
    char b[20];
    if (!clock_local(&d))
        d = (DateTime){2026, 1, 1, 0, 0, 0};
    datetime_format(&d, b);
    memcpy(fields[0], b, 10);
    fields[0][10] = 0;
    strcpy(fields[1], b + 11);
    datetime_offset_format(clock_offset(), fields[2]);
    strcpy(fields[3], clock_server());
    editing = -1;
    form_status[0] = 0;
}
void time_settings_draw(int x, int y, int w) {
    DateTime d;
    char b[20], z[7];
    if (clock_local(&d)) {
        datetime_format(&d, b);
        text(x, y + 82, b, 20, 0xffffff);
    }
    datetime_offset_format(clock_offset(), z);
    text(x + 260, y + 87, "UTC", 13, 0xb9cee5);
    text(x + 292, y + 87, z, 13, 0xb9cee5);
    const char *labels[] = {"Date / AAAA-MM-DD", "Time / HH:MM:SS", "UTC offset / +/-HH:MM",
                            "NTP server"};
    const int yy[] = {145, 145, 215, 285}, xx[] = {0, 205, 0, 0}, ww[] = {190, 150, 190, 355};
    for (int i = 0; i < 4; i++) {
        text(x + xx[i], y + yy[i] - 22, labels[i], 13, 0xb9cee5);
        round_rect(x + xx[i], y + yy[i], ww[i], 32, 8, 0xffffff, i == editing ? 55 : 20);
        text(x + xx[i] + 8, y + yy[i] + 7, fields[i], 13, 0xffffff);
    }
    btn(x + 205, y + 215, 150, clock_auto() ? "Auto: ON" : "Auto: OFF");
    btn(x, y + 340, 170, system_text("Apply manual time", "Aplicar hora manual"));
    btn(x + 185, y + 340, 170, system_text("Sync now", "Sincronizar agora"));
    text(x, y + 390, form_status[0] ? form_status : clock_status(), 13, 0xb9cee5);
    text(x, y + 415,
         system_text("Fixed UTC offset; no automatic daylight saving.",
                     "Fuso UTC fixo; sem horário de verão automático."),
         13, 0xb9cee5);
    (void)w;
}
void time_settings_key(int key) {
    if (editing < 0)
        return;
    int c = key & 0xfff, n = strlen(fields[editing]);
    if (c == 8 && n)
        fields[editing][n - 1] = 0;
    else if (c == 27 || c == 10)
        editing = -1;
    else if (c >= 32 && c < 127 && n < (editing == 3 ? 100 : 19)) {
        fields[editing][n] = c;
        fields[editing][n + 1] = 0;
    }
    gui_invalidate_app(APP_SETTINGS);
}
void time_settings_click(int x, int y) {
    editing = -1;
    const int yy[] = {145, 145, 215, 285}, xx[] = {0, 205, 0, 0}, ww[] = {190, 150, 190, 355};
    for (int i = 0; i < 4; i++)
        if (inside(x, y, xx[i], yy[i], ww[i], 32))
            editing = i;
    if (inside(x, y, 205, 215, 150, 32)) {
        clock_automatic(!clock_auto());
        form_status[0] = 0;
    }
    if (inside(x, y, 0, 340, 170, 32)) {
        DateTime d;
        int z;
        if (datetime_parse(fields[0], fields[1], &d) && datetime_offset_parse(fields[2], &z)) {
            clock_zone(z);
            if (clock_set(&d))
                form_status[0] = 0;
            else
                strcpy(form_status, "Date outside supported UTC range");
        } else
            strcpy(form_status, "Invalid date, time or UTC offset");
    }
    if (inside(x, y, 185, 340, 170, 32)) {
        int z;
        if (datetime_offset_parse(fields[2], &z) && clock_endpoint(fields[3], clock_port())) {
            clock_zone(z);
            clock_sync();
            form_status[0] = 0;
        } else
            strcpy(form_status, "Invalid UTC offset or NTP server");
    }
    gui_invalidate_app(APP_SETTINGS);
}
void calendar_refresh(void) {
    extern void settings_clock_refresh(void);
    settings_clock_refresh();
    if (open)
        gui_damage(W - 376, 52, 364, 405);
}
void calendar_draw(void) {
    if (!open)
        return;
    int x = W - 372, y = 56;
    DateTime now;
    if (!clock_local(&now))
        now = (DateTime){2026, 1, 1, 0, 0, 0};
    glass(x, y, 354, 392, 18, 1);
    char n[24];
    number(n, year);
    text(x + 52, y + 20, system_text(months[month - 1], months_pt[month - 1]), 20, 0xffffff);
    text(x + 219, y + 23, n, 16, 0xffffff);
    btn(x + 12, y + 17, 30, "<");
    btn(x + 310, y + 17, 30, ">");
    const char *days[] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
    const char *pt[] = {"Se", "Te", "Qa", "Qi", "Se", "Sa", "Do"};
    for (int i = 0; i < 7; i++)
        text(x + 20 + i * 46, y + 76, system_text(days[i], pt[i]), 13, 0xb9cee5);
    DateTime first = {year, month, 1, 0, 0, 0};
    int start = datetime_weekday(&first), count = datetime_month_days(year, month);
    for (int d = 1; d <= count; d++) {
        int cell = start + d - 1, a = x + 12 + (cell % 7) * 46, b = y + 104 + (cell / 7) * 32;
        if (d == now.day && month == now.month && year == now.year)
            round_rect(a, b, 38, 29, 9, settings_accent(), 230);
        number(n, d);
        text(a + 9, b + 5, n, 13, 0xffffff);
    }
    btn(x + 12, y + 310, 95, system_text("Today", "Hoje"));
    btn(x + 117, y + 310, 223, system_text("Date & time settings", "Ajustar data e hora"));
    char stamp[20];
    datetime_format(&now, stamp);
    text(x + 16, y + 359, stamp, 13, 0xb9cee5);
}
bool calendar_click(int x, int y) {
    if (inside(x, y, W - 136, 8, 130, 32)) {
        open = !open;
        if (open) {
            DateTime d;
            if (!clock_local(&d))
                d = (DateTime){2026, 1, 1, 0, 0, 0};
            year = d.year;
            month = d.month;
        }
        calendar_refresh();
        gui_damage(W - 376, 52, 364, 405);
        return true;
    }
    if (!open)
        return false;
    int a = x - (W - 372), b = y - 56;
    if (inside(a, b, 12, 17, 30, 32) && !(year == 1970 && month == 1)) {
        if (--month == 0) {
            month = 12;
            year--;
        }
    } else if (inside(a, b, 310, 17, 30, 32) && !(year == 2199 && month == 12)) {
        if (++month == 13) {
            month = 1;
            year++;
        }
    } else if (inside(a, b, 12, 310, 95, 32)) {
        DateTime d;
        if (clock_local(&d)) {
            year = d.year;
            month = d.month;
        }
    } else if (inside(a, b, 117, 310, 223, 32)) {
        extern void settings_open_time(void);
        settings_open_time();
        gui_open(APP_SETTINGS);
        open = false;
    } else if (!inside(a, b, 0, 0, 354, 392)) {
        open = false;
        gui_damage(W - 376, 52, 364, 405);
        return false;
    }
    gui_damage(W - 376, 52, 364, 405);
    return true;
}
