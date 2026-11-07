/* Browser surface for the Luna web engine: document lifecycle, painting,
 * scrolling and link activation. */
#include "webengine.h"

static WebNode *document_root;
static WebRule *style_rules;
static int style_rule_count;
static char document_url[256];
static char document_error[256];
static int scroll_offset;
static int layout_width;
static int viewport_height;
static size_t decoded_image_bytes;

/* The parsed stylesheet is kept in a growable buffer so that linked
 * stylesheets can be appended after the document was loaded. */
static char *sheet_text;
static size_t sheet_length, sheet_capacity;

static void sheet_append(const char *text, size_t len) {
    if (!len)
        return;
    if (sheet_length + len + 1 > sheet_capacity) {
        size_t grown = sheet_capacity ? sheet_capacity : 4096;
        while (grown < sheet_length + len + 1)
            grown *= 2;
        char *next = web_malloc(grown);
        if (sheet_length)
            memcpy(next, sheet_text, sheet_length);
        web_free(sheet_text);
        sheet_text = next;
        sheet_capacity = grown;
    }
    memcpy(sheet_text + sheet_length, text, len);
    sheet_length += len;
    sheet_text[sheet_length] = 0;
}
static void collect_style_elements(WebNode *node) {
    if (!node)
        return;
    if (node->type == WEB_ELEMENT && !strcmp(node->tag, "style")) {
        for (WebNode *c = node->first_child; c; c = c->next)
            if (c->type == WEB_TEXT && c->text)
                sheet_append(c->text, strlen(c->text));
        sheet_append("\n", 1);
    }
    for (WebNode *c = node->first_child; c; c = c->next)
        collect_style_elements(c);
}
/* Parses the collected stylesheet and applies it to the document again. */
static void styles_rebuild(void) {
    css_free_sheet(style_rules, style_rule_count);
    style_rules = 0;
    style_rule_count = 0;
    css_parse_sheet(sheet_text ? sheet_text : "", &style_rules, &style_rule_count);
    if (document_root) {
        css_apply(document_root, style_rules, style_rule_count);
        web_layout(document_root, layout_width, viewport_height);
    }
}
void web_add_stylesheet(const char *css, size_t len) {
    if (!css || !len)
        return;
    sheet_append(css, len);
    sheet_append("\n", 1);
    styles_rebuild();
}
/* True when the media attribute of a link element selects a screen. */
static bool media_selects_screen(const char *media) {
    if (!media || !*media)
        return true;
    if (!web_strcasecmp(media, "all") || !web_strcasecmp(media, "screen"))
        return true;
    if (!strncmp(media, "not", 3))
        return true;
    /* A list that names only print oriented media does not apply. */
    for (const char *p = media; *p; p++) {
        char token[32];
        size_t n = 0;
        while (*p && *p != ',' && n < sizeof(token) - 1)
            token[n++] = *p++;
        token[n] = 0;
        while (*p == ',')
            p++;
        char *t = token;
        while (*t == ' ')
            t++;
        if (!*t)
            continue;
        if (!web_strcasecmp(t, "all") || !web_strcasecmp(t, "screen"))
            return true;
    }
    return false;
}
static bool stylesheet_href(WebNode *node, int *index, char *out, size_t cap) {
    if (!node)
        return false;
    if (node->type == WEB_ELEMENT && !strcmp(node->tag, "link")) {
        const char *rel = web_attr(node, "rel");
        const char *href = web_attr(node, "href");
        if (rel && href) {
            bool is_stylesheet = false;
            const char *p = rel;
            while (*p) {
                while (*p == ' ' || *p == ',')
                    p++;
                const char *start = p;
                while (*p && *p != ' ' && *p != ',')
                    p++;
                if ((size_t)(p - start) == 10 && !strncmp(start, "stylesheet", 10))
                    is_stylesheet = true;
            }
            if (is_stylesheet && media_selects_screen(web_attr(node, "media"))) {
                if (*index == 0) {
                    size_t n = strlen(href);
                    if (n >= cap)
                        n = cap - 1;
                    memcpy(out, href, n);
                    out[n] = 0;
                    return true;
                }
                (*index)--;
            }
        }
    }
    for (WebNode *c = node->first_child; c; c = c->next)
        if (stylesheet_href(c, index, out, cap))
            return true;
    return false;
}
/* Returns the target of the index-th linked stylesheet of the document. */
bool web_stylesheet_link(int index, char *out, size_t cap) {
    if (!document_root || index < 0 || !out || !cap)
        return false;
    out[0] = 0;
    int wanted = index;
    return stylesheet_href(document_root, &wanted, out, cap);
}
static WebNode *image_node(WebNode *node, int *index) {
    if (!node || (node->type == WEB_ELEMENT && node->style.display == WEB_DISPLAY_NONE))
        return 0;
    if (node->type == WEB_ELEMENT && !strcmp(node->tag, "img") && web_attr(node, "src")) {
        if ((*index)-- == 0) return node;
    }
    for (WebNode *c = node->first_child; c; c = c->next) {
        WebNode *found = image_node(c, index);
        if (found) return found;
    }
    return 0;
}
bool web_image_link(int index, char *out, size_t cap) {
    if (index < 0 || !out || !cap) return false;
    WebNode *node = image_node(document_root, &index);
    if (!node) return false;
    const char *src = web_attr(node, "src");
    size_t n = strlen(src);
    if (n >= cap) return false;
    memcpy(out, src, n + 1);
    return true;
}
bool web_set_image(int index, const unsigned char *data, size_t len) {
    if (index < 0) return false;
    WebNode *node = image_node(document_root, &index);
    if (!node || decoded_image_bytes >= 1024 * 1024) return false;
    size_t old = (size_t)node->image_w * (size_t)node->image_h * 4;
    if (!web_decode_image(node, data, len)) return false;
    size_t bytes = (size_t)node->image_w * (size_t)node->image_h * 4;
    if (decoded_image_bytes - old + bytes > 1024 * 1024) {
        web_free(node->image);
        node->image = 0;
        node->image_w = node->image_h = 0;
        decoded_image_bytes -= old;
        return false;
    }
    decoded_image_bytes = decoded_image_bytes - old + bytes;
    web_relayout();
    return true;
}
void web_reset(void) {
    web_js_release();
    if (document_root)
        web_free_tree(document_root);
    document_root = 0;
    css_free_sheet(style_rules, style_rule_count);
    style_rules = 0;
    style_rule_count = 0;
    decoded_image_bytes = 0;
    sheet_length = 0;
    if (sheet_text)
        sheet_text[0] = 0;
    scroll_offset = 0;
    layout_width = 0;
    document_error[0] = 0;
    web_free_fragments();
}
void web_close(void) {
    web_reset();
    web_free(sheet_text);
    sheet_text=0; sheet_capacity=sheet_length=0;
    document_url[0]=0;
}
void web_relayout(void) {
    if (!document_root)
        return;
    css_apply(document_root, style_rules, style_rule_count);
    web_layout(document_root, layout_width, viewport_height);
}
bool web_load_document(const char *url, const char *html, size_t len, char *error, size_t ecap) {
    web_reset();
    if (url)
        strcpy(document_url, url);
    WebNode *root = 0;
    char parse_error[128];
    parse_error[0] = 0;
    if (!web_parse(html ? html : "", len, &root, parse_error, sizeof(parse_error)) || !root) {
        if (error)
            append(error, parse_error[0] ? parse_error : "The document could not be parsed.", ecap);
        return false;
    }
    document_root = root;
    sheet_length = 0;
    if (sheet_text)
        sheet_text[0] = 0;
    collect_style_elements(root);
    css_free_sheet(style_rules, style_rule_count);
    style_rules = 0;
    style_rule_count = 0;
    css_parse_sheet(sheet_text ? sheet_text : "", &style_rules, &style_rule_count);
    css_apply(document_root, style_rules, style_rule_count);
    char js_error[256];
    js_error[0] = 0;
    if (!web_js_run(document_root, js_error, sizeof(js_error)) && js_error[0]) {
        /* A script error is reported but the document stays on screen. */
        strcpy(document_error, js_error);
    }
    web_relayout();
    return true;
}
const char *web_document_url(void) { return document_url; }
const char *web_error_text(void) { return document_error; }
bool web_has_document(void) { return document_root != 0; }
int web_content_height(void) { return web_document_height(); }
int web_scroll_offset(void) { return scroll_offset; }
void web_scroll_by(int delta) {
    if (!document_root)
        return;
    int maximum = web_document_height() - viewport_height;
    if (maximum < 0)
        maximum = 0;
    scroll_offset += delta;
    if (scroll_offset < 0)
        scroll_offset = 0;
    if (scroll_offset > maximum)
        scroll_offset = maximum;
}
void web_render(int x, int y, int w, int h) {
    if (!document_root)
        return;
    viewport_height = h;
    if (w != layout_width) {
        layout_width = w;
        css_set_viewport(w);
        styles_rebuild();
    }
    if (web_js_dirty()) {
        web_js_clear_dirty();
        web_relayout();
    }
    rect(x, y, w, h, 0xffffff);
    web_paint(document_root, x, y - scroll_offset);
    if (web_document_height() > h) {
        int track = h - 4;
        int ratio = h * h / web_document_height();
        if (ratio < 24)
            ratio = 24;
        int thumb = track * ratio / h;
        int position = (web_document_height() - h) ? scroll_offset * (track - thumb) /
                                                         (web_document_height() - h)
                                                   : 0;
        rect(x + w - 6, y, 6, h, 0xe6ebf2);
        rect(x + w - 6, y + position, 6, thumb, 0x9fb2c8);
    }
}
/* Finds the deepest element with a link at the given document position. */
static WebNode *hit_link(WebNode *node, int x, int y) {
    if (!node || node->type != WEB_ELEMENT || node->style.display == WEB_DISPLAY_NONE ||
        node->style.hidden || node->style.clip_empty)
        return 0;
    bool inside = node->w > 0 && x >= node->x && x < node->x + node->w && y >= node->y &&
                  y < node->y + node->h;
    if (node->style.overflow_hidden && !inside)
        return 0;
    WebNode *found = 0;
    for (WebNode *c = node->first_child; c; c = c->next) {
        WebNode *hit = hit_link(c, x, y);
        if (hit)
            found = hit;
    }
    if (found)
        return found;
    if (inside && web_attr(node, "href"))
        return node;
    return 0;
}
bool web_click(int x, int y, char *href, size_t cap) {
    if (!document_root || !href || !cap)
        return false;
    href[0] = 0;
    WebNode *node = hit_link(document_root, x, y);
    if (!node)
        return false;
    web_js_dispatch(node, "click", document_root);
    const char *target = web_attr(node, "href");
    if (!target || !*target)
        return false;
    strcpy(href, target);
    return true;
}
bool web_key_press(int key) {
    if (!document_root)
        return false;
    if (key == 8 || key == 127)
        return false;
    return false;
}
