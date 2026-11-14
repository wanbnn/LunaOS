#ifndef LUNA_H
#define LUNA_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define GFX_BOOT_W 1024
#define GFX_BOOT_H 768
#define GFX_MAX_W 1920
#define GFX_MAX_H 1080
/* The seven cached windows are at most 880x558; the shared oversize surface
 * takes the maximized window. */
#define GFX_CACHE_W 880
#define GFX_CACHE_H 558
extern int screen_width, screen_height;
#define W screen_width
#define H screen_height
#define VERSION "0.3.0-dev"
static inline void outb(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0,%1" ::"a"(v), "Nd"(p));
}
static inline void outw(uint16_t p, uint16_t v) {
    __asm__ volatile("outw %0,%1" ::"a"(v), "Nd"(p));
}
static inline void outl(uint16_t p, uint32_t v) {
    __asm__ volatile("outl %0,%1" ::"a"(v), "Nd"(p));
}
static inline uint8_t inb(uint16_t p) {
    uint8_t v;
    __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline uint16_t inw(uint16_t p) {
    uint16_t v;
    __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline uint32_t inl(uint16_t p) {
    uint32_t v;
    __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
size_t strlen(const char *);
int strcmp(const char *, const char *);
int memcmp(const void *, const void *, size_t);
int strncmp(const char *, const char *, size_t);
char *strcpy(char *, const char *);
char *strcat(char *, const char *);
char *strchr(const char *, int);
int atoi(const char *);
void number(char *, uint32_t);
void hex(char *, uint32_t);
void append(char *, const char *, size_t);
extern volatile uint32_t ticks, frame_clock;
extern uint32_t memory_mb;
void serial_init(void);
void serial_put(char);
void serial_write(const char *);
int serial_get(void);
bool serial_interrupt(void);
bool serial_uses_irq(void);
void platform_init(void);
void timer_interrupt(void);
void kernel_fault(void);
void sleep_ticks(uint32_t);
void poweroff(void);
void reboot(void);
void rtc_string(char *);
void speaker_beep(unsigned, unsigned);
void speaker_poll(void);
void audio_init(void);
void audio_beep(unsigned, unsigned);
void audio_poll(void);
const char *audio_name(void);
void audio_status(char *, size_t);
void input_init(void);
int input_key(void);
void input_poll(void);
bool input_pending(void);
bool input_pointer(int *, int *, int *, int *);
extern int mouse_x, mouse_y, mouse_buttons;
bool input_interrupt(void);
bool input_uses_irq(void);
#define KEY_UP 128
#define KEY_DOWN 129
#define KEY_LEFT 130
#define KEY_RIGHT 131
#define KEY_SUPER 160
#define KEY_ALT_RELEASE 161
#define KEY_HOME 162
#define KEY_END 163
#define KEY_DELETE 164
#define MOD_ALT 0x1000
#define MOD_SHIFT 0x2000
#define MOD_CTRL 0x4000
#define KEY_F1 140
#define KEY_F2 141
#define KEY_F3 142
#define KEY_F4 143
#define KEY_F5 144
#define KEY_F6 145
uint32_t pci_read(uint8_t, uint8_t, uint8_t, uint8_t);
void pci_write(uint8_t, uint8_t, uint8_t, uint8_t, uint32_t);
void pci_list(char *, size_t);
bool disk_read(uint32_t, void *);
bool disk_write(uint32_t, const void *);
extern int fs_file_capacity;
/* Scratch buffers for shell/UI text commands; NOT a file-size limit. */
#define LUNA_TEXT_BUFFER 8192
/* Existing editor/shell scratch arrays still have their own 8 KiB UI budget.
 * This constant is NOT a LunaFS per-file limit. */
#define FILE_CAP LUNA_TEXT_BUFFER
#define PATH_CAP 128
typedef struct {
    char path[PATH_CAP];
    uint32_t used, size, dir;
    uint32_t mode, mtime, ctime; /* LunaFS v3 persisted POSIX metadata, epoch seconds. */
    char *data; /* Optional owned text cache, NUL-terminated when present. */
    unsigned disk;
    uint64_t disk_offset; /* Backing bytes in the committed snapshot. */
} File;
extern File *files;
typedef struct FileState {
    File *files;
    int capacity;
    uint32_t snapshot_base;
    struct FileState *next_backup;
    bool registered;
} FileState;
extern char cwd[PATH_CAP];
void fs_init(void);
bool fs_path(const char *, char *);
/* File pointers may move when the directory grows or a snapshot is loaded. */
File *fs_find(const char *);
int fs_create(const char *, bool);
int fs_remove(const char *);
int fs_write(const char *, const char *, bool);
int fs_write_bytes(const char *, const void *, size_t, bool);
int fs_write_at(const char *, const void *, size_t, uint32_t);
bool fs_read_bytes(const File *, uint32_t, void *, size_t);
bool fs_materialize(File *);
bool fs_read_text(const File *, char *, size_t);
int fs_copy(const char *, const char *);
int fs_rename(const char *, const char *);
int fs_chmod(const char *, uint32_t mode);
int fs_set_mtime(const char *, uint32_t unix_seconds);
bool fs_begin_write(const char *, bool, uint32_t);
bool fs_write_part(const void *, size_t);
bool fs_finish_write(void);
void fs_abort_write(uint32_t);
int fs_clone(FileState *destination, const File *source);
void fs_discard_state(FileState *state);
void fs_restore_state(FileState *backup);
bool fs_save(void);
bool fs_load(void);
extern bool fs_dirty;
void net_init(void);
void net_poll(void);
void net_status(char *, size_t);
bool net_set_dns(const char *); /* IPv4 address or "auto" (DHCP). */
bool net_ping(const char *, char *, size_t);
bool net_download_file(const char *, const char *, uint64_t *, char *, size_t);
int net_http(const char *, char *, size_t, char *, size_t);
int net_https(const char *, char *, size_t, char *, size_t);
/* Bodies that grow with the document. The caller releases *out. */
int net_http_alloc(const char *, char **, size_t *, char *, size_t);
int net_https_alloc(const char *, char **, size_t *, char *, size_t);
typedef bool (*NetHttpsSink)(void *, const void *, size_t);
bool net_https_stream(const char *, NetHttpsSink, void *, uint64_t *, char *, size_t);
extern bool net_ready;
void shell_init(void);
void shell_run(const char *);
void shell_key(int);
void shell_print(const char *);
extern char term_lines[160][180];
extern int term_count;
extern char shell_line[256];
extern int shell_cursor;
extern bool ui_dirty;
void gui_init(void);
void gui_draw(void);
void gui_motion_poll(void);
void browser_timer_poll(void);
void gui_refresh_status(void);
void gui_damage(int, int, int, int);
void gui_damage_quick(void);
void gui_damage_all(void);
void gui_key(int);
void gui_click(int, int);
void gui_open(int);
void browser_open(const char *);
extern int active_app;
extern bool controls_open;
extern char browser_url[256];
typedef enum {
    ICON_MOON,
    ICON_TERMINAL,
    ICON_FOLDER,
    ICON_GLOBE,
    ICON_SLIDERS,
    ICON_WIFI,
    ICON_BLUETOOTH,
    ICON_VOLUME,
    ICON_SUN,
    ICON_ETHERNET,
    ICON_FILE,
    ICON_CLOSE,
    ICON_MINIMIZE,
    ICON_MAXIMIZE,
    ICON_CHEVRON_RIGHT,
    ICON_CHEVRON_UP,
    ICON_PLAY,
    ICON_HOME,
    ICON_DISK,
    ICON_SAVE,
    ICON_ALERT,
    ICON_ARROW_LEFT,
    ICON_CURSOR,
    ICON_SEARCH,
    ICON_DOWNLOAD,
    ICON_SETTINGS,
    ICON_MEMORY,
    ICON_COMMAND,
    ICON_COUNT
} Icon;
void gfx_clip(int, int, int, int);
void gfx_get_clip(int out[4]);
void gfx_rgba_image(int, int, int, int, const unsigned char *, int, int);
void gfx_occlusion_clear(void);
void gfx_occlusion_add(int, int, int, int);
void gfx_unclip(void);
void gfx_scene_target(void);
void gfx_fill_wallpaper(void);
void gfx_present_region(int, int, int, int);
void gfx_set_target(uint32_t *, int, int, int, int);
void gfx_set_origin(int, int);
void gui_display_changed(void);
void gfx_blit_surface(const uint32_t *, int, int, int, int, int, int);
void gfx_window_shadow(int, int, int, int, int);
void gfx_init(void);
void gfx_present(void);
void gfx_wallpaper(void);
void rect(int, int, int, int, uint32_t);
void round_rect(int, int, int, int, int, uint32_t, int);
void glass(int, int, int, int, int, int);
void line(int, int, int, int, uint32_t);
void circle(int, int, int, uint32_t);
void text(int, int, const char *, int, uint32_t);
int text_width(const char *, int);
/* Text families the user can pick; the symbol family backs them up. */
void graphics_set_font_family(int);
int graphics_font_family(void);
int graphics_font_family_count(void);
const char *graphics_font_family_name(int);
void icon(int, int, int, int, uint32_t);
void cursor(void);
#endif
