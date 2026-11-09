#ifndef LUNA_LITEHTML_BRIDGE_H
#define LUNA_LITEHTML_BRIDGE_H
/* C ABI between Luna's freestanding graphics and the C++17 litehtml engine.
 * Host and guest use the same drawing interface. No C++ types cross it. */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct LunaLiteDocument LunaLiteDocument;
struct WebNode;
typedef struct {
    int offset_x, offset_y;
    int clip_depth;
    int clips[16][4];
    const struct WebNode *root;
} LunaLiteSurface;
typedef struct {
    void *context;
    int (*text_width)(void *, const char *, int);
    void (*draw_text)(void *, int, int, const char *, int, uint32_t);
    void (*fill_rect)(void *, int, int, int, int, uint32_t);
    void (*image_size)(void *, const char *, int *, int *);
    void (*draw_image)(void *, int, int, int, int, const char *);
    const char *(*stylesheet)(void *, const char *);
    void (*push_clip)(void *, int, int, int, int);
    void (*pop_clip)(void *);
} LunaLiteGraphics;
/* An explicit callback table avoids linking against the kernel on host. */
LunaLiteDocument *luna_lite_create(struct WebNode *root, const LunaLiteGraphics *graphics,
                                   int width, int height);
void luna_lite_destroy(LunaLiteDocument *document);
int luna_lite_layout(LunaLiteDocument *document, int width, int height);
bool luna_lite_draw(LunaLiteDocument *document, int x, int y);
int luna_lite_document_height(const LunaLiteDocument *document);
struct WebNode *luna_lite_hit_link(LunaLiteDocument *document, int x, int y, int scroll);
void web_litehtml_graphics(LunaLiteGraphics *graphics, LunaLiteSurface *surface,
                          const struct WebNode *root);
void web_litehtml_offset(LunaLiteSurface *surface, int x, int y);
#ifdef __cplusplus
}
#endif
#endif
