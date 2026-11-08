/* CSS parser, selector matcher and cascade for the Luna Browser engine. */
#include "webengine.h"

typedef struct {
    char *tag;
    char *id;
    char *classes[16];
    int class_count;
    char *attr_names[8];
    char *attr_values[8];
    int attr_count;
    char *pseudo;
    bool structural_only;
} WebCompound;

typedef struct {
    WebCompound compounds[8];
    char combinators[8];
    int count;
    int specificity;
} WebComplex;

typedef struct WebDecl {
    char *name, *value;
    bool important;
    struct WebDecl *next;
} WebDecl;

struct WebRule {
    WebComplex *complexes;
    int complex_count;
    WebDecl *decls;
    int order;
};

/* ---------- values ---------- */

typedef struct {
    const char *name;
    uint32_t color;
} WebNamedColor;
static const WebNamedColor named_colors[] = {
    {"black", 0x000000},   {"white", 0xffffff},   {"red", 0xff0000},      {"green", 0x008000},
    {"blue", 0x0000ff},   {"yellow", 0xffff00},  {"cyan", 0x00ffff},     {"aqua", 0x00ffff},
    {"magenta", 0xff00ff},{"fuchsia", 0xff00ff}, {"gray", 0x808080},     {"grey", 0x808080},
    {"silver", 0xc0c0c0}, {"maroon", 0x800000},  {"olive", 0x808000},    {"green", 0x008000},
    {"purple", 0x800080}, {"teal", 0x008080},    {"navy", 0x000080},     {"lime", 0x00ff00},
    {"orange", 0xffa500}, {"pink", 0xffc0cb},   {"brown", 0xa52a2a},    {"gold", 0xffd700},
    {"beige", 0xf5f5dc}, {"ivory", 0xfffff0},   {"khaki", 0xf0e68c},   {"salmon", 0xfa8072},
    {"tan", 0xd2b48c},   {"violet", 0xee82ee},  {"indigo", 0x4b0082},   {"turquoise", 0x40e0d0},
    {"coral", 0xff7f50}, {"crimson", 0xdc143c}, {"darkgray", 0xa9a9a9}, {"lightgray", 0xd3d3d3},
    {"lightblue", 0xadd8e6}, {"skyblue", 0x87ceeb}, {"steelblue", 0x4682b4},
    {"royalblue", 0x4169e1}, {"seagreen", 0x2e8b57}, {"forestgreen", 0x228b22},
    {"slategray", 0x708090}, {"whitesmoke", 0xf5f5f5}, {"gainsboro", 0xdcdcdc},
    {0, 0}};
int css_color(const char *value, uint32_t *out) {
    while (*value == ' ')
        value++;
    if (!*value)
        return 0;
    if (*value == '#') {
        value++;
        int len = 0;
        unsigned v = 0;
        while (len < 8 && value[len] && value[len] != ' ' && value[len] != ';') {
            char c = value[len];
            int d;
            if (c >= '0' && c <= '9')
                d = c - '0';
            else if ((c | 32) >= 'a' && (c | 32) <= 'f')
                d = (c | 32) - 'a' + 10;
            else
                return 0;
            v = v * 16 + (unsigned)d;
            len++;
        }
        if (len == 3) {
            unsigned r = (v >> 8) & 15, g = (v >> 4) & 15, b = v & 15;
            *out = (r * 17 << 16) | (g * 17 << 8) | (b * 17);
            return 1;
        }
        if (len == 6) {
            *out = v & 0xffffff;
            return 1;
        }
        return 0;
    }
    if (!strncmp(value, "rgb", 3)) {
        const char *p = value + 3;
        if (*p == '(')
            p++;
        unsigned comp[3] = {0, 0, 0};
        for (int i = 0; i < 3; i++) {
            while (*p == ' ')
                p++;
            unsigned value32 = 0;
            bool percent = false;
            while (*p >= '0' && *p <= '9')
                value32 = value32 * 10 + (unsigned)(*p++ - '0');
            if (*p == '%') {
                percent = true;
                p++;
            }
            comp[i] = percent ? value32 * 255 / 100 : value32;
            while (*p == ' ')
                p++;
            if (*p == ',')
                p++;
        }
        *out = ((comp[0] & 255) << 16) | ((comp[1] & 255) << 8) | (comp[2] & 255);
        return 1;
    }
    for (int i = 0; named_colors[i].name; i++)
        if (!web_strcasecmp(value, named_colors[i].name)) {
            *out = named_colors[i].color;
            return 1;
        }
    return 0;
}

/* Parses a CSS length. Percentages are reported through is_percent. */
int css_length(const char *value, int font_size, int percent_out, bool *is_percent) {

    if (is_percent)
        *is_percent = false;
    while (*value == ' ')
        value++;
    if (!*value)
        return 0;
    bool negative = false;
    if (*value == '-') {
        negative = true;
        value++;
    }
    int number = 0, fraction = 0, scale = 1;
    bool digits = false;
    while (*value >= '0' && *value <= '9') {
        number = number * 10 + (*value++ - '0');
        digits = true;
        if (number > 100000)
            return 0;
    }
    if (*value == '.') {
        value++;
        while (*value >= '0' && *value <= '9') {
            digits = true;
            if (scale < 1000) {
                fraction = fraction * 10 + (*value - '0');
                scale *= 10;
            }
            value++;
        }
    }
    if (!digits)
        return 0;
    const char *unit = value;
    while (*unit && *unit != ' ' && *unit != ';' && *unit != ')')
        unit++;
    size_t unit_len = (size_t)(unit - value);
    int factor = 1, divisor = 1;
    if (unit_len == 1 && value[0] == '%') {
        if (is_percent)
            *is_percent = true;
        if (percent_out > 0) factor = percent_out;
    } else if (unit_len == 2 && !strncmp(value, "em", 2))
        factor = font_size;
    else if (unit_len == 3 && !strncmp(value, "rem", 3))
        factor = 16;
    else if (unit_len == 2 && !strncmp(value, "pt", 2)) {
        factor = 4;
        divisor = 3;
    } else if (unit_len && !(unit_len == 2 && !strncmp(value, "px", 2)))
        return 0;
    int result = (int)(((long long)number * scale + fraction) * factor / (scale * divisor));
    return negative ? -result : result;
}
static void trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = 0;
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (p != s)
        memmove(s, p, strlen(p) + 1);
}

/* ---------- selectors ---------- */

static bool ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '_';
}
static void compound_free(WebCompound *c) {
    web_free(c->tag);
    web_free(c->id);
    for (int i = 0; i < c->class_count; i++)
        web_free(c->classes[i]);
    for (int i = 0; i < c->attr_count; i++) {
        web_free(c->attr_names[i]);
        web_free(c->attr_values[i]);
    }
    web_free(c->pseudo);
}
static void complex_free(WebComplex *sel) {
    for (int i = 0; i < sel->count; i++)
        compound_free(&sel->compounds[i]);
}
static char *copy_lower(const char *s, size_t len) {
    char *out = web_strndup(s, len);
    for (char *q = out; *q; q++)
        if (*q >= 'A' && *q <= 'Z')
            *q = (char)(*q + 32);
    return out;
}
static void complex_add_class(WebCompound *c, const char *name, size_t len) {
    if (c->class_count >= 16)
        return;
    c->classes[c->class_count++] = copy_lower(name, len);
}
static void complex_add_attr(WebCompound *c, const char *name, size_t name_len, const char *value,
                              size_t value_len) {
    if (c->attr_count >= 8)
        return;
    c->attr_names[c->attr_count] = copy_lower(name, name_len);
    c->attr_values[c->attr_count] = value ? copy_lower(value, value_len) : 0;
    c->attr_count++;
}

/* Parses one complex selector such as "div#main p.intro > a". */
static bool complex_parse(const char *text, WebComplex *sel) {
    memset(sel, 0, sizeof(*sel));
    const char *p = text;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (!*p)
            break;
        if (sel->count >= 8)
            return false;
        WebCompound *c = &sel->compounds[sel->count];
        memset(c, 0, sizeof(*c));
        bool first = true;
        while (*p) {
            if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '>' || *p == ',')
                break;
            if (*p == '*') {
                p++;
                continue;
            }
            if (*p == '#') {
                p++;
                const char *start = p;
                while (*p && ident_char(*p))
                    p++;
                c->id = copy_lower(start, (size_t)(p - start));
                continue;
            }
            if (*p == '.') {
                p++;
                const char *start = p;
                while (*p && ident_char(*p))
                    p++;
                complex_add_class(c, start, (size_t)(p - start));
                continue;
            }
            if (*p == '[') {
                p++;
                while (*p == ' ')
                    p++;
                const char *name_start = p;
                while (*p && *p != ']' && *p != '=' && *p != ' ')
                    p++;
                const char *name_end = p;
                char *value = 0;
                size_t value_len = 0;
                while (*p == ' ')
                    p++;
                if (*p == '=') {
                    p++;
                    char quote = 0;
                    if (*p == '"' || *p == '\'') {
                        quote = *p++;
                        const char *value_start = p;
                        while (*p && *p != quote)
                            p++;
                        value_len = (size_t)(p - value_start);
                        value = web_strndup(value_start, value_len);
                        if (*p)
                            p++;
                    } else {
                        const char *value_start = p;
                        while (*p && *p != ']' && *p != ' ')
                            p++;
                        value_len = (size_t)(p - value_start);
                        value = web_strndup(value_start, value_len);
                    }
                }
                while (*p && *p != ']')
                    p++;
                if (*p == ']')
                    p++;
                complex_add_attr(c, name_start, (size_t)(name_end - name_start), value, value_len);
                web_free(value);
                continue;
            }
            if (*p == ':') {
                p++;
                if (*p == ':')
                    p++;
                const char *start = p;
                while (*p && ident_char(*p))
                    p++;
                if (*p == '(') {
                    int depth = 1;
                    p++;
                    while (*p && depth) {
                        if (*p == '(') depth++;
                        else if (*p == ')') depth--;
                        p++;
                    }
                }
                web_free(c->pseudo);
                c->pseudo = copy_lower(start, (size_t)(p - start));
                continue;
            }
            if (ident_char(*p)) {
                const char *start = p;
                while (*p && ident_char(*p))
                    p++;
                if (first) {
                    c->tag = copy_lower(start, (size_t)(p - start));
                    first = false;
                }
                continue;
            }
            p++;
        }
        sel->count++;
        /* Consume the combinator between this compound and the next one. */
        const char *q = p;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
            q++;
        if (*q == '>') {
            sel->combinators[sel->count - 1] = '>';
            q++;
        } else if (q != p) {
            sel->combinators[sel->count - 1] = ' ';
        }
        p = q;
    }
    if (!sel->count)
        return false;
    for (int i = 0; i < sel->count; i++) {
        WebCompound *c = &sel->compounds[i];
        int spec = 0;
        if (c->id)
            spec += 10000;
        spec += c->class_count * 100;
        spec += c->attr_count * 100;
        if (c->pseudo)
            spec += 100;
        if (c->tag)
            spec += 1;
        sel->specificity += spec;
    }
    return true;
}
static bool compound_matches(const WebCompound *c, WebNode *el) {
    if (!el || el->type != WEB_ELEMENT || !el->tag || !strcmp(el->tag, "#document"))
        return false;
    if (c->tag && strcmp(c->tag, el->tag))
        return false;
    if (c->id) {
        const char *id = web_attr(el, "id");
        if (!id || strcmp(id, c->id))
            return false;
    }
    for (int i = 0; i < c->class_count; i++) {
        const char *classes = web_attr(el, "class");
        if (!classes)
            return false;
        bool found = false;
        const char *p = classes;
        while (*p) {
            while (*p == ' ')
                p++;
            const char *start = p;
            while (*p && *p != ' ')
                p++;
            if ((size_t)(p - start) == strlen(c->classes[i]) &&
                !strncmp(start, c->classes[i], (size_t)(p - start)))
                found = true;
        }
        if (!found)
            return false;
    }
    for (int i = 0; i < c->attr_count; i++) {
        const char *value = web_attr(el, c->attr_names[i]);
        if (!value)
            return false;
        if (c->attr_values[i] && strcmp(value, c->attr_values[i]))
            return false;
    }
    if (c->pseudo) {
        const char *p = c->pseudo;
        if (!strncmp(p, "not(", 4)) {
            size_t n = strlen(p);
            if (n < 6 || p[n - 1] != ')') return false;
            char *inner = web_strndup(p + 4, n - 5);
            bool matches = css_match_selector(inner, el);
            web_free(inner);
            if (matches) return false;
        } else if (!strcmp(p, "first-child")) {
            for (WebNode *n = el->prev; n; n = n->prev)
                if (n->type == WEB_ELEMENT) return false;
        } else if (!strcmp(p, "last-child")) {
            for (WebNode *n = el->next; n; n = n->next)
                if (n->type == WEB_ELEMENT) return false;
        } else if (!strcmp(p, "only-child")) {
            for (WebNode *n = el->parent ? el->parent->first_child : 0; n; n = n->next)
                if (n != el && n->type == WEB_ELEMENT) return false;
        } else if (!strcmp(p, "first-of-type")) {
            for (WebNode *s = el->prev; s; s = s->prev)
                if (s->type == WEB_ELEMENT && !strcmp(s->tag, el->tag))
                    return false;
        } else if (!strcmp(p, "last-of-type")) {
            for (WebNode *s = el->next; s; s = s->next)
                if (s->type == WEB_ELEMENT && !strcmp(s->tag, el->tag))
                    return false;
        } else if (!strcmp(p, "root")) {
            if (el->parent && el->parent->type == WEB_ELEMENT && strcmp(el->parent->tag, "#document"))
                return false;
        } else {
            /* Dynamic and unsupported pseudo classes never match. */
            return false;
        }
    }
    return true;
}
static bool complex_match_at(const WebComplex *sel, int index, WebNode *el) {
    if (!compound_matches(&sel->compounds[index], el))
        return false;
    if (!index)
        return true;
    char combinator = sel->combinators[index - 1];
    WebNode *parent = el->parent;
    if (combinator == '>') {
        if (!parent || !compound_matches(&sel->compounds[index - 1], parent))
            return false;
        return complex_match_at(sel, index - 1, parent);
    }
    for (WebNode *a = parent; a; a = a->parent)
        if (compound_matches(&sel->compounds[index - 1], a) && complex_match_at(sel, index - 1, a))
            return true;
    return false;
}
static bool complex_matches(const WebComplex *sel, WebNode *el) {
    if (!sel->count)
        return false;
    return complex_match_at(sel, sel->count - 1, el);
}
bool css_match_selector(const char *selector, WebNode *element) {
    if (!selector || !element)
        return false;
    char *copy = web_strdup(selector);
    char *part = copy;
    bool matched = false;
    while (part) {
        char *comma = strchr(part, ',');
        if (comma)
            *comma = 0;
        char *text = web_strdup(part);
        trim(text);
        if (*text) {
            WebComplex sel;
            if (complex_parse(text, &sel)) {
                matched = complex_matches(&sel, element);
                complex_free(&sel);
            }
            web_free(text);
        }
        if (!comma)
            break;
        part = comma + 1;
        if (matched)
            break;
    }
    web_free(copy);
    return matched;
}

/* ---------- declarations ---------- */

static WebDecl *decl_new(const char *name, size_t name_len, const char *value, size_t value_len) {
    WebDecl *d = web_malloc(sizeof(WebDecl));
    memset(d, 0, sizeof(*d));
    d->name = web_strndup(name, name_len);
    d->value = web_strndup(value, value_len);
    size_t n = strlen(d->value);
    if (n && d->value[n - 1] == '!')
        d->value[--n] = 0;
    char *mark = strchr(d->value, '!');
    if (mark) {
        char *rest = mark + 1;
        while (*rest == ' ')
            rest++;
        if (!web_strcasecmp(rest, "important")) {
            d->important = true;
            *mark = 0;
        }
    }
    trim(d->name);
    trim(d->value);
    return d;
}
static void decl_free_list(WebDecl *list) {
    while (list) {
        WebDecl *next = list->next;
        web_free(list->name);
        web_free(list->value);
        web_free(list);
        list = next;
    }
}
/* Splits "a: b; c: d" into declarations. */
static WebDecl *parse_declarations(const char *text) {
    WebDecl *head = 0, *tail = 0;
    const char *p = text;
    while (*p) {
        const char *start = p;
        int depth = 0;
        char quote = 0;
        while (*p) {
            if (quote) {
                if (*p == quote)
                    quote = 0;
            } else if (*p == '"' || *p == '\'') {
                quote = *p;
            } else if (*p == '(') {
                depth++;
            } else if (*p == ')') {
                if (depth)
                    depth--;
            } else if (*p == ';' && !depth) {
                break;
            }
            p++;
        }
        const char *end = p;
        if (*p == ';')
            p++;
        const char *colon = 0;
        int paren = 0;
        for (const char *q = start; q < end; q++) {
            if (*q == '(')
                paren++;
            else if (*q == ')')
                paren--;
            else if (*q == ':' && !paren) {
                colon = q;
                break;
            }
        }
        if (colon) {
            WebDecl *d = decl_new(start, (size_t)(colon - start), colon + 1, (size_t)(end - colon - 1));
            if (!*d->name) {
                web_free(d->name);
                web_free(d->value);
                web_free(d);
            } else {
                if (tail)
                    tail->next = d;
                else
                    head = d;
                tail = d;
            }
        }
    }
    return head;
}

/* ---------- stylesheet ---------- */

static int css_viewport_width = 800;
void css_set_viewport(int width) {
    css_viewport_width = width > 0 ? width : 800;
}

/* Evaluate the screen/width queries supported by the native layout engine.
 * Unknown features do not match; in particular print styles must stay out. */
bool css_media_matches(const char *query) {
    if (!query || !*query)
        return true;
    char *copy = web_strdup(query);
    bool any = false;
    char *part = copy;
    while (part && !any) {
        char *comma = strchr(part, ',');
        if (comma)
            *comma = 0;
        trim(part);
        char *p = part;
        bool negate = false, match = true;
        if (!strncmp(p, "not ", 4)) {
            negate = true;
            p += 4;
        } else if (!strncmp(p, "only ", 5))
            p += 5;
        if (*p != '(') {
            char *end = p;
            while (*end && *end != ' ' && *end != '(')
                end++;
            size_t n = (size_t)(end - p);
            match = (n == 6 && !strncmp(p, "screen", n)) ||
                    (n == 3 && !strncmp(p, "all", n));
            p = end;
        }
        while (*p) {
            while (*p == ' ' || *p == '\t' || *p == '\n')
                p++;
            if (!strncmp(p, "and", 3)) {
                p += 3;
                continue;
            }
            if (!*p)
                break;
            if (*p++ != '(') {
                match = false;
                break;
            }
            char *end = strchr(p, ')');
            if (!end) {
                match = false;
                break;
            }
            *end = 0;
            trim(p);
            char *colon = strchr(p, ':');
            if (!colon)
                match = false;
            else {
                *colon = 0;
                trim(p);
                int width = css_length(colon + 1, 16, 0, 0);
                if (!strcmp(p, "min-width"))
                    match = match && css_viewport_width >= width;
                else if (!strcmp(p, "max-width"))
                    match = match && css_viewport_width <= width;
                else if (!strcmp(p, "width"))
                    match = match && css_viewport_width == width;
                else
                    match = false;
            }
            p = end + 1;
        }
        any = negate ? !match : match;
        part = comma ? comma + 1 : 0;
    }
    web_free(copy);
    return any;
}

static bool parse_stylesheet(const char *text, WebRule **rules, int *count, int *order) {
    const char *p = text;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ';') {
            p++;
            continue;
        }
        if (!*p)
            break;
        if (p[0] == '/' && p[1] == '*') {
            const char *end = p + 2;
            while (end[0] && !(end[0] == '*' && end[1] == '/'))
                end++;
            p = end[0] ? end + 2 : end;
            continue;
        }
        if (*p == '@') {
            const char *name_start = ++p;
            while (*p && *p != ' ' && *p != '\t' && *p != '{' && *p != ';')
                p++;
            char *name = web_strndup(name_start, (size_t)(p - name_start));
            const char *query_start = p;
            while (*p && *p != '{' && *p != ';')
                p++;
            char *query = web_strndup(query_start, (size_t)(p - query_start));
            if (*p == '{') {
                const char *block_start = p + 1;
                int depth = 1;
                const char *q = block_start;
                while (*q && depth) {
                    if (*q == '{')
                        depth++;
                    else if (*q == '}')
                        depth--;
                    q++;
                }
                const char *block_end = q > block_start ? q - 1 : block_start;
                if (!web_strcasecmp(name, "media") && css_media_matches(query)) {
                    char *inner = web_strndup(block_start, (size_t)(block_end - block_start));
                    parse_stylesheet(inner, rules, count, order);
                    web_free(inner);
                }
                p = q;
            } else if (*p == ';') {
                p++;
            }
            web_free(query);
            web_free(name);
            continue;
        }
        const char *prelude_start = p;
        while (*p && *p != '{')
            p++;
        if (!*p)
            break;
        const char *prelude_end = p;
        const char *block_start = p + 1;
        int depth = 1;
        const char *q = block_start;
        while (*q && depth) {
            if (*q == '{')
                depth++;
            else if (*q == '}')
                depth--;
            q++;
        }
        const char *block_end = q > block_start ? q - 1 : block_start;
        char *prelude = web_strndup(prelude_start, (size_t)(prelude_end - prelude_start));
        char *body = web_strndup(block_start, (size_t)(block_end - block_start));
        trim(prelude);
        WebDecl *decls = parse_declarations(body);
        web_free(body);
        if (*prelude && decls) {
            /* Split the selector list on top level commas. */
            int selector_total = 1;
            for (char *c = prelude; *c; c++)
                if (*c == ',')
                    selector_total++;
            WebComplex *complexes = web_malloc(sizeof(WebComplex) * (size_t)selector_total);
            memset(complexes, 0, sizeof(WebComplex) * (size_t)selector_total);
            int found = 0;
            char *part = prelude;
            while (part && found < selector_total) {
                char *comma = strchr(part, ',');
                if (comma)
                    *comma = 0;
                char *text = web_strdup(part);
                trim(text);
                if (*text && complex_parse(text, &complexes[found]))
                    found++;
                web_free(text);
                part = comma ? comma + 1 : 0;
            }
            if (found) {
                WebRule *rule = web_malloc(sizeof(WebRule));
                memset(rule, 0, sizeof(*rule));
                rule->complexes = complexes;
                rule->complex_count = found;
                rule->decls = decls;
                rule->order = (*order)++;
                WebRule *grown = web_malloc(sizeof(WebRule) * (size_t)(*count + 1));
                if (*count)
                    memcpy(grown, *rules, sizeof(WebRule) * (size_t)*count);
                grown[*count] = *rule;
                web_free(rule);
                web_free(*rules);
                *rules = grown;
                (*count)++;
                decls = 0;
            } else {
                web_free(complexes);
            }
        }
        decl_free_list(decls);
        web_free(prelude);
        p = q;
    }
    return true;
}
bool css_parse_sheet(const char *text, WebRule **rules, int *count) {
    *rules = 0;
    *count = 0;
    int order = 0;
    if (!text)
        return true;
    char *clean = web_strdup(text);
    char quote = 0;
    for (char *p = clean; *p; p++) {
        if (quote) {
            if (*p == '\\' && p[1])
                p++;
            else if (*p == quote)
                quote = 0;
        } else if (*p == '\'' || *p == '"')
            quote = *p;
        else if (p[0] == '/' && p[1] == '*') {
            *p++ = ' ';
            *p = ' ';
            while (p[1] && !(p[1] == '*' && p[2] == '/'))
                *++p = ' ';
            if (p[1]) {
                *++p = ' ';
                *++p = ' ';
            }
        }
    }
    bool ok = parse_stylesheet(clean, rules, count, &order);
    web_free(clean);
    return ok;
}
void css_free_sheet(WebRule *rules, int count) {
    for (int i = 0; i < count; i++) {
        for (int c = 0; c < rules[i].complex_count; c++)
            complex_free(&rules[i].complexes[c]);
        web_free(rules[i].complexes);
        decl_free_list(rules[i].decls);
    }
    web_free(rules);
}

/* ---------- cascade ---------- */

static const char *const block_display[] = {"html", "body", "div",   "p",      "h1", "h2",
                                            "h3",   "h4",   "h5",    "h6",     "ul", "ol",
                                            "li",   "dl",   "dt",    "dd",     "section",
                                            "article", "aside", "header", "footer", "nav",
                                            "main", "form", "table", "thead", "tbody", "tfoot",
                                            "tr",   "td",   "th",    "blockquote", "pre",
                                            "hr",   "figure", "figcaption", "address", "fieldset",
                                            "details", "summary", "hgroup", "dialog", 0};
static bool in_list(const char *const *list, const char *tag) {
    for (int i = 0; list[i]; i++)
        if (!strcmp(list[i], tag))
            return true;
    return false;
}
/* Parses one to four box values such as "8px 0 auto". */
static int box_values(const char *value, int out[4], int font_size) {
    char buf[256];
    size_t n = strlen(value);
    if (n >= sizeof(buf))
        n = sizeof(buf) - 1;
    memcpy(buf, value, n);
    buf[n] = 0;
    int count = 0;
    char *token = buf;
    while (*token && count < 4) {
        while (*token == ' ' || *token == '\t')
            token++;
        if (!*token)
            break;
        char *start = token;
        while (*token && *token != ' ' && *token != '\t')
            token++;
        char saved = *token;
        *token = 0;
        if (!web_strcasecmp(start, "auto"))
            out[count++] = WEB_AUTO;
        else if (strchr(start, '%'))
            out[count++] = 0;
        else {
            out[count++] = css_length(start, font_size, 0, 0);
        }
        *token = saved;
    }
    return count;
}
static void expand_box(const int parts[4], int count, int out[4]) {
    switch (count) {
        case 1:
            out[0] = out[1] = out[2] = out[3] = parts[0];
            break;
        case 2:
            out[0] = out[2] = parts[0];
            out[1] = out[3] = parts[1];
            break;
        case 3:
            out[0] = parts[0];
            out[1] = out[3] = parts[1];
            out[2] = parts[2];
            break;
        default:
            for (int i = 0; i < 4; i++)
                out[i] = parts[i];
            break;
    }
}
static void ua_defaults(WebNode *el, WebStyle *s) {
    memset(s, 0, sizeof(*s));
    s->color = 0x11161c;
    s->background = 0;
    s->font_size = 16;
    s->font_weight = 400;
    s->line_height = 0;
    s->display = WEB_DISPLAY_INLINE;
    s->text_align = WEB_ALIGN_LEFT;
    s->width = -1;
    s->height = -1;
    s->width_pct = -1;
    s->height_pct = -1;
    s->list_marker = WEB_MARKER_NONE;
    s->max_width = s->max_height = -1;
    s->min_width_pct = s->max_width_pct = -1;
    for (int i = 0; i < 4; i++)
        s->inset[i] = WEB_AUTO;
    const char *tag = el->tag;
    if (!tag)
        return;
    if (in_list(block_display, tag))
        s->display = WEB_DISPLAY_BLOCK;
    /* Metadata elements never take part in the rendered page. */
    if (!strcmp(tag, "style") || !strcmp(tag, "script") || !strcmp(tag, "head") ||
        !strcmp(tag, "title") || !strcmp(tag, "meta") || !strcmp(tag, "link") ||
        !strcmp(tag, "base") || !strcmp(tag, "noscript") ||
        !strcmp(tag, "template") || !strcmp(tag, "source")) {
        s->display = WEB_DISPLAY_NONE;
        return;
    }
    if (!strcmp(tag, "body")) {
        s->margin[0] = s->margin[1] = s->margin[2] = s->margin[3] = 8;
    } else if (!strcmp(tag, "p")) {
        s->margin[0] = s->margin[2] = 16;
    } else if (!strcmp(tag, "h1")) {
        s->font_size = 32;
        s->font_weight = 700;
        s->margin[0] = s->margin[2] = 21;
    } else if (!strcmp(tag, "h2")) {
        s->font_size = 24;
        s->font_weight = 700;
        s->margin[0] = s->margin[2] = 19;
    } else if (!strcmp(tag, "h3")) {
        s->font_size = 20;
        s->font_weight = 700;
        s->margin[0] = s->margin[2] = 18;
    } else if (!strcmp(tag, "h4")) {
        s->font_weight = 700;
        s->margin[0] = s->margin[2] = 21;
    } else if (!strcmp(tag, "h5")) {
        s->font_size = 14;
        s->font_weight = 700;
    } else if (!strcmp(tag, "h6")) {
        s->font_size = 13;
        s->font_weight = 700;
    } else if (!strcmp(tag, "ul") || !strcmp(tag, "ol")) {
        s->margin[0] = s->margin[2] = 16;
        s->padding[3] = 40;
        s->list_marker = !strcmp(tag, "ol") ? WEB_MARKER_DECIMAL : WEB_MARKER_DISC;
    } else if (!strcmp(tag, "li")) {
        s->list_marker = WEB_MARKER_DISC;
    } else if (!strcmp(tag, "blockquote")) {
        s->margin[0] = s->margin[2] = 16;
        s->margin[1] = s->margin[3] = 40;
    } else if (!strcmp(tag, "hr")) {
        s->margin[0] = s->margin[2] = 8;
        s->border[0] = s->border[1] = s->border[2] = s->border[3] = 1;
        s->border_color[0] = s->border_color[1] = s->border_color[2] = s->border_color[3] = 0xcccccc;
        s->border_shown[0] = s->border_shown[1] = s->border_shown[2] = s->border_shown[3] = true;
    } else if (!strcmp(tag, "strong") || !strcmp(tag, "b")) {
        s->font_weight = 700;
    } else if (!strcmp(tag, "small")) {
        s->font_size = 13;
    } else if (!strcmp(tag, "a")) {
        s->color = 0x0b57d0;
        s->underline = true;
    } else if (!strcmp(tag, "code") || !strcmp(tag, "kbd") || !strcmp(tag, "samp")) {
        s->color = 0x9c2b2b;
    } else if (!strcmp(tag, "pre")) {
        s->white_space = 1;
        s->margin[0] = s->margin[2] = 13;
    } else if (!strcmp(tag, "img") || !strcmp(tag, "input") || !strcmp(tag, "button") ||
               !strcmp(tag, "select") || !strcmp(tag, "textarea")) {
        s->display = WEB_DISPLAY_INLINE_BLOCK;
    } else if (!strcmp(tag, "td") || !strcmp(tag, "th")) {
        s->padding[0] = s->padding[1] = s->padding[2] = s->padding[3] = 4;
        s->border[0] = s->border[1] = s->border[2] = s->border[3] = 1;
        s->border_color[0] = s->border_color[1] = s->border_color[2] = s->border_color[3] = 0xd0d7de;
        s->border_shown[0] = s->border_shown[1] = s->border_shown[2] = s->border_shown[3] = true;
    }
}
static void inherit_style(const WebStyle *parent, WebStyle *s) {
    if (!parent)
        return;
    s->color = parent->color;
    s->font_size = parent->font_size;
    s->font_weight = parent->font_weight;
    s->line_height = parent->line_height;
    s->text_align = parent->text_align;
    s->list_marker = parent->list_marker;
    s->white_space = parent->white_space;
    s->hidden = parent->hidden;
}
static void apply_declaration(WebStyle *s, const char *name, const char *value) {
    if (!strcmp(name, "color")) {
        uint32_t color;
        if (css_color(value, &color))
            s->color = color;
    } else if (!strcmp(name, "background-color")) {
        uint32_t color;
        if (css_color(value, &color)) {
            s->background = color;
            s->background_set = true;
        }
    } else if (!strcmp(name, "background")) {
        uint32_t color;
        if (css_color(value, &color)) {
            s->background = color;
            s->background_set = true;
        }
        else if (!web_strcasecmp(value, "transparent")) {
            s->background = 0;
            s->background_set = false;
        }
    } else if (!strcmp(name, "font")) {
        char *copy = web_strdup(value), *p = copy;
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            char *token = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = 0;
            if ((*token >= '0' && *token <= '9') || *token == '.') {
                char *slash = strchr(token, '/');
                if (slash) *slash = 0;
                bool percent = false;
                int size = css_length(token, s->font_size, 0, &percent);
                if (percent) size = s->font_size * size / 100;
                if (size > 0) s->font_size = size;
                if (slash) apply_declaration(s, "line-height", slash + 1);
                break;
            }
        }
        web_free(copy);
    } else if (!strcmp(name, "font-size")) {
        bool percent = false;
        int parsed = css_length(value, s->font_size, 0, &percent);
        if (percent)
            s->font_size = s->font_size * parsed / 100;
        else if (parsed)
            s->font_size = parsed;
    } else if (!strcmp(name, "font-weight")) {
        if (!web_strcasecmp(value, "bold"))
            s->font_weight = 700;
        else if (!web_strcasecmp(value, "normal"))
            s->font_weight = 400;
        else {
            int parsed = atoi(value);
            if (parsed)
                s->font_weight = parsed;
        }
    } else if (!strcmp(name, "line-height")) {
        if (!strcmp(value, "normal")) s->line_height = 0;
        else {
            bool percent = false;
            int parsed = css_length(value, s->font_size, 0, &percent);
            bool unitless = true;
            for (const char *p = value; *p; p++)
                if ((*p < '0' || *p > '9') && *p != '.' && *p != ' ') unitless = false;
            if (unitless) {
                char relative[64] = {0};
                append(relative, value, sizeof(relative));
                append(relative, "em", sizeof(relative));
                parsed = css_length(relative, s->font_size, 0, 0);
            }
            if (percent) parsed = s->font_size * parsed / 100;
            if (parsed > 0) s->line_height = parsed;
        }
    } else if (!strcmp(name, "display")) {
        if (!web_strcasecmp(value, "none"))
            s->display = WEB_DISPLAY_NONE;
        else if (!web_strcasecmp(value, "inline"))
            s->display = WEB_DISPLAY_INLINE;
        else if (!web_strcasecmp(value, "inline-block"))
            s->display = WEB_DISPLAY_INLINE_BLOCK;
        else
            s->display = WEB_DISPLAY_BLOCK;
    } else if (!strcmp(name, "text-align")) {
        if (!web_strcasecmp(value, "center"))
            s->text_align = WEB_ALIGN_CENTER;
        else if (!web_strcasecmp(value, "right"))
            s->text_align = WEB_ALIGN_RIGHT;
        else
            s->text_align = WEB_ALIGN_LEFT;
    } else if (!strcmp(name, "margin")) {
        int parts[4] = {0, 0, 0, 0};
        int count = box_values(value, parts, s->font_size);
        expand_box(parts, count, s->margin);
    } else if (!strcmp(name, "padding")) {
        int parts[4] = {0, 0, 0, 0};
        int count = box_values(value, parts, s->font_size);
        expand_box(parts, count, s->padding);
    } else if (!strncmp(name, "border", 6)) {
        int first = 0, last = 3;
        const char *part = name + 6;
        if (!strncmp(part, "-top", 4)) { first = last = 0; part += 4; }
        else if (!strncmp(part, "-right", 6)) { first = last = 1; part += 6; }
        else if (!strncmp(part, "-bottom", 7)) { first = last = 2; part += 7; }
        else if (!strncmp(part, "-left", 5)) { first = last = 3; part += 5; }
        if (!*part) {
            int width = 3;
            bool shown = false;
            uint32_t color = s->color;
            char *copy = web_strdup(value), *p = copy;
            while (*p) {
                while (*p == ' ') p++;
                if (!*p) break;
                char *start = p;
                while (*p && *p != ' ') p++;
                if (*p) *p++ = 0;
                if ((*start >= '0' && *start <= '9') || *start == '.')
                    width = css_length(start, s->font_size, 0, 0);
                else if (!strcmp(start, "solid") || !strcmp(start, "dashed") ||
                         !strcmp(start, "dotted") || !strcmp(start, "double"))
                    shown = true;
                else if (!strcmp(start, "none") || !strcmp(start, "hidden"))
                    shown = false;
                else
                    css_color(start, &color);
            }
            for (int i = first; i <= last; i++) {
                s->border[i] = shown ? width : 0;
                s->border_shown[i] = shown && width > 0;
                s->border_color[i] = color;
            }
            web_free(copy);
        } else if (!strcmp(part, "-color")) {
            uint32_t color;
            if (css_color(value, &color))
                for (int i = first; i <= last; i++) s->border_color[i] = color;
        } else if (!strcmp(part, "-width")) {
            int parts[4] = {0}, expanded[4];
            int count = box_values(value, parts, s->font_size);
            expand_box(parts, count, expanded);
            for (int i = first; i <= last; i++) s->border[i] = expanded[first == last ? 0 : i];
        } else if (!strcmp(part, "-style")) {
            bool shown = strcmp(value, "none") && strcmp(value, "hidden");
            for (int i = first; i <= last; i++) {
                s->border_shown[i] = shown;
                if (!shown) s->border[i] = 0;
            }
        }
    } else if (!strcmp(name, "text-decoration") || !strcmp(name, "text-decoration-line")) {
        s->underline = false;
        for (const char *p = value; *p; p++)
            if (!strncmp(p, "underline", 9)) {
                s->underline = true;
                break;
            }
    } else if (!strcmp(name, "width")) {
        if (!web_strcasecmp(value, "auto")) {
            s->width = s->width_pct = -1;
            return;
        }
        bool percent = false;
        int parsed = css_length(value, s->font_size, 1000, &percent);
        if (percent) {
            s->width_pct = parsed;
            s->width = -1;
        } else if (parsed >= 0) {
            s->width = parsed;
            s->width_pct = -1;
        }
    } else if (!strcmp(name, "height")) {
        if (!web_strcasecmp(value, "auto")) {
            s->height = s->height_pct = -1;
            return;
        }
        bool percent = false;
        int parsed = css_length(value, s->font_size, 1000, &percent);
        if (percent) {
            s->height_pct = parsed;
            s->height = -1;
        } else if (parsed >= 0) {
            s->height = parsed;
            s->height_pct = -1;
        }
    } else if (!strncmp(name, "margin-", 7) || !strncmp(name, "padding-", 8)) {
        bool margin = !strncmp(name, "margin-", 7);
        const char *side = name + (margin ? 7 : 8);
        int index = !strcmp(side, "top") ? 0 : !strcmp(side, "right") ? 1 :
                    !strcmp(side, "bottom") ? 2 : !strcmp(side, "left") ? 3 : -1;
        if (index >= 0) {
            int v = margin && !strcmp(value, "auto") ? WEB_AUTO :
                    css_length(value, s->font_size, 0, 0);
            if (!margin && v < 0) v = 0;
            (margin ? s->margin : s->padding)[index] = v;
        }
    } else if (!strcmp(name, "float")) {
        s->floating = !strcmp(value, "left") ? 1 : !strcmp(value, "right") ? 2 : 0;
    } else if (!strcmp(name, "clear")) {
        s->clear = !strcmp(value, "left") ? 1 : !strcmp(value, "right") ? 2 :
                   !strcmp(value, "both") ? 3 : 0;
    } else if (!strcmp(name, "position")) {
        s->position = !strcmp(value, "relative") ? 1 : !strcmp(value, "absolute") ? 2 :
                      !strcmp(value, "fixed") ? 3 : 0;
    } else if (!strcmp(name, "top") || !strcmp(name, "right") ||
               !strcmp(name, "bottom") || !strcmp(name, "left")) {
        int side = !strcmp(name, "top") ? 0 : !strcmp(name, "right") ? 1 :
                   !strcmp(name, "bottom") ? 2 : 3;
        s->inset[side] = !strcmp(value, "auto") ? WEB_AUTO : css_length(value, s->font_size, 0, 0);
    } else if (!strcmp(name, "box-sizing")) {
        s->border_box = !strcmp(value, "border-box");
    } else if (!strcmp(name, "min-width") || !strcmp(name, "max-width")) {
        bool percent = false;
        int v = css_length(value, s->font_size, 1000, &percent);
        bool min = !strcmp(name, "min-width");
        if (min) {
            s->min_width_pct = percent ? v : -1;
            s->min_width = percent ? 0 : v;
        } else {
            s->max_width_pct = percent ? v : -1;
            s->max_width = percent || !strcmp(value, "none") ? -1 : v;
        }
    } else if (!strcmp(name, "min-height")) {
        s->min_height = css_length(value, s->font_size, 0, 0);
    } else if (!strcmp(name, "max-height")) {
        s->max_height = !strcmp(value, "none") ? -1 : css_length(value, s->font_size, 0, 0);
    } else if (!strcmp(name, "overflow") || !strcmp(name, "overflow-x") || !strcmp(name, "overflow-y")) {
        s->overflow_hidden = strcmp(value, "visible") != 0;
    } else if (!strcmp(name, "clip")) {
        s->clip_empty = !strcmp(value, "rect(0 0 0 0)") || !strcmp(value, "rect(0, 0, 0, 0)");
    } else if (!strcmp(name, "white-space")) {
        s->white_space = !strcmp(value, "pre") ? 1 : !strcmp(value, "pre-wrap") ? 2 :
                         !strcmp(value, "nowrap") ? 3 : 0;
    } else if (!strcmp(name, "list-style")) {
        if (!strcmp(value, "none")) s->list_marker = WEB_MARKER_NONE;
    } else if (!strcmp(name, "visibility")) {
        s->hidden = !web_strcasecmp(value, "hidden");
    } else if (!strcmp(name, "list-style-type")) {
        if (!web_strcasecmp(value, "none"))
            s->list_marker = WEB_MARKER_NONE;
        else if (!web_strcasecmp(value, "circle"))
            s->list_marker = WEB_MARKER_CIRCLE;
        else if (!web_strcasecmp(value, "square"))
            s->list_marker = WEB_MARKER_SQUARE;
        else if (!web_strcasecmp(value, "decimal"))
            s->list_marker = WEB_MARKER_DECIMAL;
        else
            s->list_marker = WEB_MARKER_DISC;
    }
}
typedef struct {
    WebDecl *decl;
    int specificity;
    int order;
} WebCandidate;
static void style_element(WebNode *el, WebRule *rules, int count, const WebStyle *parent_style) {
    ua_defaults(el, &el->style);
    inherit_style(parent_style, &el->style);
    if (el->tag && !strcmp(el->tag, "pre")) el->style.white_space = 1;
    int capacity = 64, candidate_count = 0;
    WebCandidate *candidates = web_malloc(sizeof(WebCandidate) * (size_t)capacity);
    WebDecl *inline_decls = parse_declarations(web_attr(el, "style") ? web_attr(el, "style") : "");
    for (int i = 0; i <= count; i++) {
        int specificity = i == count ? 1000 : -1;
        if (i < count)
            for (int c = 0; c < rules[i].complex_count; c++)
                if (complex_matches(&rules[i].complexes[c], el) &&
                    rules[i].complexes[c].specificity > specificity)
                    specificity = rules[i].complexes[c].specificity;
        if (specificity < 0) continue;
        for (WebDecl *d = i == count ? inline_decls : rules[i].decls; d; d = d->next) {
            if (candidate_count == capacity) {
                capacity *= 2;
                WebCandidate *grown = web_malloc(sizeof(WebCandidate) * (size_t)capacity);
                memcpy(grown, candidates, sizeof(WebCandidate) * (size_t)candidate_count);
                web_free(candidates);
                candidates = grown;
            }
            candidates[candidate_count++] = (WebCandidate){d, specificity, i};
        }
    }
    for (int i = 1; i < candidate_count; i++) {
        WebCandidate item = candidates[i];
        int j = i - 1;
        while (j >= 0 && candidates[j].specificity > item.specificity) {
            candidates[j + 1] = candidates[j];
            j--;
        }
        candidates[j + 1] = item;
    }
    for (int important = 0; important < 2; important++)
        for (int i = 0; i < candidate_count; i++) {
            WebDecl *d = candidates[i].decl;
            if (d->important != (bool)important) continue;
            if (!strcmp(d->name, "font-size"))
                el->style.font_size = parent_style ? parent_style->font_size : 16;
            apply_declaration(&el->style, d->name, d->value);
        }
    /* HTML hidden controls are not CSS boxes. In particular, Google embeds
     * internal form values (ei, hl, encoding, etc.) as hidden inputs; the
     * old renderer painted their values visibly and corrupted line layout.
     * A hidden input never generates a box, even if display is overridden. */
    if (el->type == WEB_ELEMENT && el->tag) {
        if (!strcmp(el->tag, "input")) {
            const char *type = web_attr(el, "type");
            if (type && !web_strcasecmp(type, "hidden"))
                el->style.display = WEB_DISPLAY_NONE;
        }
        /* The hidden HTML attribute applies unless author CSS explicitly
         * overrides it, unlike input[type=hidden]. */
        if (web_attr_has(el, "hidden") &&
            el->style.display != WEB_DISPLAY_NONE) {
            const char *inline_style = web_attr(el, "style");
            if (!inline_style || !strstr(inline_style, "display"))
                el->style.display = WEB_DISPLAY_NONE;
        }
    }
    web_free(candidates);
    decl_free_list(inline_decls);
    el->styled = true;
}
static void css_apply_node(WebNode *el, WebRule *rules, int count, const WebStyle *parent) {
    style_element(el, rules, count, parent);
    for (WebNode *c = el->first_child; c; c = c->next)
        css_apply_node(c, rules, count, &el->style);
}
void css_apply(WebNode *root, WebRule *rules, int count) {
    if (!root)
        return;
    css_apply_node(root, rules, count, 0);
}

/* ---------- inline style changes from scripts ---------- */

/* Rebuilds the inline style attribute without the named property. */
static void inline_style_without(WebNode *node, const char *name, char *out, size_t cap) {
    out[0] = 0;
    const char *current = web_attr(node, "style");
    if (!current)
        return;
    WebDecl *decls = parse_declarations(current);
    for (WebDecl *d = decls; d; d = d->next) {
        if (!strcmp(d->name, name))
            continue;
        append(out, d->name, cap);
        append(out, ": ", cap);
        append(out, d->value, cap);
        if (d->important)
            append(out, " !important", cap);
        append(out, "; ", cap);
    }
    decl_free_list(decls);
}
void css_set_property(WebNode *node, const char *name, const char *value) {
    if (!node || !name || !value)
        return;
    apply_declaration(&node->style, name, value);
    char rebuilt[1024];
    inline_style_without(node, name, rebuilt, sizeof(rebuilt));
    char combined[1200];
    combined[0] = 0;
    append(combined, rebuilt, sizeof(combined));
    append(combined, name, sizeof(combined));
    append(combined, ": ", sizeof(combined));
    append(combined, value, sizeof(combined));
    append(combined, ";", sizeof(combined));
    web_set_attr(node, "style", combined);
}
void css_get_property(WebNode *node, const char *name, char *out, size_t cap) {
    if (!out || !cap)
        return;
    out[0] = 0;
    if (!node || !name)
        return;
    const char *current = web_attr(node, "style");
    if (!current)
        return;
    WebDecl *decls = parse_declarations(current);
    for (WebDecl *d = decls; d; d = d->next)
        if (!strcmp(d->name, name)) {
            append(out, d->value, cap);
            break;
        }
    decl_free_list(decls);
}
void css_remove_property(WebNode *node, const char *name) {
    if (!node || !name)
        return;
    char rebuilt[1024];
    inline_style_without(node, name, rebuilt, sizeof(rebuilt));
    web_set_attr(node, "style", rebuilt);
}
