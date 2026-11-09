#ifndef LUNA_WEBENGINE_H
#define LUNA_WEBENGINE_H
#include "luna.h"
#include "web_dom_types.h"

/* Native web engine for the Luna Browser: HTML parser, CSS cascade, box
 * layout, painting and a DOM binding for the embedded QuickJS runtime.
 * The engine is a kernel service like the other application backends. */

void *web_malloc(size_t n);
void web_free(void *p);
char *web_strdup(const char *s);
char *web_strndup(const char *s, size_t n);
int web_strcasecmp(const char *a, const char *b);

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
WebNode *web_parse_fragment(const char *tag, const char *html, size_t len);
void web_free_tree(WebNode *node);
WebNode *web_node_new(unsigned char type);
void web_node_append(WebNode *parent, WebNode *child);
bool web_node_contains(const WebNode *ancestor, const WebNode *node);
void web_node_detach(WebNode *node);
WebNode *web_node_clone(const WebNode *source, bool deep);
WebNode *web_attach_shadow(WebNode *host, bool open_mode);
bool web_node_insert_before(WebNode *parent, WebNode *child, WebNode *reference);
WebNode *web_node_root(WebNode *node);
WebNode *web_node_root_composed(WebNode *node);
bool web_node_is_connected(const WebNode *node);
bool web_node_replace_child(WebNode *parent, WebNode *replacement, WebNode *old_child);
void web_node_normalize(WebNode *parent);
const char *web_attr(const WebNode *node, const char *name);
void web_set_attr(WebNode *node, const char *name, const char *value);
bool web_attr_has(const WebNode *node, const char *name);
void web_node_text(WebNode *node, char *out, size_t cap);
char *web_convert_legacy_text(const char *text, size_t *length);
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
const char *css_pseudo_property(WebRule *, int count, WebNode *, const char *pseudo, const char *name);
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
void web_js_finish_document(void);
bool web_js_timer_ready(void);
void web_js_viewport_changed(void);
void web_js_dispatch(WebNode *target, const char *type, WebNode *root);
void web_js_pump(void);
bool web_js_dirty(void);
void web_js_clear_dirty(void);
void web_js_release(void);

/* browser surface */
void web_reset(void);
void web_close(void);
bool web_load_document(const char *url, const char *html, size_t len, char *error, size_t ecap);
bool web_decode_image(WebNode *node, const unsigned char *data, size_t len);
bool web_image_link(int index, char *out, size_t cap);
bool web_set_image(int index, const unsigned char *data, size_t len);
void web_add_stylesheet(const char *href, const char *css, size_t len);
bool web_stylesheet_link(int index, char *out, size_t cap);
void web_render(int x, int y, int w, int h);
int web_content_height(void);
bool web_scroll_by(int delta);
void web_finish_resources(void);
void web_set_viewport(int width, int height);
int web_scroll_offset(void);
bool web_click(int x, int y, char *href, size_t cap);
bool web_has_document(void);
bool web_document_ready(void);
const char *web_error_text(void);
const char *web_document_url(void);
const char *web_pseudo_property(WebNode *, const char *pseudo, const char *name);

#endif
