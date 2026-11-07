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
    bool space_before, newline, nowrap;
    int line_height;
} WebInlineItem;

static WebFragment *fragments;
static int fragment_count, fragment_capacity;
static WebInlineItem *items;
static int item_count, item_capacity;
static int layout_document_height;
static char *run_buffer;
static int run_length, run_capacity;
static bool collect_space;

static int layout_block(WebNode *node, int x, int y, int avail_width, bool shrink);
static int measure_natural(WebNode *node);
static int measure_outer(WebNode *node);

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
    for (int i = 0; i < fragment_count; i++)
        web_free(fragments[i].text);
    fragment_count = 0;
    run_length = 0;
}
static void fragment_add(int x, int y, const char *text, int len, int size, uint32_t color,
                         bool underline, WebNode *owner) {
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
    f->owner = owner;
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
    if (owner && (owner->style.white_space == 1 || owner->style.white_space == 2)) {
        int offset = 0;
        while (text[offset]) {
            int start = offset;
            while (text[offset] && text[offset] != '\n' && offset - start < 200) offset++;
            int index = item_add();
            WebInlineItem *item = &items[index];
            memset(item, 0, sizeof(*item));
            item->node = node;
            item->owner = owner;
            item->offset = start;
            item->len = offset - start;
            item->size = size;
            item->color = color;
            item->underline = underline;
            item->line_height = owner->style.line_height;
            item->newline = text[offset] == '\n';
            item->nowrap = owner->style.white_space == 1;
            if (item->newline) offset++;
        }
        return;
    }
    int length = (int)strlen(text);
    int i = 0;
    bool pending_space = collect_space;
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
        item->line_height = owner ? owner->style.line_height : 0;
        item->nowrap = owner && owner->style.white_space == 3;
        item->space_before = pending_space;
        pending_space = false;
    }
    collect_space = pending_space;
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
    if (node->style.display == WEB_DISPLAY_NONE || node->style.position >= 2)
        return;
    if (node->style.display == WEB_DISPLAY_INLINE_BLOCK ||
        (node->style.display == WEB_DISPLAY_INLINE &&
         (node->style.padding[0] || node->style.padding[1] || node->style.padding[2] ||
          node->style.padding[3]))) {
        int index = item_add();
        WebInlineItem *item = &items[index];
        memset(item, 0, sizeof(*item));
        item->atomic = true;
        item->atomic_node = node;
        item->size = web_font_size(node->style.font_size);
        item->color = node->style.color;
        item->owner = node;
        WebStyle *s = &node->style;
        int natural = measure_outer(node);
        if (s->width >= 0)
            natural = s->width + (s->border_box ? 0 : s->padding[1] + s->padding[3] + s->border[1] + s->border[3]);
        else if (s->width_pct >= 0)
            natural = avail_width * s->width_pct / 100000;
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
static void align_fragments(int from, int x, int end_x, int width, int align) {
    int shift = align == WEB_ALIGN_CENTER ? (width - (end_x - x)) / 2 :
                align == WEB_ALIGN_RIGHT ? width - (end_x - x) : 0;
    if (shift <= 0) return;
    for (int i = from; i < fragment_count; i++) fragments[i].x += shift;
}
static int flush_inline(int x, int y, int width, int from, int to, int align) {
    int line_start = fragment_count;
    int line_y = y;
    int line_x = x;
    int line_height = 0;
    int run_size = 0;
    uint32_t run_color = 0;
    bool run_underline = false;
    bool run_active = false;
    WebNode *run_owner = 0;
    int run_x = x;
    for (int i = from; i < to; i++) {
        WebInlineItem *item = &items[i];
        if (item->atomic) {
            if (run_active) {
                fragment_add(run_x, line_y + (line_height - run_size) / 2, run_buffer, run_length,
                             run_size, run_color, run_underline, run_owner);
                run_active = false;
                run_length = 0;
            }
            if (line_x > x && line_x + item->width > x + width) {
                line_y += line_height;
                line_height = 0;
                line_x = x;
            }
            int remaining = x + width - line_x;
            /* Recursive layout may grow items and invalidate item. */
            WebNode *atomic = item->atomic_node;
            layout_block(atomic, line_x, line_y, remaining, true);
            line_x += atomic->w;
            if (atomic->h > line_height)
                line_height = atomic->h;
            continue;
        }
        int item_width = measure_text(item->node->text + item->offset, item->len, item->size);
        int item_height = item->line_height > 0 ? item->line_height : item->size + 7;
        int space_width =
            item->space_before ? measure_text(" ", 1, item->size) : 0;
        bool line_has_content = line_x > x;
        if (!item->nowrap && line_has_content && line_x + space_width + item_width > x + width) {
            if (run_active)
                fragment_add(run_x, line_y + (line_height - run_size) / 2, run_buffer, run_length,
                             run_size, run_color, run_underline, run_owner);
            run_active = false;
            run_length = 0;
            align_fragments(line_start, x, line_x, width, align);
            line_start = fragment_count;
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
            run_owner = item->owner;
            run_length = 0;
        } else if (item->size != run_size || item->color != run_color ||
                   item->underline != run_underline || item->owner != run_owner) {
            fragment_add(run_x, line_y + (line_height - run_size) / 2, run_buffer, run_length,
                         run_size, run_color, run_underline, run_owner);
            run_length = 0;
            run_x = line_x;
            run_size = item->size;
            run_color = item->color;
            run_underline = item->underline;
            run_owner = item->owner;
        }
        if (space_width) {
            run_append(" ", 1);
            line_x += space_width;
        }
        run_append(item->node->text + item->offset, item->len);
        if (item->owner && item->owner->type == WEB_ELEMENT &&
            item->owner->style.display == WEB_DISPLAY_INLINE) {
            WebNode *owner = item->owner;
            int text_top = line_y + (item_height - item->size) / 2;
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
        if (item->newline) {
            if (run_active)
                fragment_add(run_x, line_y, run_buffer, run_length, run_size, run_color,
                             run_underline, run_owner);
            run_active = false;
            run_length = 0;
            align_fragments(line_start, x, line_x, width, align);
            line_start = fragment_count;
            line_y += line_height;
            line_x = x;
            line_height = 0;
        }
    }
    if (run_active)
        fragment_add(run_x, line_y + (line_height - run_size) / 2, run_buffer, run_length, run_size,
                     run_color, run_underline, run_owner);
    align_fragments(line_start, x, line_x, width, align);
    run_length = 0;
    return line_y + line_height - y;
}

/* Natural width of an inline subtree, used for shrink to fit boxes. */
static int measure_natural(WebNode *node) {
    if (!node)
        return 0;
    if (node->type == WEB_TEXT) {
        int size = node->style.font_size ? node->style.font_size : 16;
        size = web_font_size(size);
        char *copy = web_strdup(node->text ? node->text : "");
        int width = 0;
        if (node->style.white_space == 1 || node->style.white_space == 2) {
            char *line = copy;
            for (char *p = copy;; p++) {
                if (*p == '\n' || !*p) {
                    bool end = !*p;
                    *p = 0;
                    int measured = text_width(line, size);
                    if (measured > width) width = measured;
                    if (end) break;
                    line = p + 1;
                }
            }
        } else {
            int used = 0;
            bool space = false;
            for (int i = 0; copy[i]; i++) {
                char c = copy[i];
                if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
                    if (!space) copy[used++] = ' ';
                    space = true;
                } else {
                    copy[used++] = c;
                    space = false;
                }
            }
            copy[used] = 0;
            width = text_width(copy, size);
        }
        web_free(copy);
        return width;
    }
    if (node->type != WEB_ELEMENT || node->style.display == WEB_DISPLAY_NONE)
        return 0;
    WebStyle *s = &node->style;
    if (s->width >= 0)
        return s->width;
    if (!strcmp(node->tag, "input"))
        return 160;
    if (!strcmp(node->tag, "img")) {
        const char *width = web_attr(node, "width");
        return width ? atoi(width) : node->image_w ? node->image_w :
               text_width(web_attr(node, "alt") ? web_attr(node, "alt") : "", web_font_size(s->font_size));
    }
    int total = 0, line = 0;
    for (WebNode *c = node->first_child; c; c = c->next) {
        if (c->style.position >= 2) continue;
        int width = measure_outer(c);
        if (c->type == WEB_ELEMENT && c->style.display == WEB_DISPLAY_BLOCK && !c->style.floating) {
            if (line > total) total = line;
            if (width > total) total = width;
            line = 0;
        } else
            line += width;
    }
    return line > total ? line : total;
}

static int measure_outer(WebNode *node) {
    int width = measure_natural(node);
    if (node && node->type == WEB_ELEMENT && node->style.display != WEB_DISPLAY_NONE &&
        !(node->style.border_box && node->style.width >= 0))
        width += node->style.padding[1] + node->style.padding[3] +
                 node->style.border[1] + node->style.border[3];
    return width;
}

/* Exclusions for the direct floats already laid out in this formatting context. */
static void float_bounds(WebNode *parent, WebNode *end, int y, int *left, int *right, int *next) {
    *left = parent->content_x;
    *right = *left + parent->content_w;
    *next = y;
    for (WebNode *c = parent->first_child; c && c != end; c = c->next) {
        if (!c->style.floating || c->style.display == WEB_DISPLAY_NONE || c->style.position >= 2)
            continue;
        int bottom = c->y + c->h + (c->style.margin[2] == WEB_AUTO ? 0 : c->style.margin[2]);
        if (y >= bottom) continue;
        if (c->style.floating == 1 && c->x + c->w > *left)
            *left = c->x + c->w;
        if (c->style.floating == 2 && c->x < *right)
            *right = c->x;
        if (*next == y || bottom < *next) *next = bottom;
    }
}

static void move_box(WebNode *node, int dx, int dy) {
    node->x += dx;
    node->y += dy;
    node->content_x += dx;
    node->content_y += dy;
    for (int i = 0; i < fragment_count; i++)
        if (fragments[i].owner == node) {
            fragments[i].x += dx;
            fragments[i].y += dy;
        }
    for (WebNode *c = node->first_child; c; c = c->next)
        move_box(c, dx, dy);
}

static int layout_block(WebNode *node, int x, int y, int avail_width, bool shrink) {
    WebStyle *s = &node->style;
    if (s->display == WEB_DISPLAY_NONE) {
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
    bool auto_left = s->margin[3] == WEB_AUTO;
    bool auto_right = s->margin[1] == WEB_AUTO;
    int margin_left = auto_left ? 0 : s->margin[3];
    int margin_right = auto_right ? 0 : s->margin[1];
    int border_top = s->border[0], border_bottom = s->border[2];
    int border_left = s->border[3], border_right = s->border[1];
    int padding_top = s->padding[0], padding_bottom = s->padding[2];
    int padding_left = s->padding[3], padding_right = s->padding[1];
    int width;
    if (s->width >= 0)
        width = s->width;
    else if (s->width_pct >= 0)
        width = avail_width * s->width_pct / 100000;
    else if (shrink) {
        int natural = measure_outer(node);
        width = natural < avail_width ? natural : avail_width;
    } else
        width = avail_width - margin_left - margin_right;
    if (width < 0)
        width = 0;
    int horizontal = border_left + border_right + padding_left + padding_right;
    if (!s->border_box && (s->width >= 0 || s->width_pct >= 0))
        width += horizontal;
    int min_w = (s->min_width_pct >= 0 ? avail_width * s->min_width_pct / 100000 : s->min_width) +
                (s->border_box ? 0 : horizontal);
    int max_w = (s->max_width_pct >= 0 ? avail_width * s->max_width_pct / 100000 : s->max_width) +
                (s->border_box ? 0 : horizontal);
    if ((s->max_width >= 0 || s->max_width_pct >= 0) && width > max_w) width = max_w;
    if (width < min_w) width = min_w;
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
    collect_space = false;
    for (WebNode *child = node->first_child; child; child = child->next) {
        if (child->type == WEB_ELEMENT && child->style.display == WEB_DISPLAY_NONE)
            continue;
        bool boxed = child->type == WEB_ELEMENT &&
                     (child->style.display == WEB_DISPLAY_BLOCK || child->style.floating ||
                      child->style.position >= 2);
        if (boxed) {
            int inline_to = item_count;
            if (!child->style.floating && item_count > pending_from) {
                int left, right, next;
                float_bounds(node, child, cursor_y, &left, &right, &next);
                cursor_y += flush_inline(left, cursor_y, right - left, pending_from, item_count, s->text_align);
                if (cursor_y > bottom) bottom = cursor_y;
            }
            WebStyle *cs = &child->style;
            if (cs->position >= 2) {
                WebNode *containing = node;
                while (containing->parent && !containing->style.position &&
                       containing->parent->type == WEB_ELEMENT)
                    containing = containing->parent;
                int ax = containing->content_x, ay = containing->content_y;
                if (cs->inset[3] != WEB_AUTO) ax += cs->inset[3];
                if (cs->inset[0] != WEB_AUTO) ay += cs->inset[0];
                layout_block(child, ax, ay, containing->content_w, true);
                if (cs->inset[3] == WEB_AUTO && cs->inset[1] != WEB_AUTO)
                    move_box(child, containing->content_x + containing->content_w - cs->inset[1] - child->w - child->x, 0);
            } else {
                if (cs->clear)
                    for (WebNode *f = node->first_child; f != child; f = f->next)
                        if (f->style.floating && (cs->clear == 3 || cs->clear == f->style.floating) &&
                            f->y + f->h > cursor_y)
                            cursor_y = f->y + f->h;
                if (cs->floating) {
                    /* Resolve percentages against the full containing block,
                     * then find the first row where the float fits. */
                    layout_block(child, node->content_x, cursor_y, content_width, true);
                    int left, right, next, fy = cursor_y;
                    for (;;) {
                        float_bounds(node, child, fy, &left, &right, &next);
                        if (child->w <= right - left || next <= fy) break;
                        fy = next;
                    }
                    int fx = cs->floating == 2 ? right - child->w : left;
                    move_box(child, fx - child->x, fy - child->y);
                    if (fy > cursor_y) cursor_y = fy;
                    if (child->y + child->h > bottom) bottom = child->y + child->h;
                } else {
                    int used = layout_block(child, node->content_x, cursor_y, content_width, false);
                    cursor_y += used;
                    if (cursor_y > bottom) bottom = cursor_y;
                }
            }
            if (child->style.floating && inline_to > pending_from) {
                int left, right, next;
                float_bounds(node, child->next, cursor_y, &left, &right, &next);
                cursor_y += flush_inline(left, cursor_y, right - left, pending_from, inline_to, s->text_align);
                if (cursor_y > bottom) bottom = cursor_y;
            }
            pending_from = item_count;
            collect_space = false;
        } else if (child->type == WEB_TEXT || child->type == WEB_ELEMENT)
            collect_inline(child, s, node, content_width);
    }
    if (item_count > pending_from) {
        int left, right, next;
        float_bounds(node, 0, cursor_y, &left, &right, &next);
        cursor_y += flush_inline(left, cursor_y, right - left, pending_from, item_count, s->text_align);
        if (cursor_y > bottom) bottom = cursor_y;
    }
    if (!strcmp(node->tag, "img")) {
        int iw = node->image_w, ih = node->image_h;
        const char *aw = web_attr(node, "width"), *ah = web_attr(node, "height");
        if (s->width < 0 && s->width_pct < 0) {
            int intrinsic = aw ? atoi(aw) : iw;
            if (intrinsic > 0) {
                node->w = intrinsic + horizontal;
                node->content_w = intrinsic;
            }
        }
        int image_height = ah ? atoi(ah) : iw > 0 ? node->content_w * ih / iw : s->font_size;
        bottom = node->content_y + image_height;
    } else if (!strcmp(node->tag, "input"))
        bottom = node->content_y + s->font_size + 7;
    node->content_h = bottom - node->content_y;
    int height = node->content_y + node->content_h + padding_bottom + border_bottom - box_y;
    if (s->height >= 0)
        height = s->height + (s->border_box ? 0 : padding_top + padding_bottom + border_top + border_bottom);
    else if (s->height_pct >= 0)
        height = avail_width * s->height_pct / 100000;
    if (height < s->min_height) height = s->min_height;
    if (s->max_height >= 0 && height > s->max_height) height = s->max_height;
    node->h = height;
    if (s->position == 1) {
        int dx = s->inset[3] != WEB_AUTO ? s->inset[3] : s->inset[1] != WEB_AUTO ? -s->inset[1] : 0;
        int dy = s->inset[0] != WEB_AUTO ? s->inset[0] : s->inset[2] != WEB_AUTO ? -s->inset[2] : 0;
        move_box(node, dx, dy);
    }
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
    if (s->background_set)
        rect(x, y, node->w, node->h, s->background);
    if (s->border[0] > 0 && s->border_shown[0])
        rect(x, y, node->w, s->border[0], s->border_color[0]);
    if (s->border[2] > 0 && s->border_shown[2])
        rect(x, y + node->h - s->border[2], node->w, s->border[2], s->border_color[2]);
    if (s->border[1] > 0 && s->border_shown[1])
        rect(x + node->w - s->border[1], y, s->border[1], node->h, s->border_color[1]);
    if (s->border[3] > 0 && s->border_shown[3])
        rect(x, y, s->border[3], node->h, s->border_color[3]);
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
    if (!node) return;
    if (node->type == WEB_ELEMENT &&
        (node->style.display == WEB_DISPLAY_NONE || node->style.hidden || node->style.clip_empty))
        return;
    int saved[4];
    gfx_get_clip(saved);
    if (node->type == WEB_ELEMENT) {
        paint_borders(node, ox, oy);
        paint_marker(node, ox, oy);
        if (node->style.overflow_hidden) {
            int x = node->x + ox, y = node->y + oy;
            int right = x + node->w, bottom = y + node->h;
            if (x < saved[0]) x = saved[0];
            if (y < saved[1]) y = saved[1];
            if (right > saved[0] + saved[2]) right = saved[0] + saved[2];
            if (bottom > saved[1] + saved[3]) bottom = saved[1] + saved[3];
            gfx_clip(x, y, right > x ? right - x : 0, bottom > y ? bottom - y : 0);
        }
    }
    if (node->type == WEB_ELEMENT && !strcmp(node->tag, "img")) {
        int height = node->h - node->style.padding[0] - node->style.padding[2] -
                     node->style.border[0] - node->style.border[2];
        if (node->image)
            gfx_rgba_image(node->content_x + ox, node->content_y + oy, node->content_w, height,
                           node->image, node->image_w, node->image_h);
        else if (web_attr(node, "alt"))
            text(node->content_x + ox, node->content_y + oy, web_attr(node, "alt"),
                 web_font_size(node->style.font_size), node->style.color);
    } else if (node->type == WEB_ELEMENT && !strcmp(node->tag, "input")) {
        const char *value = web_attr(node, "value");
        if (!value || !*value) value = web_attr(node, "placeholder");
        if (value) text(node->content_x + ox, node->content_y + oy, value,
                        web_font_size(node->style.font_size), node->style.color);
    }
    for (int i = 0; i < fragment_count; i++) {
        WebFragment *f = &fragments[i];
        if (f->owner != node) continue;
        text(f->x + ox, f->y + oy, f->text, f->size, f->color);
        if (f->underline)
            line(f->x + ox, f->y + oy + f->size + 3,
                 f->x + ox + text_width(f->text, f->size), f->y + oy + f->size + 3, f->color);
    }
    for (WebNode *c = node->first_child; c; c = c->next)
        paint_tree(c, ox, oy);
    gfx_clip(saved[0], saved[1], saved[2], saved[3]);
}
void web_paint(WebNode *root, int ox, int oy) {
    if (root) paint_tree(root, ox, oy);
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
