/* Box layout for the Luna Browser engine: block flow, inline line boxes and
 * word wrapping over the embedded bitmap fonts. */
#include "webengine.h"

typedef struct {
    WebNode *node;
    int offset, len;
    int size;
    uint32_t color;
    bool underline;
    bool bold;
    WebNode *owner;
    bool atomic;
    WebNode *atomic_node;
    int width, height;
    bool space_before;
} WebInlineItem;

static WebFragment *fragments;
static int fragment_count, fragment_capacity;
static WebInlineItem *items;
static int item_count, item_capacity;
static int layout_document_height;
static char *run_buffer;
static int run_length, run_capacity;

static int layout_block(WebNode *node, int x, int y, int avail_width, bool shrink);
static int measure_natural(WebNode *node);

int web_font_size(int css_px) {
    static const int sizes[] = {13, 16, 20, 28, 48};
    int best = 16;
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        int distance = sizes[i] - css_px;
        if (distance < 0)
            distance = -distance;
        int best_distance = best - css_px;
        if (best_distance < 0)
            best_distance = -best_distance;
        if (distance < best_distance)
            best = sizes[i];
    }
    return best;
}

static void fragments_reset(void) {
    fragment_count = 0;
    run_length = 0;
}
static void fragment_add(int x, int y, const char *text, int len, int size, uint32_t color,
                         bool underline) {
    if (fragment_count >= fragment_capacity) {
        int grown = fragment_capacity ? fragment_capacity * 2 : 256;
        WebFragment *next = web_malloc(sizeof(WebFragment) * (size_t)grown);
        if (fragment_count)
            memcpy(next, fragments, sizeof(WebFragment) * (size_t)fragment_count);
        web_free(fragments);
        fragments = next;
        fragment_capacity = grown;
    }
    WebFragment *f = &fragments[fragment_count++];
    f->x = x;
    f->y = y;
    f->size = size;
    f->color = color;
    f->underline = underline;
    f->text = web_strndup(text, (size_t)len);
}
static void run_append(const char *text, int len) {
    if (run_length + len + 1 > run_capacity) {
        int grown = run_capacity ? run_capacity * 2 : 512;
        while (grown < run_length + len + 1)
            grown *= 2;
        char *next = web_malloc((size_t)grown);
        if (run_length)
            memcpy(next, run_buffer, (size_t)run_length);
        web_free(run_buffer);
        run_buffer = next;
        run_capacity = grown;
    }
    memcpy(run_buffer + run_length, text, (size_t)len);
    run_length += len;
    run_buffer[run_length] = 0;
}
static int measure_text(const char *s, int len, int size) {
    char buf[256];
    if (len > 255)
        len = 255;
    if (len <= 0)
        return 0;
    memcpy(buf, s, (size_t)len);
    buf[len] = 0;
    return text_width(buf, size);
}
static int item_add(void) {
    if (item_count >= item_capacity) {
        int grown = item_capacity ? item_capacity * 2 : 128;
        WebInlineItem *next = web_malloc(sizeof(WebInlineItem) * (size_t)grown);
        if (item_count)
            memcpy(next, items, sizeof(WebInlineItem) * (size_t)item_count);
        web_free(items);
        items = next;
        item_capacity = grown;
    }
    item_count++;
    return item_count - 1;
}

/* Splits a text node into word items using the given inline style. */
static void collect_words(WebNode *node, int size, uint32_t color, bool underline, bool bold,
                          WebNode *owner) {
    const char *text = node->text;
    if (!text)
        return;
    size = web_font_size(size);
    int length = (int)strlen(text);
    int i = 0;
    bool pending_space = false;
    while (i < length) {
        while (i < length && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' ||
                              text[i] == '\r')) {
            pending_space = true;
            i++;
        }
        if (i >= length)
            break;
        int start = i;
        while (i < length && text[i] != ' ' && text[i] != '\t' && text[i] != '\n' &&
               text[i] != '\r')
            i++;
        int end = i;
        while (end - start > 200) {
            int index = item_add();
            WebInlineItem *item = &items[index];
            memset(item, 0, sizeof(*item));
            item->node = node;
            item->offset = start;
            item->len = 200;
            item->size = size;
            item->color = color;
            item->underline = underline;
            item->bold = bold;
            item->owner = owner;
            item->space_before = pending_space;
            pending_space = false;
            start += 200;
        }
        int index = item_add();
        WebInlineItem *item = &items[index];
        memset(item, 0, sizeof(*item));
        item->node = node;
        item->offset = start;
        item->len = end - start;
        item->size = size;
        item->color = color;
        item->underline = underline;
        item->bold = bold;
        item->owner = owner;
        item->space_before = pending_space;
        pending_space = false;
    }
}
static void collect_inline(WebNode *node, WebStyle *style, WebNode *owner, int avail_width) {
    if (!node)
        return;
    if (node->type == WEB_TEXT) {
        collect_words(node, style->font_size, style->color, style->underline,
                      style->font_weight >= 600, owner);
        return;
    }
    if (node->type != WEB_ELEMENT)
        return;
    if (node->style.display == WEB_DISPLAY_NONE)
        return;
    if (node->style.display == WEB_DISPLAY_INLINE_BLOCK) {
        int index = item_add();
        WebInlineItem *item = &items[index];
        memset(item, 0, sizeof(*item));
        item->atomic = true;
        item->atomic_node = node;
        item->size = web_font_size(node->style.font_size);
        item->color = node->style.color;
        item->owner = node;
        WebStyle *s = &node->style;
        int natural = measure_natural(node) + s->border[1] + s->border[3] + s->padding[3] +
                      s->padding[1];
        item->width = natural < avail_width ? natural : avail_width;
        item->height = s->font_size + 7;
        return;
    }
    WebStyle merged = *style;
    merged.font_size = node->style.font_size;
    merged.color = node->style.color;
    merged.font_weight = node->style.font_weight;
    merged.underline = node->style.underline;
    node->w = 0;
    node->h = 0;
    for (WebNode *c = node->first_child; c; c = c->next)
        collect_inline(c, &merged, node, avail_width);
}


/* Lays out a run of inline items into line boxes. Returns the used height. */
static int flush_inline(int x, int y, int width, int from, int to) {
    int line_y = y;
    int line_x = x;
    int line_height = 0;
    int run_size = 0;
    uint32_t run_color = 0;
    bool run_underline = false;
    bool run_active = false;
    int run_x = x;
    for (int i = from; i < to; i++) {
        WebInlineItem *item = &items[i];
        if (item->atomic) {
            if (run_active) {
                fragment_add(run_x, line_y + (line_height - run_size) / 2, run_buffer, run_length,
                             run_size, run_color, run_underline);
                run_active = false;
                run_length = 0;
            }
            if (line_x > x && line_x + item->width > x + width) {
                line_y += line_height;
                line_height = 0;
                line_x = x;
            }
            int remaining = x + width - line_x;
            layout_block(item->atomic_node, line_x, line_y, remaining, true);
            line_x += item->atomic_node->w;
            if (item->atomic_node->h > line_height)
                line_height = item->atomic_node->h;
            continue;
        }
        int item_width = measure_text(item->node->text + item->offset, item->len, item->size);
        int item_height = item->size + 7;
        int space_width =
            item->space_before ? measure_text(" ", 1, item->size) : 0;
        bool line_has_content = line_x > x;
        if (line_has_content && line_x + space_width + item_width > x + width) {
            if (run_active)
                fragment_add(run_x, line_y + (line_height - run_size) / 2, run_buffer, run_length,
                             run_size, run_color, run_underline);
            run_active = false;
            run_length = 0;
            line_y += line_height;
            line_height = 0;
            line_x = x;
            space_width = 0;
        }
        if (!run_active) {
            run_active = true;
            run_x = line_x;
            run_size = item->size;
            run_color = item->color;
            run_underline = item->underline;
            run_length = 0;
        } else if (item->size != run_size || item->color != run_color ||
                   item->underline != run_underline) {
            fragment_add(run_x, line_y + (line_height - run_size) / 2, run_buffer, run_length,
                         run_size, run_color, run_underline);
            run_length = 0;
            run_x = line_x;
            run_size = item->size;
            run_color = item->color;
            run_underline = item->underline;
        }
        if (space_width) {
            run_append(" ", 1);
            line_x += space_width;
        }
        run_append(item->node->text + item->offset, item->len);
        if (item->owner && item->owner->type == WEB_ELEMENT) {
            WebNode *owner = item->owner;
            int text_top = line_y + (line_height - item->size) / 2;
            if (!owner->w) {
                owner->x = line_x;
                owner->y = text_top;
                owner->w = item_width;
                owner->h = item_height;
            } else {
                if (line_x < owner->x)
                    owner->x = line_x;
                if (text_top < owner->y)
                    owner->y = text_top;
                int right = line_x + item_width, bottom = text_top + item_height;
                if (right > owner->x + owner->w)
                    owner->w = right - owner->x;
                if (bottom > owner->y + owner->h)
                    owner->h = bottom - owner->y;
            }
        }
        line_x += item_width;
        if (item_height > line_height)
            line_height = item_height;
    }
    if (run_active)
        fragment_add(run_x, line_y + (line_height - run_size) / 2, run_buffer, run_length, run_size,
                     run_color, run_underline);
    run_length = 0;
    if (!line_height)
        return 0;
    return line_y + line_height - y;
}

/* Natural width of an inline subtree, used for shrink to fit boxes. */
static int measure_natural(WebNode *node) {
    if (!node)
        return 0;
    if (node->type == WEB_TEXT) {
        int size = node->style.font_size ? node->style.font_size : 16;
        return measure_text(node->text, (int)strlen(node->text), web_font_size(size));
    }
    if (node->type != WEB_ELEMENT || node->style.display == WEB_DISPLAY_NONE)
        return 0;
    int total = 0;
    for (WebNode *c = node->first_child; c; c = c->next) {
        int width = measure_natural(c);
        total += width;
        if (c->next)
            total += measure_text(" ", 1, web_font_size(node->style.font_size));
    }
    return total;
}

static int layout_block(WebNode *node, int x, int y, int avail_width, bool shrink) {
    WebStyle *s = &node->style;
    if (s->display == WEB_DISPLAY_NONE || s->hidden) {
        node->x = x;
        node->y = y;
        node->w = node->h = 0;
        node->content_x = x;
        node->content_y = y;
        node->content_w = node->content_h = 0;
        return 0;
    }
    int margin_top = s->margin[0] == WEB_AUTO ? 0 : s->margin[0];
    int margin_bottom = s->margin[2] == WEB_AUTO ? 0 : s->margin[2];
    bool auto_left = s->margin[1] == WEB_AUTO;
    bool auto_right = s->margin[3] == WEB_AUTO;
    int margin_left = auto_left ? 0 : s->margin[1];
    int margin_right = auto_right ? 0 : s->margin[3];
    int border_top = s->border[0], border_bottom = s->border[2];
    int border_left = s->border[1], border_right = s->border[3];
    int padding_top = s->padding[0], padding_bottom = s->padding[2];
    int padding_left = s->padding[3], padding_right = s->padding[1];
    int width;
    if (s->width >= 0)
        width = s->width;
    else if (s->width_pct >= 0)
        width = avail_width * s->width_pct / 100;
    else if (shrink) {
        int natural =
            measure_natural(node) + border_left + border_right + padding_left + padding_right;
        width = natural < avail_width ? natural : avail_width;
    } else
        width = avail_width - margin_left - margin_right;
    if (width < 0)
        width = 0;
    int content_width = width - border_left - border_right - padding_left - padding_right;
    if (content_width < 0)
        content_width = 0;
    int box_x = x + margin_left;
    if (auto_left && auto_right)
        box_x = x + (avail_width - width) / 2;
    int box_y = y + margin_top;
    node->x = box_x;
    node->y = box_y;
    node->w = width;
    node->content_x = box_x + border_left + padding_left;
    node->content_y = box_y + border_top + padding_top;
    node->content_w = content_width;
    int cursor_y = node->content_y;
    int bottom = node->content_y;
    int pending_from = item_count;
    WebNode *child = node->first_child;
    while (child) {
        if (child->type == WEB_ELEMENT && child->style.display == WEB_DISPLAY_BLOCK) {
            if (item_count > pending_from) {
                int used =
                    flush_inline(node->content_x, cursor_y, content_width, pending_from, item_count);
                cursor_y += used;
                if (cursor_y > bottom)
                    bottom = cursor_y;
                pending_from = item_count;
            }
            int used = layout_block(child, node->content_x, cursor_y, content_width, false);
            cursor_y += used;
            if (cursor_y > bottom)
                bottom = cursor_y;
            /* The child flushed the inline items it collected on its own. */
            pending_from = item_count;
        } else if (child->type == WEB_ELEMENT && child->style.display == WEB_DISPLAY_NONE) {
            /* hidden elements take no space */
        } else if (child->type == WEB_TEXT) {
            if (!child->text || !*child->text)
                continue;
            collect_inline(child, s, 0, content_width);
        } else if (child->type == WEB_ELEMENT) {
            collect_inline(child, s, 0, content_width);
        }
        child = child->next;
    }
    if (item_count > pending_from) {
        int used = flush_inline(node->content_x, cursor_y, content_width, pending_from, item_count);
        cursor_y += used;
        if (cursor_y > bottom)
            bottom = cursor_y;
        pending_from = item_count;
    }
    node->content_h = bottom - node->content_y;
    int height = node->content_y + node->content_h + padding_bottom + border_bottom - box_y;
    if (s->height >= 0)
        height = s->height;
    else if (s->height_pct >= 0)
        height = avail_width * s->height_pct / 100;
    node->h = height;
    return margin_top + height + margin_bottom;
}

void web_layout(WebNode *root, int width, int height) {
    (void)height;
    fragments_reset();
    item_count = 0;
    layout_document_height = 0;
    if (!root)
        return;
    int y = 0;
    for (WebNode *c = root->first_child; c; c = c->next) {
        if (c->type != WEB_ELEMENT)
            continue;
        y += layout_block(c, 0, y, width, false);
    }
    layout_document_height = y;
}
int web_document_height(void) { return layout_document_height; }

/* ---------- painting ---------- */

static void paint_borders(WebNode *node, int ox, int oy) {
    WebStyle *s = &node->style;
    if (!s || s->display == WEB_DISPLAY_NONE || !node->w)
        return;
    int x = node->x + ox, y = node->y + oy;
    if (s->background)
        rect(x, y, node->w, node->h, s->background);
    if (s->border[0] > 0 && s->border_shown[0])
        rect(x, y, node->w, s->border[0], s->border_color[0]);
    if (s->border[2] > 0 && s->border_shown[2])
        rect(x, y + node->h - s->border[2], node->w, s->border[2], s->border_color[2]);
    if (s->border[1] > 0 && s->border_shown[1])
        rect(x, y, s->border[1], node->h, s->border_color[1]);
    if (s->border[3] > 0 && s->border_shown[3])
        rect(x + node->w - s->border[3], y, s->border[3], node->h, s->border_color[3]);
}
static void paint_marker(WebNode *node, int ox, int oy) {
    if (!node->style.list_marker || strcmp(node->tag, "li"))
        return;
    char buffer[16];
    const char *marker = "\xb7";
    if (node->style.list_marker == WEB_MARKER_CIRCLE)
        marker = "o";
    else if (node->style.list_marker == WEB_MARKER_SQUARE)
        marker = "-";
    else if (node->style.list_marker == WEB_MARKER_DECIMAL) {
        unsigned index = 1;
        for (WebNode *s = node->prev; s; s = s->prev)
            if (s->type == WEB_ELEMENT && !strcmp(s->tag, "li"))
                index++;
        number(buffer, index);
        strcat(buffer, ".");
        marker = buffer;
    }
    int size = web_font_size(node->style.font_size);
    text(node->content_x + ox - 16, node->content_y + oy, marker, size, node->style.color);
}
static void paint_tree(WebNode *node, int ox, int oy) {
    if (!node)
        return;
    if (node->type == WEB_ELEMENT) {
        paint_borders(node, ox, oy);
        paint_marker(node, ox, oy);
    }
    for (WebNode *c = node->first_child; c; c = c->next)
        paint_tree(c, ox, oy);
}
void web_paint(WebNode *root, int ox, int oy) {
    if (!root)
        return;
    paint_tree(root, ox, oy);
    for (int i = 0; i < fragment_count; i++) {
        WebFragment *f = &fragments[i];
        text(f->x + ox, f->y + oy, f->text, f->size, f->color);
        if (f->underline)
            line(f->x + ox, f->y + oy + f->size + 3, f->x + ox + text_width(f->text, f->size),
                 f->y + oy + f->size + 3, f->color);
    }
}
void web_free_fragments(void) {
    for (int i = 0; i < fragment_count; i++)
        web_free(fragments[i].text);
    fragment_count = 0;
    fragment_capacity = 0;
    web_free(fragments);
    fragments = 0;
    web_free(run_buffer);
    run_buffer = 0;
    run_capacity = 0;
    run_length = 0;
    web_free(items);
    items = 0;
    item_count = 0;
    item_capacity = 0;
}
