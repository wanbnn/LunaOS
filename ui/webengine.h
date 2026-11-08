#ifndef LUNA_WEBENGINE_H
#define LUNA_WEBENGINE_H
#include "luna.h"

/* Native web engine for the Luna Browser: HTML parser, CSS cascade, box
 * layout, painting and a DOM binding for the embedded QuickJS runtime.
 * The engine is a kernel service like the other application backends. */

void *web_malloc(size_t n);
void web_free(void *p);
char *web_strdup(const char *s);
char *web_strndup(const char *s, size_t n);
int web_strcasecmp(const char *a, const char *b);

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

typedef struct WebFragment {
    int x, y;
    char *text;
    int size;
    uint32_t color;
    bool underline;
    int next;
} WebFragment;

/* dom */
bool web_parse(const char *html, size_t len, WebNode **root, char *error, size_t ecap);
void web_free_tree(WebNode *node);
WebNode *web_node_new(unsigned char type);
void web_node_append(WebNode *parent, WebNode *child);
bool web_node_contains(const WebNode *ancestor, const WebNode *node);
void web_node_detach(WebNode *node);
WebNode *web_node_clone(const WebNode *source, bool deep);
WebNode *web_attach_shadow(WebNode *host, bool open_mode);
bool web_node_insert_before(WebNode *parent, WebNode *child, WebNode *reference);
WebNode *web_node_root(WebNode *node);
bool web_node_is_connected(const WebNode *node);
bool web_node_replace_child(WebNode *parent, WebNode *replacement, WebNode *old_child);
void web_node_normalize(WebNode *parent);
const char *web_attr(const WebNode *node, const char *name);
void web_set_attr(WebNode *node, const char *name, const char *value);
bool web_attr_has(const WebNode *node, const char *name);
void web_node_text(WebNode *node, char *out, size_t cap);
WebNode *web_find_by_id(WebNode *root, const char *id);
WebNode *web_find_first(WebNode *root, const char *tag);
bool web_is_void_tag(const char *tag);

/* css */
typedef struct WebRule WebRule;
void css_set_viewport(int width);
bool css_media_matches(const char *query);
bool css_parse_sheet(const char *text, WebRule **rules, int *count);
void css_free_sheet(WebRule *rules, int count);
void css_apply(WebNode *root, WebRule *rules, int count);
void css_set_property(WebNode *node, const char *name, const char *value);
void css_get_property(WebNode *node, const char *name, char *out, size_t cap);
void css_remove_property(WebNode *node, const char *name);
bool css_match_selector(const char *selector, WebNode *element);
int css_color(const char *value, uint32_t *out);
int css_length(const char *value, int font_size, int percent_out, bool *is_percent);

/* layout and paint */
int web_font_size(int css_px);
void web_layout(WebNode *root, int width, int height);
void web_paint(WebNode *root, int ox, int oy);
int web_document_height(void);
void web_free_fragments(void);

/* script engine */
bool web_js_run(WebNode *root, char *error, size_t ecap);
void web_js_dispatch(WebNode *target, const char *type, WebNode *root);
void web_js_pump(void);
bool web_js_dirty(void);
void web_js_clear_dirty(void);
void web_js_release(void);

/* browser surface */
void web_reset(void);
void web_close(void);
void web_relayout(void);
bool web_load_document(const char *url, const char *html, size_t len, char *error, size_t ecap);
bool web_decode_image(WebNode *node, const unsigned char *data, size_t len);
bool web_image_link(int index, char *out, size_t cap);
bool web_set_image(int index, const unsigned char *data, size_t len);
void web_add_stylesheet(const char *css, size_t len);
bool web_stylesheet_link(int index, char *out, size_t cap);
void web_render(int x, int y, int w, int h);
int web_content_height(void);
bool web_scroll_by(int delta);
void web_finish_resources(void);
void web_set_viewport(int width, int height);
int web_scroll_offset(void);
bool web_click(int x, int y, char *href, size_t cap);
bool web_has_document(void);
const char *web_error_text(void);
const char *web_document_url(void);

#endif
