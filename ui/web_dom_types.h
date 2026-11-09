#ifndef LUNA_WEB_DOM_TYPES_H
#define LUNA_WEB_DOM_TYPES_H

#include <stdbool.h>
#include <stdint.h>

enum { WEB_ELEMENT, WEB_TEXT, WEB_COMMENT, WEB_DOCUMENT_FRAGMENT, WEB_SHADOW_ROOT };
enum { WEB_NS_HTML = 0, WEB_NS_SVG = 1, WEB_NS_MATHML = 2 };
#define WEB_AUTO (-9999)
enum { WEB_DISPLAY_BLOCK, WEB_DISPLAY_INLINE, WEB_DISPLAY_INLINE_BLOCK, WEB_DISPLAY_NONE };
enum { WEB_ALIGN_LEFT, WEB_ALIGN_CENTER, WEB_ALIGN_RIGHT };
enum { WEB_MARKER_NONE, WEB_MARKER_DISC, WEB_MARKER_CIRCLE, WEB_MARKER_SQUARE, WEB_MARKER_DECIMAL };

typedef struct WebAttr {
    struct WebAttr *next;
    char *name, *value;
} WebAttr;

typedef struct {
    uint32_t color, background;
    int font_size, font_weight, line_height;
    int display, text_align;
    int margin[4], padding[4], border[4];
    uint32_t border_color[4];
    bool border_shown[4];
    int width, height;
    int width_pct, height_pct;
    bool underline, hidden;
    int list_marker;
    int floating, clear, position; /* 0=normal; floats/clear: 1=left,2=right,3=both */
    int inset[4];
    int min_width, max_width, min_height, max_height;
    int min_width_pct, max_width_pct;
    bool border_box, overflow_hidden, clip_empty, background_set;
    int white_space; /* 0=normal,1=pre,2=pre-wrap,3=nowrap */
} WebStyle;

typedef struct WebNode {
    unsigned char type;
    unsigned char namespace_id; /* HTML, SVG, or MathML, not inferred from case */
    char *tag;
    char *text;
    WebAttr *attrs;
    struct WebNode *parent, *first_child, *last_child, *next, *prev;
    struct WebNode *template_content, *shadow_root, *host;
    bool shadow_open;
    WebStyle style;
    bool styled;
    int x, y, w, h;
    int content_x, content_y, content_w, content_h;
    void *js;
    unsigned char *image;
    int image_w, image_h;
    uint64_t fragment_epoch;
    int first_fragment, last_fragment;
} WebNode;

#endif
