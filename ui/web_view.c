#include "perf.h"
/* Browser surface for the Luna web engine: document lifecycle, painting,
 * scrolling and link activation. */
#include "webengine.h"
#ifdef LUNA_USE_LITEHTML
#include "litehtml_bridge.h"
static LunaLiteDocument *litehtml_document;
static bool litehtml_failed;
static LunaLiteSurface litehtml_surface;
static LunaLiteGraphics litehtml_graphics;
#endif


static WebNode *document_root;
static WebRule *style_rules;
static int style_rule_count;
static char document_url[256];
static char document_error[256];
static int scroll_offset;
static bool styles_pending, images_pending;
static int layout_width;
static int viewport_height;
static size_t decoded_image_bytes;

/* The parsed stylesheet is kept in a growable buffer so that linked
 * stylesheets can be appended after the document was loaded. */
static char *sheet_text;
static size_t sheet_length, sheet_capacity;
static void relayout(bool rebuild_renderer);

typedef struct WebStylesheet {
    struct WebStylesheet *next;
    char *href, *text;
} WebStylesheet;
static WebStylesheet *linked_styles;
static WebStylesheet **linked_styles_tail = &linked_styles;

#ifdef LUNA_USE_LITEHTML
static const char *litehtml_stylesheet(void *context, const char *href) {
    (void)context;
    for (WebStylesheet *sheet = linked_styles; sheet; sheet = sheet->next)
        if (!strcmp(sheet->href, href))
            return sheet->text;
    return 0;
}

static void litehtml_disable(void) {
    luna_lite_destroy(litehtml_document);
    litehtml_document = 0;
    litehtml_failed = true;
}

static void litehtml_rebuild(void) {
    luna_lite_destroy(litehtml_document);
    web_litehtml_graphics(&litehtml_graphics, &litehtml_surface, document_root);
    litehtml_graphics.stylesheet = litehtml_stylesheet;
    litehtml_document = luna_lite_create(document_root, &litehtml_graphics,
                                         layout_width, viewport_height);
    if (!litehtml_document)
        litehtml_failed = true;
}
#endif

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
static void styles_rebuild(bool rebuild_renderer) {
    sheet_length = 0;
    if (sheet_text)
        sheet_text[0] = 0;
    collect_style_elements(document_root);
    for (WebStylesheet *sheet = linked_styles; sheet; sheet = sheet->next) {
        sheet_append(sheet->text, strlen(sheet->text));
        sheet_append("\n", 1);
    }
    css_free_sheet(style_rules, style_rule_count);
    style_rules = 0;
    style_rule_count = 0;
    css_parse_sheet(sheet_text ? sheet_text : "", &style_rules, &style_rule_count);
    relayout(rebuild_renderer);
}
void web_add_stylesheet(const char *href, const char *css, size_t len) {
    if (!href || !css || !len)
        return;
    WebStylesheet *sheet = web_malloc(sizeof(*sheet));
    sheet->href = web_strdup(href);
    sheet->text = web_strndup(css, len);
    sheet->next = 0;
    *linked_styles_tail = sheet;
    linked_styles_tail = &sheet->next;
    styles_pending = true;
}
void web_finish_resources(void) {
    if (styles_pending)
        styles_rebuild(true);
    else if (images_pending)
        relayout(false);
    styles_pending = images_pending = false;
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
    images_pending = true;
    return true;
}
void web_reset(void) {
#ifdef LUNA_USE_LITEHTML
    if (litehtml_document) {
        luna_lite_destroy(litehtml_document);
        litehtml_document = 0;
    }
    litehtml_failed = false;
#endif
    while (linked_styles) {
        WebStylesheet *sheet = linked_styles;
        linked_styles = sheet->next;
        web_free(sheet->href);
        web_free(sheet->text);
        web_free(sheet);
    }
    linked_styles_tail = &linked_styles;
    web_js_release();
    web_js_clear_dirty();
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
    styles_pending = images_pending = false;
    document_error[0] = 0;
    web_free_fragments();
}
void web_close(void) {
    web_reset();
    web_free(sheet_text);
    sheet_text=0; sheet_capacity=sheet_length=0;
    document_url[0]=0;
}
static void relayout(bool rebuild_renderer) {
    if (!document_root)
        return;
    PerfArea previous = perf_enter(PERF_WEB_LAYOUT);
    css_apply(document_root, style_rules, style_rule_count);
    web_layout(document_root, layout_width, viewport_height);
#ifdef LUNA_USE_LITEHTML
    if (!litehtml_failed && layout_width > 0 && viewport_height > 0) {
        if (rebuild_renderer || !litehtml_document)
            litehtml_rebuild();
        else if (luna_lite_layout(litehtml_document, layout_width, viewport_height) < 0)
            litehtml_disable();
    }
#else
    (void)rebuild_renderer;
#endif
    perf_leave(previous);
}
void web_set_viewport(int width, int height) {
    layout_width = width;
    viewport_height = height;
    css_set_viewport(width);
}
/* The parser only reads UTF-8. A document whose bytes are not valid UTF-8 is
 * read as windows-1252, the way a browser treats such a page before parsing
 * it, so accented text survives instead of turning into a question mark. */
static const unsigned short cp1252_high[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
};
static bool utf8_valid(const char *text, size_t len) {
    const unsigned char *p = (const unsigned char *)text;
    size_t i = 0;
    while (i < len) {
        unsigned c = p[i];
        int extra;
        if (c < 0x80) {
            i++;
            continue;
        } else if ((c & 0xe0) == 0xc0) {
            extra = 1;
            if (c < 0xc2)
                return false;
        } else if ((c & 0xf0) == 0xe0) {
            extra = 2;
            if (c == 0xe0 && p[i + 1] < 0xa0)
                return false;
        } else if ((c & 0xf8) == 0xf0) {
            extra = 3;
            if (c > 0xf4 || (c == 0xf0 && p[i + 1] < 0x90))
                return false;
        } else {
            return false;
        }
        if (i + (size_t)extra >= len)
            return false;
        for (int k = 1; k <= extra; k++)
            if ((p[i + k] & 0xc0) != 0x80)
                return false;
        i += (size_t)extra + 1;
    }
    return true;
}
static size_t windows1252_to_utf8(const char *text, size_t len, char *out) {
    const unsigned char *p = (const unsigned char *)text;
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned c = p[i];
        unsigned cp = (c >= 0x80 && c < 0xa0) ? cp1252_high[c - 0x80] : c;
        if (cp < 0x80) {
            out[n++] = (char)cp;
        } else if (cp < 0x800) {
            out[n++] = (char)(0xc0 | (cp >> 6));
            out[n++] = (char)(0x80 | (cp & 63));
        } else {
            out[n++] = (char)(0xe0 | (cp >> 12));
            out[n++] = (char)(0x80 | ((cp >> 6) & 63));
            out[n++] = (char)(0x80 | (cp & 63));
        }
    }
    out[n] = 0;
    return n;
}
bool web_load_document(const char *url, const char *html, size_t len, char *error, size_t ecap) {
    web_reset();
    if (url)
        strcpy(document_url, url);
    WebNode *root = 0;
    char parse_error[128];
    parse_error[0] = 0;
    const char *text = html ? html : "";
    size_t size = len;
    char *converted = 0;
    if (size && !utf8_valid(text, size)) {
        converted = web_malloc(size * 3 + 1);
        if (converted)
            size = windows1252_to_utf8(text, size, converted);
    }
    if (converted)
        text = converted;
    bool parsed = web_parse(text, size, &root, parse_error, sizeof(parse_error)) && root;
    if (converted)
        web_free(converted);
    if (!parsed) {
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
    web_js_clear_dirty();
    styles_rebuild(true);
    return true;
}
const char *web_document_url(void) { return document_url; }
const char *web_error_text(void) { return document_error; }
bool web_has_document(void) { return document_root != 0; }
int web_content_height(void) {
#ifdef LUNA_USE_LITEHTML
    if (litehtml_document)
        return luna_lite_document_height(litehtml_document);
#endif
    return web_document_height();
}
int web_scroll_offset(void) { return scroll_offset; }
bool web_scroll_by(int delta) {
    if (!document_root)
        return false;
    int previous = scroll_offset;
    int maximum = web_content_height() - viewport_height;
    if (maximum < 0)
        maximum = 0;
    scroll_offset += delta;
    if (scroll_offset < 0)
        scroll_offset = 0;
    if (scroll_offset > maximum)
        scroll_offset = maximum;
    return previous != scroll_offset;
}
void web_render(int x, int y, int w, int h) {
    if (!document_root)
        return;
    if (w != layout_width || h != viewport_height) {
        web_set_viewport(w, h);
        styles_rebuild(false);
    }
    if (web_js_dirty()) {
        web_js_clear_dirty();
        styles_rebuild(true);
    }
    PerfArea previous = perf_enter(PERF_WEB_PAINT);
    int saved[4];
    gfx_get_clip(saved);
    int left = x > saved[0] ? x : saved[0], top = y > saved[1] ? y : saved[1];
    int right = x + w < saved[0] + saved[2] ? x + w : saved[0] + saved[2];
    int bottom = y + h < saved[1] + saved[3] ? y + h : saved[1] + saved[3];
    gfx_clip(left, top, right > left ? right - left : 0, bottom > top ? bottom - top : 0);
    rect(x, y, w, h, 0xffffff);
#ifdef LUNA_USE_LITEHTML
    if (litehtml_document) {
        web_litehtml_offset(&litehtml_surface, x, y);
        if (!luna_lite_draw(litehtml_document, 0, -scroll_offset)) {
            litehtml_disable();
            gfx_clip(left, top, right > left ? right - left : 0, bottom > top ? bottom - top : 0);
            rect(x, y, w, h, 0xffffff);
        }
    }
    if (!litehtml_document)
#endif
        web_paint(document_root, x, y - scroll_offset);
    gfx_clip(saved[0], saved[1], saved[2], saved[3]);
    perf_leave(previous);
    if (web_content_height() > h) {
        int track = h - 4;
        int ratio = h * h / web_content_height();
        if (ratio < 24)
            ratio = 24;
        int thumb = track * ratio / h;
        int position = (web_content_height() - h) ? scroll_offset * (track - thumb) /
                                                         (web_content_height() - h)
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
    if (web_js_dirty()) {
        web_js_clear_dirty();
        styles_rebuild(true);
    }
    WebNode *node;
#ifdef LUNA_USE_LITEHTML
    if (litehtml_document)
        node = luna_lite_hit_link(litehtml_document, x, y, scroll_offset);
    else
#endif
        node = hit_link(document_root, x, y);
    if (!node)
        return false;
    web_js_dispatch(node, "click", document_root);
    const char *target = web_attr(node, "href");
    if (!target || !*target || strlen(target) >= cap)
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
