/* LunaOS native graphics callback implementation for litehtml.
 * No host window system or desktop toolkit is required.
 */
#include "webengine.h"
#include "litehtml_bridge.h"

static int lw(void *p, const char *str, int size) {
    (void)p;
    return text_width(str, web_font_size(size));
}
static void lt(void *p, int x, int y, const char *str, int size, uint32_t color) {
    LunaLiteSurface *surface = p;
    text(surface->offset_x + x, surface->offset_y + y, str,
         web_font_size(size), color);
}
static void lr(void *p, int x, int y, int w, int h, uint32_t color) {
    LunaLiteSurface *surface = p;
    if (w > 0 && h > 0)
        rect(surface->offset_x + x, surface->offset_y + y, w, h, color);
}
static const WebNode *find_image(const WebNode *node, const char *url) {
    const char *src = node->image ? web_attr(node, "src") : 0;
    if (src && !strcmp(src, url))
        return node;
    for (const WebNode *child = node->first_child; child; child = child->next) {
        const WebNode *image = find_image(child, url);
        if (image)
            return image;
    }
    return 0;
}
static void ls(void *p, const char *url, int *w, int *h) {
    LunaLiteSurface *surface = p;
    const WebNode *image = find_image(surface->root, url);
    if (image) {
        *w = image->image_w;
        *h = image->image_h;
    }
}
static void li(void *p, int x, int y, int w, int h, const char *url) {
    LunaLiteSurface *surface = p;
    const WebNode *image = find_image(surface->root, url);
    if (image && w > 0 && h > 0)
        gfx_rgba_image(surface->offset_x + x, surface->offset_y + y, w, h,
                       image->image, image->image_w, image->image_h);
}
static void lc(void *p, int x, int y, int w, int h) {
    LunaLiteSurface *s = p;
    if (s->clip_depth >= 16) {
        s->clip_depth++;
        return;
    }
    int *saved = s->clips[s->clip_depth++];
    gfx_get_clip(saved);
    int left = s->offset_x + x, top = s->offset_y + y;
    int right = left + w, bottom = top + h;
    if (left < saved[0]) left = saved[0];
    if (top < saved[1]) top = saved[1];
    if (right > saved[0] + saved[2]) right = saved[0] + saved[2];
    if (bottom > saved[1] + saved[3]) bottom = saved[1] + saved[3];
    gfx_clip(left, top, right > left ? right - left : 0,
             bottom > top ? bottom - top : 0);
}
static void lu(void *p) {
    LunaLiteSurface *s = p;
    if (!s->clip_depth)
        return;
    if (--s->clip_depth >= 16)
        return;
    int *saved = s->clips[s->clip_depth];
    gfx_clip(saved[0], saved[1], saved[2], saved[3]);
}
void web_litehtml_graphics(LunaLiteGraphics *g, LunaLiteSurface *s, const WebNode *root) {
    memset(g, 0, sizeof(*g));
    memset(s, 0, sizeof(*s));
    g->context = s;
    g->text_width = lw;
    g->draw_text = lt;
    g->fill_rect = lr;
    g->image_size = ls;
    g->draw_image = li;
    g->push_clip = lc;
    g->pop_clip = lu;
    s->root = root;
}
void web_litehtml_offset(LunaLiteSurface *s, int x, int y) {
    s->offset_x = x;
    s->offset_y = y;
}
