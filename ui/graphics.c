#include "boot.h"
#include "font.h"
#include "icons.h"
#include "luna.h"
#include "settings.h"
#include "perf.h"
#include "task.h"
int screen_width = GFX_BOOT_W, screen_height = GFX_BOOT_H;
static uint32_t scene[GFX_MAX_W * GFX_MAX_H], wall[GFX_MAX_W * GFX_MAX_H];
static uint32_t image[64 * 64];
static int image_w, image_h;
static uint32_t *back = scene;
static int target_w = GFX_BOOT_W, target_h = GFX_BOOT_H, wall_x, wall_y;
static int origin_x, origin_y;
static int cursor_x = -100, cursor_y = -100;
static uint32_t red_table[256], green_table[256], blue_table[256];
static volatile uint8_t *fb;
static uint32_t pitch;
static int bpp;
static bool bochs_modes, framebuffer_wc;
static bool raw_scanout;
int display_mode;
static const int mode_w[] = {1024, 1280, 1360, 1600, 1920};
static const int mode_h[] = {768, 720, 768, 900, 1080};
static const char *mode_names[] = {"1024 x 768", "1280 x 720", "1360 x 768", "1600 x 900",
                                   "1920 x 1080"};
int gfx_mode_count(void) {
    return bochs_modes ? 5 : 1;
}
const char *gfx_mode_name(int i) {
    static char firmware_mode[48];
    if (!bochs_modes) {
        char n[16];
        number(firmware_mode, W);
        strcat(firmware_mode, " x ");
        number(n, H);
        strcat(firmware_mode, n);
        return firmware_mode;
    }
    return mode_names[i >= 0 && i < gfx_mode_count() ? i : 0];
}
bool gfx_change_mode(int i) {
    if (i < 0 || i >= gfx_mode_count())
        return false;
    if (!bochs_modes)
        return i == 0;
    outw(0x1ce, 4);
    outw(0x1cf, 0);
    outw(0x1ce, 1);
    outw(0x1cf, mode_w[i]);
    outw(0x1ce, 2);
    outw(0x1cf, mode_h[i]);
    outw(0x1ce, 3);
    outw(0x1cf, 32);
    outw(0x1ce, 4);
    outw(0x1cf, 0x41);
    screen_width = mode_w[i];
    screen_height = mode_h[i];
    pitch = W * 4;
    mouse_x = W / 2;
    mouse_y = H / 2;
    display_mode = i;
    prefs.output_mode = i;
    cursor_x = cursor_y = -100;
    gfx_scene_target();
    gfx_rebuild_wallpaper();
    gui_display_changed();
    return true;
}
int display_brightness = 100;
static uint8_t red_pos, green_pos, blue_pos;
static int clip_x, clip_y, clip_w = GFX_BOOT_W, clip_h = GFX_BOOT_H;
void gfx_set_target(uint32_t *data, int width, int height, int wx, int wy) {
    origin_x = origin_y = 0;
    back = data;
    target_w = width;
    target_h = height;
    wall_x = wx;
    wall_y = wy;
    clip_x = clip_y = 0;
    clip_w = width;
    clip_h = height;
    gfx_occlusion_clear();
}

static struct {
    int x, y, w, h;
} covers[12];
static int cover_count;
void gfx_occlusion_clear(void) {
    cover_count = 0;
}
void gfx_occlusion_add(int x, int y, int w, int h) {
    if (cover_count < 12) {
        covers[cover_count].x = x;
        covers[cover_count].y = y;
        covers[cover_count].w = w;
        covers[cover_count++].h = h;
    }
}
static bool visible(int x, int y) {
    if ((unsigned)x >= (unsigned)target_w || (unsigned)y >= (unsigned)target_h || x < clip_x ||
        y < clip_y || x >= clip_x + clip_w || y >= clip_y + clip_h)
        return false;
    for (int i = 0; i < cover_count; i++)
        if (x >= covers[i].x && y >= covers[i].y && x < covers[i].x + covers[i].w &&
            y < covers[i].y + covers[i].h)
            return false;
    return true;
}
/* Clips a shape box given in local coordinates against the current clip and
 * returns the surviving bounds, still relative to the shape origin. Every
 * primitive walks only this window, so a damaged region never pays for the
 * rest of the screen. */
static void shape_box(int x, int y, int w, int h, int *bx, int *by, int *bw, int *bh) {
    int x0 = x + origin_x, y0 = y + origin_y, x1 = x0 + w, y1 = y0 + h;
    if (x0 < clip_x)
        x0 = clip_x;
    if (y0 < clip_y)
        y0 = clip_y;
    if (x1 > clip_x + clip_w)
        x1 = clip_x + clip_w;
    if (y1 > clip_y + clip_h)
        y1 = clip_y + clip_h;
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > target_w)
        x1 = target_w;
    if (y1 > target_h)
        y1 = target_h;
    if (x1 < x0)
        x1 = x0;
    if (y1 < y0)
        y1 = y0;
    /* The bounds stay relative to the shape origin, so the caller keeps
     * walking its own 0-based geometry. */
    *bx = x0 - (x + origin_x);
    *by = y0 - (y + origin_y);
    *bw = x1 - x0;
    *bh = y1 - y0;
}
void gfx_clip(int x, int y, int w, int h) {
    clip_x = x + origin_x;
    clip_y = y + origin_y;
    clip_w = w;
    clip_h = h;
}
void gfx_get_clip(int out[4]) {
    out[0] = clip_x - origin_x;
    out[1] = clip_y - origin_y;
    out[2] = clip_w;
    out[3] = clip_h;
}
void gfx_unclip(void) {
    clip_x = clip_y = 0;
    clip_w = target_w;
    clip_h = target_h;
}
/* The clip rectangle is kept in target coordinates, so it survives a change
 * of origin: the same pixels stay clipped no matter where the content is
 * drawn from. Unclipping here used to release the dock, the launcher and the
 * control panel from the damaged region, which rasterized the whole glass
 * again on every hover and piled a second shadow over the one already in the
 * frame buffer. */
void gfx_set_origin(int x, int y) {
    origin_x = x;
    origin_y = y;
}
/* (x * 0x8081) >> 23 is exactly x / 255 for every x below 65536, so the mixer
 * needs no division and keeps the previous rounding bit for bit. */
static inline uint32_t mix(int a, int b, int alpha) {
    uint32_t x = (uint32_t)(a * (255 - alpha) + b * alpha);
    return (x * 0x8081u) >> 23;
}
static uint32_t blend(uint32_t a, uint32_t b, int alpha) {
    return (mix((a >> 16) & 255, (b >> 16) & 255, alpha) << 16) |
           (mix((a >> 8) & 255, (b >> 8) & 255, alpha) << 8) | mix(a & 255, b & 255, alpha);
}
static void pixel(int x, int y, uint32_t c, int a) {
    x += origin_x;
    y += origin_y;
    if (visible(x, y)) {
        unsigned i = y * target_w + x;
        back[i] = a == 255 ? c : blend(back[i], c, a);
    }
}
void gfx_init(void) {
    volatile uint8_t *v = (uint8_t *)0x6000;
    pitch = *(volatile uint16_t *)(v + 16);
    bpp = v[25];
    fb = (void *)*(volatile uint32_t *)(v + 40);
    red_pos = v[32];
    green_pos = v[34];
    blue_pos = v[36];
    if (boot_is_uefi()) {
        screen_width = *(volatile uint16_t *)(v + 18);
        screen_height = *(volatile uint16_t *)(v + 20);
        if (W < 1024 || H < 720 || W > GFX_MAX_W || H > GFX_MAX_H)
            kernel_fault();
        gfx_scene_target();
    }
    if (pitch < (uint32_t)W * (bpp / 8) || *(volatile uint16_t *)(v + 18) != W || *(volatile uint16_t *)(v + 20) != H ||
        (bpp != 24 && bpp != 32) || !fb || red_pos > bpp - 8 ||
        green_pos > bpp - 8 || blue_pos > bpp - 8) {
        serial_write("Unsupported VBE framebuffer\n");
        kernel_fault();
    }
    /* Bochs/QEMU stdvga supports a real 32-bit linear mode without BIOS calls. */
    uint32_t bar = pci_read(0, 2, 0, 0x10);
    outw(0x1ce, 0);
    uint16_t id = inw(0x1cf);
    if (id >= 0xb0c0 && id <= 0xb0c5 && bar && !(bar & 1)) {
        outw(0x1ce, 4);
        outw(0x1cf, 0);
        outw(0x1ce, 1);
        outw(0x1cf, W);
        outw(0x1ce, 2);
        outw(0x1cf, H);
        outw(0x1ce, 3);
        outw(0x1cf, 32);
        outw(0x1ce, 4);
        outw(0x1cf, 0x41);
        bochs_modes = true;
        fb = (void *)(bar & ~15u);
        pitch = W * 4;
        bpp = 32;
        red_pos = 16;
        green_pos = 8;
        blue_pos = 0;
    }
    uint32_t framebuffer_bytes = bochs_modes ? GFX_MAX_W * GFX_MAX_H * 4u : pitch * H;
    framebuffer_wc = task_framebuffer_wc((uintptr_t)fb, framebuffer_bytes);
    serial_write(framebuffer_wc ? "Video: write-combining framebuffer\n" : "Video: firmware framebuffer cache policy\n");
    gfx_transform_changed();
    if (prefs.wallpaper == 4)
        gfx_load_wallpaper(prefs.wallpaper_path);
    gfx_rebuild_wallpaper();
    if (prefs.output_mode && bochs_modes)
        gfx_change_mode(prefs.output_mode);
}
static uint32_t wallpaper_base(int x, int y) {
    int r = 93 + y * 27 / GFX_BOOT_H, g = 139 - y * 12 / GFX_BOOT_H, b = 181 + y * 24 / GFX_BOOT_H;
    int dx = x - 140, dy = y - 110;
    int glow = 100 - (dx * dx + dy * dy) / 3400;
    if (glow > 0) {
        r += glow / 3;
        g += glow / 2;
        b += glow / 3;
    }
    dx = x - 860;
    dy = y - 480;
    int lilac = 115 - (dx * dx + dy * dy) / 2400;
    if (lilac > 0) {
        r += lilac / 2;
        g -= lilac / 8;
        b += lilac / 3;
    }
    dx = x - 420;
    dy = y - 700;
    int blue = 155 - (dx * dx + dy * dy) / 1800;
    if (blue > 0) {
        r -= blue / 3;
        g -= blue / 4;
        b += blue / 4;
    }
    // Soft sweeping ribbon.
    int center = 550 - (x * x) / 2300;
    int dist = y - center;
    if (dist < 0)
        dist = -dist;
    int ribbon = 65 - dist / 3;
    if (ribbon > 0) {
        r += ribbon / 4;
        g += ribbon / 2;
        b += ribbon / 3;
    }
    if (r > 255)
        r = 255;
    if (g > 255)
        g = 255;
    if (b > 255)
        b = 255;
    if (r < 0)
        r = 0;
    if (g < 0)
        g = 0;
    if (b < 0)
        b = 0;
    return (r << 16) | (g << 8) | b;
}
void gfx_rebuild_wallpaper(void) {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint32_t base = wallpaper_base(x * GFX_BOOT_W / W, y * GFX_BOOT_H / H);
            int r = (base >> 16) & 255, g = (base >> 8) & 255, b = base & 255;
            if (prefs.wallpaper == 1) {
                int t = r;
                r = g / 2 + 30;
                g = b / 2 + 60;
                b = t + 40;
            }
            if (prefs.wallpaper == 2) {
                int t = b;
                b = g / 2 + 90;
                g = r / 2 + 45;
                r = t;
            }
            if (prefs.wallpaper == 3) {
                r = (r + g + b) / 4;
                g = r + 12;
                b = r + 26;
            }
            wall[y * W + x] = (r << 16) | (g << 8) | b;
            if (prefs.wallpaper == 4 && image_w && image_h) {
                unsigned fx = (unsigned)x * (image_w - 1) * 256 / (W - 1),
                         fy = (unsigned)y * (image_h - 1) * 256 / (H - 1);
                unsigned ix = fx >> 8, iy = fy >> 8, ax = fx & 255, ay = fy & 255,
                         jx = ix + 1 < (unsigned)image_w ? ix + 1 : ix,
                         jy = iy + 1 < (unsigned)image_h ? iy + 1 : iy;
                uint32_t colors[4] = {image[iy * image_w + ix], image[iy * image_w + jx],
                                      image[jy * image_w + ix], image[jy * image_w + jx]},
                         color = 0;
                for (int shift = 0; shift <= 16; shift += 8) {
                    unsigned top = (((colors[0] >> shift) & 255) * (256 - ax) +
                                    ((colors[1] >> shift) & 255) * ax);
                    unsigned bottom = (((colors[2] >> shift) & 255) * (256 - ax) +
                                       ((colors[3] >> shift) & 255) * ax);
                    color |= ((top * (256 - ay) + bottom * ay) >> 16) << shift;
                }
                wall[y * W + x] = color;
            }
        }
}
void gfx_scene_target(void) {
    gfx_set_target(scene, W, H, 0, 0);
}
void gfx_wallpaper(void) {
    gfx_set_target(scene, W, H, 0, 0);
    memcpy(scene, wall, (size_t)W * H * 4);
}
/* Copies the wallpaper into the current target, limited to the clip rectangle.
 * The damaged region is refilled instead of the whole screen. */
void gfx_fill_wallpaper(void) {
    int x0 = clip_x, y0 = clip_y, x1 = clip_x + clip_w, y1 = clip_y + clip_h;
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > W)
        x1 = W;
    if (y1 > H)
        y1 = H;
    for (int y = y0; y < y1; y++)
        memcpy(back + y * target_w + x0, wall + y * W + x0, (size_t)(x1 - x0) * 4);
}
void gfx_transform_changed(void) {
    display_brightness = prefs.brightness;
    raw_scanout = gfx_fast_scanout() && prefs.brightness == 100 && !prefs.night_light;
    for (int i = 0; i < 256; i++) {
        int v = i * prefs.brightness / 100;
        red_table[i] = (uint32_t)v << red_pos;
        green_table[i] = (uint32_t)(prefs.night_light ? v * 86 / 100 : v) << green_pos;
        blue_table[i] = (uint32_t)(prefs.night_light ? v * 64 / 100 : v) << blue_pos;
    }
}
static uint32_t output_color(uint32_t c) {
    return red_table[(c >> 16) & 255] | green_table[(c >> 8) & 255] | blue_table[c & 255];
}
bool gfx_fast_scanout(void) {
    return bpp == 32 && red_pos == 16 && green_pos == 8 && blue_pos == 0;
}
static void write_scanline(int x, int y, const uint32_t *pixels, unsigned count) {
    volatile uint8_t *dst = fb + y * pitch + x * (bpp / 8);
    if (raw_scanout) {
        memcpy((void *)dst, pixels, count * 4);
    } else if (bpp == 32 && !((uintptr_t)dst & 3)) {
        volatile uint32_t *words = (volatile uint32_t *)dst;
        for (unsigned i = 0; i < count; i++)
            words[i] = output_color(pixels[i]);
    } else {
        unsigned bytes = bpp / 8;
        for (unsigned i = 0; i < count; i++, dst += bytes) {
            uint32_t color = output_color(pixels[i]);
            dst[0] = color;
            dst[1] = color >> 8;
            dst[2] = color >> 16;
            if (bytes == 4)
                dst[3] = color >> 24;
        }
    }
}

#define CURSOR_SIZE 24
/* Publish the cursor and its background together. The scene always remains
 * cursor-free, including when the cursor crosses a partially redrawn window. */
static void present_span(int x0, int y0, int x1, int y1) {
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 <= x0 || y1 <= y0)
        return;
    int cx = mouse_x - 4, cy = mouse_y - 4;
    int left = x0 > cx ? x0 : cx;
    int right = x1 < cx + CURSOR_SIZE ? x1 : cx + CURSOR_SIZE;
    const uint8_t *mask = icon_atlases[2] + ICON_CURSOR * (CURSOR_SIZE * CURSOR_SIZE / 2);
    for (int y = y0; y < y1; y++) {
        const uint32_t *row = scene + y * W;
        if (left >= right || y < cy || y >= cy + CURSOR_SIZE) {
            write_scanline(x0, y, row + x0, x1 - x0);
            continue;
        }
        uint32_t overlay[CURSOR_SIZE];
        for (int x = left; x < right; x++) {
            unsigned offset = (y - cy) * CURSOR_SIZE + x - cx;
            int alpha = ((mask[offset / 2] >> (4 * (offset % 2))) & 15) * 17;
            overlay[x - left] = alpha ? blend(row[x], 0xffffff, alpha) : row[x];
        }
        if (left > x0)
            write_scanline(x0, y, row + x0, left - x0);
        write_scanline(left, y, overlay, right - left);
        if (right < x1)
            write_scanline(right, y, row + right, x1 - right);
    }
}

static void scanout_fence(void) {
    if (framebuffer_wc)
        __asm__ volatile("sfence" ::: "memory");
}
void gfx_present(void) {
    PerfArea previous = perf_enter(PERF_SCANOUT);
    present_span(0, 0, W, H);
    scanout_fence();
    cursor_x = mouse_x - 4;
    cursor_y = mouse_y - 4;
    perf_leave(previous);
}

void gfx_present_region(int x, int y, int w, int h) {
    present_span(x, y, x + w, y + h);
}

void gfx_cursor_present(void) {
    int x = mouse_x - 4, y = mouse_y - 4;
    if (x != cursor_x || y != cursor_y) {
        PerfArea previous = perf_enter(PERF_CURSOR);
        /* Write the new cursor first. A subsequent restore also includes any
         * overlap, so it cannot expose the background under the new cursor. */
        present_span(x, y, x + CURSOR_SIZE, y + CURSOR_SIZE);
        present_span(cursor_x, cursor_y, cursor_x + CURSOR_SIZE, cursor_y + CURSOR_SIZE);
        cursor_x = x;
        cursor_y = y;
        perf_leave(previous);
    }
    scanout_fence();
}
void gfx_draw_wall_preview(int x, int y, int w, int h, int theme) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            uint32_t c = wallpaper_base(i * GFX_BOOT_W / w, j * GFX_BOOT_H / h);
            if (theme) {
                unsigned r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
                if (theme == 1) {
                    unsigned t = r;
                    r = g / 2 + 30;
                    g = b / 2 + 60;
                    b = t + 40;
                } else if (theme == 2) {
                    unsigned t = b;
                    b = g / 2 + 90;
                    g = r / 2 + 45;
                    r = t;
                } else if (theme == 3) {
                    r = (r + g + b) / 4;
                    g = r + 12;
                    b = r + 26;
                }
                c = (r << 16) | (g << 8) | b;
            }
            pixel(x + i, y + j, c, 255);
        }
}
void rect(int x, int y, int w, int h, uint32_t c) {
    int bx, by, bw, bh;
    shape_box(x, y, w, h, &bx, &by, &bw, &bh);
    if (!bw)
        return;
    for (int j = by; j < by + bh; j++) {
        if (cover_count) {
            for (int i = bx; i < bx + bw; i++)
                pixel(x + i, y + j, c, 255);
        } else {
            uint32_t *row = back + (y + j + origin_y) * target_w + x + bx + origin_x;
            unsigned count = bw;
            __asm__ volatile("cld; rep stosl" : "+D"(row), "+c"(count) : "a"(c) : "memory");
        }
    }
}
/* Four-by-four subpixel coverage, evaluated only in the rounded corners.
 * Interior pixels stay on the solid fast path. All native shapes share this rasterizer. */
static int rounded_coverage(int x, int y, int w, int h, int r) {
    if (x < 0 || y < 0 || x >= w || y >= h || w <= 0 || h <= 0)
        return 0;
    if (r > w / 2)
        r = w / 2;
    if (r > h / 2)
        r = h / 2;
    if (r <= 0)
        return 255;
    if ((x >= r && x < w - r) || (y >= r && y < h - r))
        return 255;
    int cx = (x < r ? r : w - r) * 8, cy = (y < r ? r : h - r) * 8;
    int dx = x * 8 + 4 - cx, dy = y * 8 + 4 - cy, rr = r * 8;
    int distance = dx * dx + dy * dy;
    if (distance <= (rr - 6) * (rr - 6))
        return 255;
    if (distance >= (rr + 6) * (rr + 6))
        return 0;
    int count = 0;
    for (int sy = 1; sy < 8; sy += 2)
        for (int sx = 1; sx < 8; sx += 2) {
            dx = x * 8 + sx - cx;
            dy = y * 8 + sy - cy;
            if (dx * dx + dy * dy <= rr * rr)
                count++;
        }
    return (count * 255 + 8) / 16;
}
void round_rect(int x, int y, int w, int h, int r, uint32_t c, int alpha) {
    int bx, by, bw, bh;
    shape_box(x, y, w, h, &bx, &by, &bw, &bh);
    for (int j = by; j < by + bh; j++)
        for (int i = bx; i < bx + bw; i++) {
            int coverage = rounded_coverage(i, j, w, h, r);
            if (coverage)
                pixel(x + i, y + j, c, (alpha * coverage + 127) / 255);
        }
}
/* The opaque panel replaces its interior shadow. Rasterize only the exposed
 * strips and corners; painting four full shadow rectangles wasted most of a frame. */
static void shadow_span(int x, int y, int w, int h, int r, int row, int begin, int end) {
    for (int i = begin; i < end; i++) {
        if (!visible(x + i + origin_x, y + row + origin_y))
            continue;
        int coverage = rounded_coverage(i, row, w, h, r);
        if (coverage)
            pixel(x + i, y + row, 0x122138, (6 * coverage + 127) / 255);
    }
}
void gfx_window_shadow(int x, int y, int w, int h, int r) {
    for (int spread = 12; spread > 0; spread -= 3) {
        int offset = spread / 2, sw = w + spread, sh = h + spread, sr = r + offset;
        int bx, by, bw, bh;
        shape_box(x - offset, y + offset, sw, sh, &bx, &by, &bw, &bh);
        if (bw <= 0 || bh <= 0)
            continue;
        for (int row = by; row < by + bh; row++) {
            int panel_y = row + offset;
            int lo = bx, hi = bx + bw;
            if (panel_y >= h) {
                shadow_span(x - offset, y + offset, sw, sh, sr, row, lo, hi);
            } else {
                int corner = (panel_y < r || panel_y >= h - r) ? r : 0;
                int a = offset + corner, b = offset + w - corner;
                if (a < lo)
                    a = lo;
                if (a < hi)
                    shadow_span(x - offset, y + offset, sw, sh, sr, row, lo, a);
                if (b < lo)
                    b = lo;
                if (b < hi)
                    shadow_span(x - offset, y + offset, sw, sh, sr, row, b, hi);
            }
        }
    }
}
void glass(int x, int y, int w, int h, int r, int dark) {
    gfx_window_shadow(x, y, w, h, r);
    int bx, by, bw, bh;
    shape_box(x, y, w, h, &bx, &by, &bw, &bh);
    bool shaded = dark || prefs.dark;
    for (int j = by; j < by + bh; j++) {
        int highlight = 18 * (h - j) / h;
        int sy = y + j + wall_y + origin_y;
        sy = sy < 0 ? 0 : sy >= H ? H - 1 : sy;
        for (int i = bx; i < bx + bw; i++) {
            int px = x + i, py = y + j;
            bool interior = (i >= r && i < w - r && j > 0 && j < h - 1) ||
                            (j >= r && j < h - r && i > 0 && i < w - 1);
            int outer = interior ? 255 : rounded_coverage(i, j, w, h, r);
            if (!outer)
                continue;
            int sx = px + wall_x + origin_x;
            if (sx < 0)
                sx = 0;
            if (sx >= W)
                sx = W - 1;
            uint32_t c = wall[sy * W + sx];
            c = blend(c, shaded ? 0x142239 : 0x29445d, shaded ? 184 : 80);
            c = blend(c, 0xffffff, highlight);
            if (interior && !cover_count) {
                back[(py + origin_y) * target_w + px + origin_x] = c;
                continue;
            }
            pixel(px, py, c, outer);
            int inner = rounded_coverage(i - 1, j - 1, w - 2, h - 2, r - 1);
            int edge = outer - inner;
            if (edge > 0)
                pixel(px, py, 0xffffff, (edge * (j < h / 2 ? 112 : 45) + 127) / 255);
        }
    }
}
void line(int x0, int y0, int x1, int y1, uint32_t c) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1, dy = y1 > y0 ? y0 - y1 : y1 - y0,
        sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        pixel(x0, y0, c, 255);
        if (x0 == x1 && y0 == y1)
            break;
        int e = 2 * err;
        if (e >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}
/* RGBA PNG pixels, scaled into the current clipped window surface. */
void gfx_rgba_image(int x, int y, int w, int h, const unsigned char *rgba, int sw, int sh) {
    if (!rgba || w <= 0 || h <= 0 || sw <= 0 || sh <= 0) return;
    int bx, by, bw, bh;
    shape_box(x, y, w, h, &bx, &by, &bw, &bh);
    for (int j = by; j < by + bh; j++)
        for (int i = bx; i < bx + bw; i++) {
            const unsigned char *p = rgba + (((j * sh / h) * sw + i * sw / w) * 4);
            pixel(x + i, y + j, ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2], p[3]);
        }
}
void circle(int x, int y, int r, uint32_t c) {
    round_rect(x - r, y - r, 2 * r, 2 * r, r, c, 255);
}
/* Decodes one UTF-8 codepoint. Sequences of three and four bytes are read
 * properly so characters outside the basic plane keep their shape. */
static int decode(const char **s) {
    unsigned char c = *(*s)++;
    if (c < 128)
        return c;
    int extra;
    int value;
    if ((c & 0xe0) == 0xc0) {
        extra = 1;
        value = c & 31;
    } else if ((c & 0xf0) == 0xe0) {
        extra = 2;
        value = c & 15;
    } else if ((c & 0xf8) == 0xf0) {
        extra = 3;
        value = c & 7;
    } else {
        return '?';
    }
    for (int i = 0; i < extra; i++) {
        unsigned char next = **s;
        if ((next & 0xc0) != 0x80) {
            /* A truncated sequence is not a character. The lead byte is
             * already consumed, so the walk always moves forward. */
            return '?';
        }
        (*s)++;
        value = (value << 6) | (next & 63);
    }
    return value;
}
static int font_index(int size) {
    for (int i = 0; i < 5; i++)
        if (font_sizes[i] == size)
            return i;
    return 1;
}
/* The family the user chose; the symbol family backs it up for the glyphs
 * the text families do not carry. */
static int font_family = 0;
int graphics_font_family(void) { return font_family; }
int graphics_font_family_count(void) { return FONT_TEXT_FAMILIES; }
const char *graphics_font_family_name(int family) {
    if (family < 0 || family >= FONT_TEXT_FAMILIES)
        return font_family_names[font_family];
    return font_family_names[family];
}
void graphics_set_font_family(int family) {
    if (family >= 0 && family < FONT_TEXT_FAMILIES)
        font_family = family;
}
/* Position of a codepoint in the table shared by every family. */
static int codepoint_slot(int c) {
    int lo = 0, hi = FONT_CODEPOINTS - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if ((int)font_codepoints[mid] == c)
            return mid;
        if ((int)font_codepoints[mid] < c)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}
typedef struct {
    int family;
    unsigned short slot;
} GlyphRef;
/* Finds the glyph of a codepoint, falling back to the symbol family. A
 * codepoint no family carries has no glyph: the caller draws the empty box
 * a browser shows instead of a question mark that the page never asked for. */
static bool glyph_for(int c, GlyphRef *ref) {
    int slot = codepoint_slot(c);
    if (slot < 0)
        return false;
    unsigned short s = font_slots[font_family][slot];
    if (s != 0xFFFF) {
        ref->family = font_family;
        ref->slot = s;
        return true;
    }
    s = font_slots[FONT_SYMBOL_FAMILY][slot];
    if (s != 0xFFFF) {
        ref->family = FONT_SYMBOL_FAMILY;
        ref->slot = s;
        return true;
    }
    return false;
}
/* Width reserved for a codepoint no family carries. */
static int missing_advance(int fi) {
    int slot = codepoint_slot('?');
    if (slot < 0)
        return 0;
    unsigned short s = font_slots[font_family][slot];
    if (s == 0xFFFF)
        return 0;
    return font_glyphs[font_family][fi][s].advance;
}
/* The hollow box browsers draw for a glyph the font does not have. */
static void draw_missing(int x, int y, int adv, int size, uint32_t color) {
    int w = adv - 2;
    if (w < 2)
        w = 2;
    int h = size * 3 / 4;
    if (h < 3)
        h = 3;
    int top = (size - h) / 2 + 1;
    for (int j = top; j < top + h; j++)
        for (int i = 1; i <= w; i++)
            if (j == top || j == top + h - 1 || i == 1 || i == w)
                pixel(x + i, y + j, color, 255);
}
int text_width(const char *s, int size) {
    int fi = font_index(size), w = 0;
    while (*s) {
        int c = decode(&s);
        GlyphRef ref;
        if (!glyph_for(c, &ref)) {
            w += missing_advance(fi);
            continue;
        }
        w += font_glyphs[ref.family][fi][ref.slot].advance;
    }
    return w;
}
void text(int x, int y, const char *s, int size, uint32_t color) {
    int fi = font_index(size), start = x;
    while (*s && y + origin_y < clip_y + clip_h) {
        int c = decode(&s);
        if (c == '\n') {
            y += size + 7;
            x = start;
            continue;
        }
        GlyphRef ref;
        if (!glyph_for(c, &ref)) {
            int adv = missing_advance(fi);
            draw_missing(x, y, adv, size, color);
            x += adv;
            continue;
        }
        const Glyph *g = &font_glyphs[ref.family][fi][ref.slot];
        int bx, by, bw, bh;
        shape_box(x, y, g->w, g->h, &bx, &by, &bw, &bh);
        for (int j = by; j < by + bh; j++)
            for (int i = bx; i < bx + bw; i++) {
                unsigned off = g->offset + j * g->w + i;
                int a = (font_data[ref.family][fi][off / 2] >> (4 * (off % 2))) & 15;
                if (a)
                    pixel(x + i, y + j, color, a * 17);
            }
        x += g->advance;
    }
}
static unsigned icon_alpha(const uint8_t *data, int source, int x, int y) {
    unsigned offset = y * source + x;
    return ((data[offset / 2] >> (4 * (offset % 2))) & 15) * 17;
}
void icon(int x, int y, int kind, int size, uint32_t color) {
    if (kind < 0 || kind >= ICON_COUNT || size <= 0)
        return;
    const unsigned count = sizeof(icon_sizes) / sizeof(icon_sizes[0]);
    unsigned atlas = count - 1;
    /* Prefer an exact SVG raster, otherwise downsample the next larger one. */
    for (unsigned i = 0; i < count; i++)
        if (icon_sizes[i] >= size) {
            atlas = i;
            break;
        }
    int source = icon_sizes[atlas];
    const uint8_t *data = icon_atlases[atlas] + kind * ((source * source + 1) / 2);
    for (int j = 0; j < size; j++)
        for (int i = 0; i < size; i++) {
            unsigned alpha;
            if (source == size) {
                alpha = icon_alpha(data, source, i, j);
            } else {
                /* Bilinear coverage for uncommon sizes; no nearest-neighbor enlargement. */
                int sx = (int)(((int64_t)(2 * i + 1) * source * 256) / (2 * size)) - 128;
                int sy = (int)(((int64_t)(2 * j + 1) * source * 256) / (2 * size)) - 128;
                if (sx < 0)
                    sx = 0;
                if (sy < 0)
                    sy = 0;
                int x0 = sx / 256, y0 = sy / 256;
                int x1 = x0 + 1 < source ? x0 + 1 : x0;
                int y1 = y0 + 1 < source ? y0 + 1 : y0;
                unsigned fx = sx & 255, fy = sy & 255;
                unsigned top = icon_alpha(data, source, x0, y0) * (256 - fx) +
                               icon_alpha(data, source, x1, y0) * fx;
                unsigned bottom = icon_alpha(data, source, x0, y1) * (256 - fx) +
                                  icon_alpha(data, source, x1, y1) * fx;
                alpha = (top * (256 - fy) + bottom * fy + 32768) / 65536;
            }
            if (alpha)
                pixel(x + i, y + j, color, alpha);
        }
}

void cursor(void) {
    icon(mouse_x - 4, mouse_y - 4, ICON_CURSOR, 24, 0xffffff);
}

void gfx_blit_surface(const uint32_t *surface, int stride, int x, int y, int w, int h, int r) {
    int bx, by, bw, bh;
    shape_box(x, y, w, h, &bx, &by, &bw, &bh);
    for (int j = by; j < by + bh; j++) {
        int yy = y + j;
        if ((unsigned)yy >= (unsigned)H)
            continue;
        int begin = bx, end = bx + bw;
        if (x + begin < 0)
            begin = -x;
        if (x + end > W)
            end = W - x;
        if (begin >= end)
            continue;
        int left = begin, right = end;
        if (j < r || j >= h - r) {
            left = r > begin ? r : begin;
            if (left > end) left = end;
            right = w - r < end ? w - r : end;
            if (right < left) right = left;
        }
        if (left < right)
            memcpy(scene + yy * W + x + left, surface + j * stride + left, (right - left) * 4);
        for (int i = begin; i < end; i++) {
            if (i == left)
                i = right;
            if (i == end)
                break;
            int coverage = rounded_coverage(i, j, w, h, r);
            if (coverage)
                pixel(x + i, yy, surface[j * stride + i], coverage);
        }
    }
}

static bool ppm_number(const char **p, int *out) {
    while (**p) {
        if (**p == '#') {
            while (**p && **p != '\n')
                (*p)++;
        } else if (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n')
            (*p)++;
        else
            break;
    }
    if (**p < '0' || **p > '9')
        return false;
    int v = 0;
    while (**p >= '0' && **p <= '9') {
        v = v * 10 + *(*p)++ - '0';
        if (v > 65535)
            return false;
    }
    *out = v;
    return true;
}
bool gfx_load_wallpaper(const char *path) {
    File *f = fs_find(path);
    if (!f || f->dir || f->size > 65536 || !fs_materialize(f) || strncmp(f->data, "P3", 2))
        return false;
    const char *p = f->data + 2;
    int width, height, max;
    if (!ppm_number(&p, &width) || !ppm_number(&p, &height) || !ppm_number(&p, &max) || width < 1 ||
        height < 1 || width > 64 || height > 64 || max != 255)
        return false;
    static uint32_t temp[4096];
    for (int i = 0; i < width * height; i++) {
        int r, g, b;
        if (!ppm_number(&p, &r) || !ppm_number(&p, &g) || !ppm_number(&p, &b) || r > 255 ||
            g > 255 || b > 255)
            return false;
        temp[i] = (r << 16) | (g << 8) | b;
    }
    memcpy(image, temp, width * height * 4);
    image_w = width;
    image_h = height;
    return true;
}
