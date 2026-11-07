#include "calendar.h"
#include "install.h"
#include "settings.h"
#include "web.h"
#include "window.h"
Window windows[APP_COUNT];
bool ui_dirty = true, controls_open;
int active_app = -1;
static int zorder[APP_COUNT], zcount, drag = -1, drag_dx, drag_dy, previous_buttons;
static uint32_t surfaces[APP_COUNT][GFX_CACHE_W * GFX_CACHE_H];
/* One cached window background. Repainting the glass costs a full per-pixel
 * pass over the window; keeping the last one turns a content update into a
 * memcpy plus the client drawing. */
static uint32_t glass_cache[GFX_CACHE_W * GFX_CACHE_H];
static bool glass_valid;
static int glass_app, glass_x, glass_y, glass_w, glass_h;
/* One large cache is shared by oversize windows; the small caches stay bounded. */
static uint32_t large_surface[GFX_MAX_W * GFX_MAX_H];
static int large_owner = -1;
static bool surface_valid[APP_COUNT], surface_focus[APP_COUNT];
static bool launcher_open;
static char launcher_query[64];
static int launcher_selected;
static int switch_order[APP_COUNT], switch_count, switch_index;
unsigned compositor_fps, compositor_missed, compositor_render_ms;
static uint32_t fps_epoch, frames;

/* Damaged regions. The compositor never repaints the whole screen: each
 * invalidated element marks the rectangle it occupies, and the painter walks
 * the window stack inside that rectangle only. Overlapping marks are merged
 * so a burst of keystrokes stays a single region. */
#define DAMAGE_MAX 8
typedef struct {
    int x, y, w, h;
} Damage;
static Damage damage[DAMAGE_MAX];
static int damage_count;
static bool damage_full;

static bool damage_overlap(Damage a, Damage b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}
void gui_damage(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0)
        return;
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > W)
        w = W - x;
    if (y + h > H)
        h = H - y;
    if (w <= 0 || h <= 0)
        return;
    Damage next = {x, y, w, h};
    for (int i = 0; i < damage_count; i++)
        if (damage_overlap(damage[i], next)) {
            int x0 = damage[i].x < x ? damage[i].x : x;
            int y0 = damage[i].y < y ? damage[i].y : y;
            int x1 = damage[i].x + damage[i].w > x + w ? damage[i].x + damage[i].w : x + w;
            int y1 = damage[i].y + damage[i].h > y + h ? damage[i].y + damage[i].h : y + h;
            damage[i] = (Damage){x0, y0, x1 - x0, y1 - y0};
            ui_dirty = true;
            return;
        }
    if (damage_count < DAMAGE_MAX) {
        damage[damage_count++] = next;
        ui_dirty = true;
        return;
    }
    damage_full = true;
    ui_dirty = true;
}
void gui_damage_all(void) {
    damage_full = true;
    ui_dirty = true;
}
static int dock_left(void);
static int dock_top(void);
/* A glass panel spreads its shadow six pixels sideways and drops it a little
 * further below. The damaged region has to cover that whole reach: the
 * painter clips to it, so a region that stops at the panel border would cut
 * the shadow off and leave a hard edge behind. */
#define SHADOW_SIDE 6
#define SHADOW_DROP 24
static void damage_dock(void) {
    gui_damage(dock_left() - 12, dock_top() - 46, 566, 140);
}
/* Moving the pointer over the dock only changes the tooltip: the dock glass
 * stays untouched instead of being rasterized again on every hover change.
 * The region covers the tooltip and the shadow it drops on the dock below it,
 * so the previous tooltip leaves no residue when the pointer moves on. */
static void damage_tooltip(int app) {
    if (app < 0 || app >= APP_COUNT)
        return;
    int left = dock_left(), top = dock_top();
    int x = left + 16 + app * 74;
    int width = text_width(app_name(app), 13) + 24;
    gui_damage(x + 25 - width / 2 - 8, top - 38, width + 16, 54);
}
static void damage_launcher(void) {
    int ox = (W - GFX_BOOT_W) / 2;
    gui_damage(ox + 236, 40, 552, 96 + APP_COUNT * 48 + 20);
}
void gui_damage_quick(void) {
    int ox = W - GFX_BOOT_W;
    if (ox < 0)
        ox = 0;
    gui_damage(ox + 676, 46, 340, 484);
}
static void damage_switch(void) {
    gui_damage((W - 468) / 2 - 10, H - 218, 488, 96);
}
static void damage_window(int app) {
    Window *w = &windows[app];
    if (w->open)
        gui_damage(w->x - SHADOW_SIDE, w->y - SHADOW_SIDE, w->w + 2 * SHADOW_SIDE,
                   w->h + SHADOW_DROP);
}
static void damage_titlebar(int app) {
    Window *w = &windows[app];
    if (w->open)
        gui_damage(w->x, w->y, w->w, 33);
}
void gui_invalidate_app(int app) {
    if (app < 0 || app >= APP_COUNT)
        return;
    surface_valid[app] = false;
    damage_window(app);
}
void gui_invalidate_all(void) {
    memset(surface_valid, 0, sizeof(surface_valid));
    glass_valid = false;
    gui_damage_all();
}

static uint32_t last_title_tick;
static int last_title_app = -1;
static const char *names[APP_COUNT] = {"Luna Home", "lunaSH",       "Files", "Luna Browser",
                                       "Settings",  "Task Manager", "Notes"};
static const int app_icons[APP_COUNT] = {ICON_MOON,    ICON_TERMINAL, ICON_FOLDER, ICON_GLOBE,
                                         ICON_SLIDERS, ICON_MEMORY,   ICON_FILE};
const char *app_name(int app) {
    static const char *pt[] = {"Início",       "lunaSH",        "Arquivos",
                               "Luna Browser", "Configurações", "Gerenciador de Tarefas",
                               "Notas"};
    return app >= 0 && app < APP_COUNT ? system_text(names[app], pt[app]) : "Desktop";
}
static bool hit(int x, int y, int rx, int ry, int w, int h) {
    return x >= rx && y >= ry && x < rx + w && y < ry + h;
}
static void remove_z(int app) {
    for (int i = 0; i < zcount; i++)
        if (zorder[i] == app) {
            memmove(zorder + i, zorder + i + 1, (zcount - i - 1) * sizeof(int));
            zcount--;
            break;
        }
}
static void next_focus(void) {
    active_app = -1;
    for (int i = zcount - 1; i >= 0; i--)
        if (windows[zorder[i]].open && !windows[zorder[i]].minimized) {
            active_app = zorder[i];
            break;
        }
}
void window_focus(int app) {
    Window *w = &windows[app];
    if (!w->open)
        return;
    int previous = active_app;
    w->minimized = false;
    remove_z(app);
    zorder[zcount++] = app;
    active_app = app;

    if (previous >= 0 && previous != app)
        damage_titlebar(previous);
    damage_titlebar(app);
    damage_dock();
}
void gui_task_closed(int app) {
    if (app < 0 || app >= APP_COUNT)
        return;
    damage_window(app);
    windows[app].open = false;
    windows[app].minimized = false;
    windows[app].pid = 0;
    remove_z(app);
    if (drag == app)
        drag = -1;
    if (active_app == app)
        next_focus();

    damage_dock();
}
void window_close(int app) {
    if (app >= 0 && app < APP_COUNT)
        task_kill(windows[app].pid);
}
void window_minimize(int app) {
    if (app < 0)
        return;
    damage_window(app);
    windows[app].minimized = true;
    if (active_app == app)
        next_focus();

    drag = -1;
    damage_dock();
}
static int dock_left(void) {
    return (W - 538) / 2;
}
static int dock_top(void) {
    return H - 94;
}
static void fit_window(Window *w) {
    if (w->maximized) {
        w->x = 8;
        w->y = 54;
        w->w = W - 16;
        w->h = H - 162;
        return;
    }
    if (w->w > W - 16)
        w->w = W - 16;
    if (w->h > H - 164)
        w->h = H - 164;
    if (w->x > W - 8 - w->w)
        w->x = W - 8 - w->w;
    if (w->x < 8)
        w->x = 8;
    if (w->y > H - 110 - w->h)
        w->y = H - 110 - w->h;
    if (w->y < 54)
        w->y = 54;
}
void gui_display_changed(void) {
    for (int i = 0; i < APP_COUNT; i++) {
        if (windows[i].open)
            fit_window(&windows[i]);
        surface_valid[i] = false;
    }
    large_owner = -1;
    drag = -1;
    gui_damage_all();
}
void window_maximize(int app) {
    if (app < 0)
        return;
    Window *w = &windows[app];
    damage_window(app);
    if (!w->maximized) {
        w->restore_x = w->x;
        w->restore_y = w->y;
        w->restore_w = w->w;
        w->restore_h = w->h;
        w->x = 8;
        w->y = 54;
        w->w = W - 16;
        w->h = H - 162;
    } else {
        w->x = w->restore_x;
        w->y = w->restore_y;
        w->w = w->restore_w;
        w->h = w->restore_h;
    }
    w->maximized = !w->maximized;
    fit_window(w);
    surface_valid[app] = false;
    drag = -1;
    damage_window(app);
    damage_dock();
}
void gui_open(int app) {
    if (app < 0 || app >= APP_COUNT)
        return;
    Window *w = &windows[app];
    if (!w->open) {
        surface_valid[app] = false;
        uint32_t pid = task_spawn(app, names[app], 3, 0);
        if (!pid)
            return;
        uint32_t previous=task_memory_enter(pid);
        app_reset(app);
        task_memory_leave(previous);
        int width = app == APP_SETTINGS ? 880
                    : app == APP_HOME   ? 650
                    : app == APP_TASKS  ? 864
                                        : 820;
        int height = app == APP_SETTINGS ? 558
                     : app == APP_HOME   ? 488
                     : app == APP_TASKS  ? 540
                                         : 510;
        *w = (Window){.app = app,
                      .x = 48 + app * 31,
                      .y = 84 + app * 17,
                      .w = width,
                      .h = height,
                      .pid = pid,
                      .open = true};
        fit_window(w);
        task_post(pid, (AppEvent){EVENT_INIT, 0, 0});
        damage_window(app);
    }
    window_focus(app);
    damage_dock();
}
void gui_focus_home(void) {
    for (int i = 0; i < APP_COUNT; i++)
        if (windows[i].open) {
            damage_window(i);
            windows[i].minimized = true;
        }
    active_app = -1;
    controls_open = false;
    gui_damage_all();
}
void gui_serial(int c) {
    if (setup_active)
        return;
    if (!windows[APP_SHELL].open)
        gui_open(APP_SHELL);
    task_post(windows[APP_SHELL].pid, (AppEvent){EVENT_KEY, c, 0});
}
static bool contains(const char *s, const char *q) {
    if (!*q)
        return true;
    for (; *s; s++) {
        const char *a = s, *b = q;
        while (*a && *b) {
            int x = *a, y = *b;
            if (x >= 'A' && x <= 'Z')
                x += 32;
            if (y >= 'A' && y <= 'Z')
                y += 32;
            if (x != y)
                break;
            a++;
            b++;
        }
        if (!*b)
            return true;
    }
    return false;
}
static int launcher_results(int *out) {
    const char *alias[] = {"home inicio",
                           "terminal console lunash",
                           "arquivos files",
                           "navegador browser internet",
                           "configuracoes settings preferencias",
                           "tarefas task tasks manager processos",
                           "notes notas editor python markdown"};
    int n = 0;
    for (int i = 0; i < APP_COUNT; i++)
        if (contains(names[i], launcher_query) || contains(alias[i], launcher_query))
            out[n++] = i;
    return n;
}
static void switch_window(bool backwards) {
    if (!switch_count) {
        for (int i = zcount - 1; i >= 0; i--)
            if (windows[zorder[i]].open)
                switch_order[switch_count++] = zorder[i];
        switch_index = 0;
    }
    if (!switch_count)
        return;
    switch_index = (switch_index + (backwards ? switch_count - 1 : 1)) % switch_count;
    window_focus(switch_order[switch_index]);
}
void gui_key(int key) {
    if (setup_active) {
        setup_key(key);
        return;
    }
    int code = key & 0xfff;
    if (code == KEY_ALT_RELEASE) {
        switch_count = 0;
        damage_switch();
        return;
    }
    if ((key & MOD_ALT) && code == 9) {
        switch_window(key & MOD_SHIFT);
        return;
    }
    if ((key & MOD_ALT) && code == KEY_F4) {
        if (launcher_open)
            launcher_open = false;
        else if (controls_open)
            controls_open = false;
        else if (active_app >= 0)
            window_close(active_app);
        gui_damage_all();
        return;
    }
    if ((key & (MOD_ALT | MOD_CTRL)) == (MOD_ALT | MOD_CTRL) && (code == 't' || code == 'T')) {
        gui_open(APP_SHELL);
        return;
    }
    if ((key & (MOD_SHIFT | MOD_CTRL)) == (MOD_SHIFT | MOD_CTRL) && code == 27) {
        gui_open(APP_TASKS);
        return;
    }
    if (code == KEY_SUPER) {
        launcher_open = !launcher_open;
        controls_open = false;
        launcher_query[0] = 0;
        launcher_selected = 0;
        damage_launcher();
        damage_dock();
        return;
    }
    if (launcher_open) {
        int found[APP_COUNT], n = launcher_results(found);
        int length = strlen(launcher_query);
        if (code == 27)
            launcher_open = false;
        else if (code == KEY_DOWN && n)
            launcher_selected = (launcher_selected + 1) % n;
        else if (code == KEY_UP && n)
            launcher_selected = (launcher_selected + n - 1) % n;
        else if (code == 10 || code == 13) {
            if (n) {
                if (launcher_selected >= n)
                    launcher_selected = 0;
                gui_open(found[launcher_selected]);
            }
            launcher_open = false;
        } else if (code == 8) {
            if (length)
                launcher_query[length - 1] = 0;
            launcher_selected = 0;
        } else if (code >= 32 && code < 127 && length < 62) {
            launcher_query[length] = code;
            launcher_query[length + 1] = 0;
            launcher_selected = 0;
        }
        damage_launcher();
        return;
    }
    if (controls_open) {
        if (code == 27) {
            controls_open = false;
            gui_damage_quick();
        }
        return;
    }
    if (code >= KEY_F1 && code <= KEY_F6)
        return;
    if (active_app >= 0)
        task_post(windows[active_app].pid, (AppEvent){EVENT_KEY, key & ~MOD_SHIFT, 0});
}
static void launcher_draw(void) {
    gfx_set_origin((W - GFX_BOOT_W) / 2, 0);
    glass(252, 57, 520, 49, 24, 1);
    icon(270, 69, ICON_SEARCH, 24, 0xffffff);
    text(310, 70, *launcher_query ? launcher_query : "Type to search apps...", 16, 0xe3efff);
    int found[APP_COUNT], n = launcher_results(found);
    if (launcher_selected >= n)
        launcher_selected = 0;
    if (n) {
        glass(252, 116, 520, n * 48 + 18, 20, 1);
        for (int i = 0; i < n; i++) {
            int y = 125 + i * 48;
            if (i == launcher_selected)
                round_rect(262, y, 500, 42, 13, 0xffffff, 26);
            icon(276, y + 7, app_icons[found[i]], 28, 0xeaf3ff);
            text(323, y + 10, names[found[i]], 16, 0xf2f7ff);
        }
    } else {
        glass(252, 116, 520, 70, 20, 1);
        text(276, 137, "No matching apps", 16, 0xc8d9ed);
    }
}
static void dock(void) {
    gfx_set_origin(0, 0);
    int left = dock_left(), top = dock_top();
    glass(left, top, 538, 76, 28, 1);
    for (int i = 0; i < APP_COUNT; i++) {
        int x = left + 16 + i * 74;
        round_rect(x, top + 12, 50, 50, 15, i == active_app ? settings_accent() : 0xb0cbe5,
                   i == active_app ? 210 : 30);
        icon(x + 8, top + 20, app_icons[i], 34, 0xf5f9ff);
        if (windows[i].open) {
            if (windows[i].minimized)
                round_rect(x + 18, top + 67, 14, 3, 1, 0xc5d6ef, 180);
            else
                circle(x + 25, top + 68, 2, 0xffffff);
        }
        if (hit(mouse_x, mouse_y, x, top + 7, 60, 65)) {
            int width = text_width(app_name(i), 13) + 24;
            glass(x + 25 - width / 2, top - 32, width, 28, 14, 1);
            text(x + 37 - width / 2, top - 27, app_name(i), 13, 0xffffff);
        }
    }
}
static void titlebar(Window *w, bool focused) {
    int x = w->x, y = w->y;
    round_rect(x + 1, y + 1, w->w - 2, 31, 15, focused ? 0xffffff : 0x182943, focused ? 22 : 24);
    const uint32_t colors[3] = {0xff6b79, 0xffce74, 0x79dca9};
    for (int i = 0; i < 3; i++) {
        int cx = x + 24 + i * 26;
        circle(cx, y + 17, 7, colors[i]);
        if (focused || hit(mouse_x, mouse_y, x + 11, y + 9, 83, 29))
            icon(cx - 8, y + 9,
                 i == 0   ? ICON_CLOSE
                 : i == 1 ? ICON_MINIMIZE
                          : ICON_MAXIMIZE,
                 16, 0x29405b);
    }
    icon(x + 106, y + 7, app_icons[w->app], 20, 0xe6f0fc);
    text(x + 137, y + 7, app_name(w->app), 16, 0xf3f8ff);
    line(x + 12, y + 32, x + w->w - 12, y + 32, 0x66809c);
}
/* A window fully hidden by another one does not need to be painted. */
static bool window_covered(int z) {
    Window *w = &windows[zorder[z]];
    for (int next = z + 1; next < zcount; next++) {
        Window *above = &windows[zorder[next]];
        if (above->open && !above->minimized && above->x <= w->x && above->y <= w->y &&
            above->x + above->w >= w->x + w->w && above->y + above->h >= w->y + w->h)
            return true;
    }
    return false;
}
static void draw_topbar(void) {
    round_rect(12, 8, W - 24, 37, 18, 0x182e48, 45);
    icon(26, 14, ICON_MOON, 24, 0xffffff);
    text(62, 15, "Luna", 16, 0xffffff);
    text(126, 17, app_name(active_app), 13, 0xe9f3ff);
    text((W - text_width("Your apps. Their own space.", 13)) / 2, 17, "Your apps. Their own space.",
         13, 0xcde1f4);
    icon(W - 265, 15, ICON_ETHERNET, 24, 0xffffff);
    icon(W - 225, 15, ICON_VOLUME, 24, 0xffffff);
    icon(W - 185, 15, ICON_SLIDERS, 24, 0xffffff);
    char clock[48];
    rtc_string(clock);
    text(W - 136, 17, clock + 12, 13, 0xffffff);
}
/* Repaints the cached surfaces of the windows whose content changed. The
 * glass background is only redrawn when the window itself moved or the theme
 * changed; a content update alone keeps the cached background. */
static bool app_dark(int app) {
    return app == APP_SHELL || app == APP_TASKS;
}
/* Rasterizes the window background into the given target. */
static void render_glass(Window *w, uint32_t *target, int stride, int height) {
    gfx_set_target(target, stride, height, w->x, w->y);
    Window paint = *w;
    paint.x = paint.y = 0;
    glass(0, 0, w->w, w->h, 20, app_dark(w->app));
}
static void update_surfaces(void) {
    for (int z = 0; z < zcount; z++) {
        Window *w = &windows[zorder[z]];
        if (!w->open || w->minimized || window_covered(z))
            continue;
        bool focused = w->app == active_app;
        bool large = w->w > GFX_CACHE_W || w->h > GFX_CACHE_H;
        uint32_t *surface = large ? large_surface : surfaces[w->app];
        int stride = large ? GFX_MAX_W : GFX_CACHE_W;
        int height = large ? GFX_MAX_H : GFX_CACHE_H;
        bool replaced = large && large_owner != w->app;
        bool content = !surface_valid[w->app] || replaced;
        bool title = surface_focus[w->app] != focused;
        if (!content && !title)
            continue;
        Window paint = *w;
        paint.x = paint.y = 0;
        if (content) {
            /* The background only changes when the window moves, when the
             * theme changes or when it was never painted. Otherwise the
             * cached copy is reused and only the client is drawn again. */
            bool cached = !large && glass_valid && glass_app == w->app &&
                          glass_x == w->x && glass_y == w->y && glass_w == w->w &&
                          glass_h == w->h;
            if (cached) {
                gfx_set_target(surface, stride, height, w->x, w->y);
                for (int j = 0; j < w->h; j++)
                    memcpy(surface + j * stride, glass_cache + j * GFX_CACHE_W, w->w * 4);
            } else {
                render_glass(w, glass_cache, GFX_CACHE_W, GFX_CACHE_H);
                glass_valid = true;
                glass_app = w->app;
                glass_x = w->x;
                glass_y = w->y;
                glass_w = w->w;
                glass_h = w->h;
                gfx_set_target(surface, stride, height, w->x, w->y);
                for (int j = 0; j < w->h; j++)
                    memcpy(surface + j * stride, glass_cache + j * GFX_CACHE_W, w->w * 4);
            }
            titlebar(&paint, focused);
            gfx_clip(12, 36, w->w - 24, w->h - 48);
            app_draw(w->app, 20, 46, w->w - 40, w->h - 66);
        } else {
            /* Only the focus changed: the titlebar strip is repainted. */
            gfx_set_target(surface, stride, height, w->x, w->y);
            gfx_clip(0, 0, w->w, 33);
            glass(0, 0, w->w, w->h, 20, app_dark(w->app));
            titlebar(&paint, focused);
        }
        surface_valid[w->app] = true;
        surface_focus[w->app] = focused;
        if (large)
            large_owner = w->app;
        /* The scene still holds the previous pixels of that rectangle. */
        if (content)
            gui_damage(w->x - SHADOW_SIDE, w->y - SHADOW_SIDE, w->w + 2 * SHADOW_SIDE,
                       w->h + SHADOW_DROP);
        else
            gui_damage(w->x, w->y, w->w, 33);
    }
}
/* Repaints one damaged rectangle: wallpaper, top bar, every window that
 * intersects it in stacking order, then the dock and the overlays. */
static void paint_region(int rx, int ry, int rw, int rh) {
    gfx_scene_target();
    gfx_clip(rx, ry, rw, rh);
    gfx_fill_wallpaper();
    draw_topbar();
    for (int z = 0; z < zcount; z++) {
        Window *w = &windows[zorder[z]];
                if (!w->open || w->minimized || window_covered(z))
            continue;
        bool large = w->w > GFX_CACHE_W || w->h > GFX_CACHE_H;
        uint32_t *surface = large ? large_surface : surfaces[w->app];
        int stride = large ? GFX_MAX_W : GFX_CACHE_W;
        gfx_window_shadow(w->x, w->y, w->w, w->h, 20);
        gfx_blit_surface(surface, stride, w->x, w->y, w->w, w->h, 20);
    }
    gfx_occlusion_clear();
    dock();
    if (controls_open)
        quick_draw();
    if (launcher_open)
        launcher_draw();
    gfx_set_origin(0, 0);
    calendar_draw();
    if (switch_count) {
        glass((W - 468) / 2, H - 208, 468, 62, 22, 1);
        icon((W - 468) / 2 + 20, H - 192, app_icons[active_app], 28, 0xffffff);
        text((W - 468) / 2 + 64, H - 190, app_name(active_app), 20, 0xffffff);
    }
}
void gui_draw(void) {
        if (setup_active) {
        setup_draw();
        damage_count = 0;
        damage_full = false;
        return;
    }
    uint32_t started = frame_clock;
    if (damage_full) {
        damage[0] = (Damage){0, 0, W, H};
        damage_count = 1;
        damage_full = false;
    }
    if (!damage_count) {
        ui_dirty = false;
        return;
    }
        update_surfaces();
    for (int i = 0; i < damage_count; i++)
        paint_region(damage[i].x, damage[i].y, damage[i].w, damage[i].h);
    for (int i = 0; i < damage_count; i++)
        gfx_present_region(damage[i].x, damage[i].y, damage[i].w, damage[i].h);
    gfx_cursor_present();
    damage_count = 0;
    compositor_render_ms = frame_clock - started;
    frames++;
    if (frame_clock - fps_epoch >= 1000) {
        compositor_fps = frames * 1000 / (frame_clock - fps_epoch);
        fps_epoch = frame_clock;
        frames = 0;
    }
    ui_dirty = false;
}

void gui_init(void) {
    gfx_init();
    setup_init();
    if (!setup_active)
        gui_open(APP_HOME);
    serial_write("GUI: floating windows / native ring-3 clients / Inter Tight + Lucide\n");
}
void gui_click(int x, int y) {
    gui_pointer(x, y, 1);
    gui_pointer(x, y, 0);
}
void gui_pointer(int x, int y, int buttons) {
    if (setup_active) {
        setup_pointer(x, y, buttons);
        return;
    }
    static int hover = -1;
    int current_hover =
        hit(x, y, dock_left() + 16, dock_top() + 6, 518, 67) ? (x - dock_left() - 16) / 74 : -1;
    if (current_hover != hover) {
        damage_tooltip(hover);
        hover = current_hover;
        damage_tooltip(hover);
    }
    bool pressed = (buttons & 1) && !(previous_buttons & 1);
    if (!(buttons & 1)) {
        if (drag >= 0)
            gui_invalidate_app(drag);
        drag = -1;
    }
    if (drag >= 0 && (buttons & 1)) {
        Window *w = &windows[drag];
        int ox = w->x, oy = w->y;
        w->x = x - drag_dx;
        w->y = y - drag_dy;
        if (w->x < 0)
            w->x = 0;
        if (w->x > W - w->w)
            w->x = W - w->w;
        if (w->y < 50)
            w->y = 50;
        if (w->y > H - 90)
            w->y = H - 90;
        /* Both the old and the new footprint must be repainted. */
        gui_damage(ox - SHADOW_SIDE, oy - SHADOW_SIDE, w->w + 2 * SHADOW_SIDE,
                   w->h + SHADOW_DROP);
        gui_damage(w->x - SHADOW_SIDE, w->y - SHADOW_SIDE, w->w + 2 * SHADOW_SIDE,
                   w->h + SHADOW_DROP);
    }
    previous_buttons = buttons;
    if (controls_open && (buttons & 1)) {
        quick_pointer(x, y, pressed, false);
        return;
    }
    if (controls_open && !(buttons & 1))
        quick_pointer(x, y, false, true);
    if (!pressed && drag < 0 && (buttons & 1) && active_app == APP_SETTINGS)
        task_post(windows[APP_SETTINGS].pid,
                  (AppEvent){EVENT_DRAG, x - windows[APP_SETTINGS].x - 20,
                             y - windows[APP_SETTINGS].y - 46});
    if (!pressed)
        return;
    if(calendar_click(x,y)){controls_open=false;launcher_open=false;gui_damage_all();return;}
    if (launcher_open) {
        x -= (W - GFX_BOOT_W) / 2;
        int found[APP_COUNT], n = launcher_results(found);
        if (hit(x, y, 252, 125, 520, n * 48)) {
            int row = (y - 125) / 48;
            if (row < n)
                gui_open(found[row]);
        }
        launcher_open = false;
        damage_launcher();
        damage_dock();
        return;
    }
    if (controls_open) {
        if (!quick_click(x, y))
            controls_open = false;
        gui_damage_quick();
        return;
    }
    if (hit(x, y, dock_left() + 16, dock_top() + 6, 518, 67)) {
        int app = (x - dock_left() - 16) / 74;
        if (app < APP_COUNT)
            gui_open(app);
        return;
    }
    if (hit(x, y, W - 195, 8, 48, 36)) {
        controls_open = !controls_open;
        gui_damage_quick();
        return;
    }
    for (int z = zcount - 1; z >= 0; z--) {
        int app = zorder[z];
        Window *w = &windows[app];
        if (!w->open || w->minimized || !hit(x, y, w->x, w->y, w->w, w->h))
            continue;
        window_focus(app);
        if (y < w->y + 33) {
            if (hit(x, y, w->x + 14, w->y + 6, 20, 22))
                window_close(app);
            else if (hit(x, y, w->x + 40, w->y + 6, 20, 22))
                window_minimize(app);
            else if (hit(x, y, w->x + 66, w->y + 6, 20, 22))
                window_maximize(app);
            else {
                if (last_title_app == app && ticks - last_title_tick < 35) {
                    window_maximize(app);
                    last_title_app = -1;
                } else {
                    last_title_app = app;
                    last_title_tick = ticks;
                    if (!w->maximized) {
                        drag = app;
                        drag_dx = x - w->x;
                        drag_dy = y - w->y;
                    }
                }
            }
            return;
        }
        task_post(w->pid, (AppEvent){EVENT_CLICK, x - w->x - 20, y - w->y - 46});
        return;
    }
    active_app = -1;
    gui_damage_all();
}

void gui_benchmark(char *out, size_t cap) {
    uint32_t start = frame_clock;
    unsigned count = 120;
    for (unsigned i = 0; i < count; i++) {
        /* Measure the worst case: every frame repaints the whole screen. */
        gui_damage_all();
        gui_draw();
    }
    unsigned elapsed = frame_clock - start;
    if (!elapsed)
        elapsed = 1;
    out[0] = 0;
    char n[32];
    append(out, "Cached composition benchmark: ", cap);
    number(n, count * 1000 / elapsed);
    append(out, n, cap);
    append(out, " fps (120 frames / ", cap);
    number(n, elapsed);
    append(out, n, cap);
    append(out, " ms). Compositor target: 60 fps.\n", cap);
}
