#ifndef LUNA_SETTINGS_H
#define LUNA_SETTINGS_H
#include "luna.h"
typedef struct {
    int mouse_speed, left_handed, keyboard_layout, repeat_delay, repeat_rate;
    int brightness, night_light, volume, balance, muted;
    int wallpaper, dark, accent, frame_hz;
    char wallpaper_path[PATH_CAP];
    int language, developer, output_mode;
} Preferences;
extern Preferences prefs;
void settings_init(void);
bool settings_save(void);
bool settings_write(void);
void settings_changed(void);
void settings_draw(int, int, int, int);
void settings_key(int);
void settings_click(int, int, int, int);
void quick_pointer(int, int, bool, bool);
void settings_drag(int, int, int, int);
void quick_draw(void);
bool quick_click(int, int);
void quick_key(int);
void input_apply_settings(void);
void audio_apply_settings(void);
uint32_t settings_accent(void);
bool gfx_load_wallpaper(const char *);
void gfx_rebuild_wallpaper(void);
void gfx_transform_changed(void);
void gfx_draw_wall_preview(int, int, int, int, int);
void gui_invalidate_app(int);
void gui_invalidate_all(void);
extern volatile uint32_t frame_clock;
extern unsigned compositor_fps, compositor_missed, compositor_render_ms;
void gfx_cursor_present(void);
bool gfx_fast_scanout(void);
int gfx_mode_count(void);
const char *gfx_mode_name(int);
bool gfx_change_mode(int);
extern int display_mode;
const char *system_text(const char *, const char *);
void wireless_status(char *,size_t);
#endif
