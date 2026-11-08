/* HTML parser and DOM tree for the Luna Browser engine. */
#include "webengine.h"

/* The kernel runtime provides the allocator used by the engine. */
void *malloc(size_t n);
void free(void *p);

void *web_malloc(size_t n) {
    void *p = malloc(n);
    if (!p)
        kernel_fault();
    return p;
}
void web_free(void *p) { free(p); }
char *web_strdup(const char *s) {
    if (!s)
        return 0;
    size_t n = strlen(s);
    char *p = web_malloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}
char *web_strndup(const char *s, size_t n) {
    char *p = web_malloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}
int web_strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a;
        char cb = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (ca != cb)
            return ca < cb ? -1 : 1;
        a++;
        b++;
    }
    if (*a == *b)
        return 0;
    return *a ? 1 : -1;
}

WebNode *web_node_new(unsigned char type) {
    WebNode *n = web_malloc(sizeof(WebNode));
    memset(n, 0, sizeof(WebNode));
    n->type = type;
    if (type == WEB_DOCUMENT_FRAGMENT || type == WEB_SHADOW_ROOT)
        n->tag = web_strdup("#document-fragment");
    return n;
}
/* DOM tree invariants: a node has one parent, never its descendant.
 * Detached nodes remain owned by the caller until explicitly destroyed. */
bool web_node_contains(const WebNode *ancestor, const WebNode *node) {
    for (const WebNode *p = node; p; p = p->parent)
        if (p == ancestor)
            return true;
    return false;
}
void web_node_detach(WebNode *node) {
    if (!node || !node->parent)
        return;
    WebNode *parent = node->parent;
    if (node->prev)
        node->prev->next = node->next;
    else
        parent->first_child = node->next;
    if (node->next)
        node->next->prev = node->prev;
    else
        parent->last_child = node->prev;
    node->parent = node->next = node->prev = 0;
}
bool web_node_insert_before(WebNode *parent, WebNode *child, WebNode *reference) {
    if (!parent || !child || web_node_contains(child, parent) ||
        child->type == WEB_SHADOW_ROOT)
        return false;
    if (reference && reference->parent != parent)
        return false;
    if (reference == child)
        return true;
    if (child->type == WEB_DOCUMENT_FRAGMENT) {
        /* DOM fragment insertion moves its children, not the fragment. */
        for (WebNode *n = child->first_child; n; n = n->next)
            if (web_node_contains(n, parent))
                return false;
        while (child->first_child) {
            WebNode *n = child->first_child;
            web_node_detach(n);
            if (!web_node_insert_before(parent, n, reference))
                return false;
        }
        return true;
    }
    web_node_detach(child);
    child->parent = parent;
    if (!reference) {
        child->prev = parent->last_child;
        if (parent->last_child)
            parent->last_child->next = child;
        else
            parent->first_child = child;
        parent->last_child = child;
        child->next = 0;
    } else {
        child->next = reference;
        child->prev = reference->prev;
        if (reference->prev)
            reference->prev->next = child;
        else
            parent->first_child = child;
        reference->prev = child;
    }
    return true;
}
/* DOM Core helpers. Shadow roots are separate trees; normal parent links
 * intentionally do not traverse to their host. */
WebNode *web_node_root(WebNode *node) {
    if (!node)
        return 0;
    while (node->parent)
        node = node->parent;
    return node;
}
bool web_node_is_connected(const WebNode *node) {
    if (!node)
        return false;
    while (node->parent)
        node = node->parent;
    return node->tag && !strcmp(node->tag, "#document");
}
bool web_node_replace_child(WebNode *parent, WebNode *replacement, WebNode *old_child) {
    if (!parent || !replacement || !old_child || old_child->parent != parent)
        return false;
    if (replacement == old_child)
        return true;
    if (!web_node_insert_before(parent, replacement, old_child))
        return false;
    web_node_detach(old_child);
    return true;
}
/* Merge adjacent text nodes and drop empty text nodes without losing text. */
void web_node_normalize(WebNode *parent) {
    if (!parent)
        return;
    WebNode *node = parent->first_child;
    while (node) {
        if (node->type == WEB_TEXT) {
            while (node->next && node->next->type == WEB_TEXT) {
                WebNode *other = node->next;
                size_t a = node->text ? strlen(node->text) : 0;
                size_t b = other->text ? strlen(other->text) : 0;
                char *combined = web_malloc(a + b + 1);
                if (a)
                    memcpy(combined, node->text, a);
                if (b)
                    memcpy(combined + a, other->text, b);
                combined[a + b] = 0;
                web_free(node->text);
                node->text = combined;
                web_node_detach(other);
                web_free_tree(other);
            }
            if (!node->text || !node->text[0]) {
                WebNode *next = node->next;
                web_node_detach(node);
                web_free_tree(node);
                node = next;
                continue;
            }
        } else {
            web_node_normalize(node);
        }
        node = node->next;
    }
}

void web_node_append(WebNode *parent, WebNode *child) {
    (void)web_node_insert_before(parent, child, 0);
}
const char *web_attr(const WebNode *node, const char *name) {
    for (WebAttr *a = node->attrs; a; a = a->next)
        if (!web_strcasecmp(a->name, name))
            return a->value;
    return 0;
}
bool web_attr_has(const WebNode *node, const char *name) {
    return web_attr(node, name) != 0;
}
void web_set_attr(WebNode *node, const char *name, const char *value) {
    for (WebAttr *a = node->attrs; a; a = a->next) {
        if (!web_strcasecmp(a->name, name)) {
            web_free(a->value);
            a->value = web_strdup(value ? value : "");
            return;
        }
    }
    WebAttr *a = web_malloc(sizeof(WebAttr));
    a->name = web_strdup(name);
    a->value = web_strdup(value ? value : "");
    a->next = node->attrs;
    node->attrs = a;
}
/* Clone a node and, optionally, its descendants without sharing owned data.
 * Layout, decoded image buffers and JavaScript state are intentionally not copied. */
WebNode *web_node_clone(const WebNode *source, bool deep) {
    if (!source)
        return 0;
    WebNode *copy = web_node_new(source->type);
    web_free(copy->tag); /* fragment constructor installs a default tag */
    copy->tag = web_strdup(source->tag);
    copy->namespace_id = source->namespace_id;
    copy->text = web_strdup(source->text);
    for (const WebAttr *a = source->attrs; a; a = a->next)
        web_set_attr(copy, a->name, a->value);
    if (deep) {
        for (const WebNode *child = source->first_child; child; child = child->next)
            web_node_append(copy, web_node_clone(child, true));
    }
    if (source->template_content) {
        copy->template_content = web_node_clone(source->template_content, deep);
    }
    /* Shadow trees are intentionally not implicitly cloned. */
    return copy;
}
WebNode *web_attach_shadow(WebNode *host, bool open_mode) {
    if (!host || host->type != WEB_ELEMENT || host->shadow_root ||
        host->namespace_id != WEB_NS_HTML)
        return 0;
    WebNode *root = web_node_new(WEB_SHADOW_ROOT);
    root->host = host;
    host->shadow_root = root;
    host->shadow_open = open_mode;
    return root;
}
void web_free_tree(WebNode *node) {
    if (!node)
        return;
    WebNode *child = node->first_child;
    while (child) {
        WebNode *next = child->next;
        web_free_tree(child);
        child = next;
    }
    web_free_tree(node->template_content);
    web_free_tree(node->shadow_root);
    WebAttr *a = node->attrs;
    while (a) {
        WebAttr *next = a->next;
        web_free(a->name);
        web_free(a->value);
        web_free(a);
        a = next;
    }
    web_free(node->image);
    web_free(node->tag);
    web_free(node->text);
    web_free(node);
}
static void collect_text(WebNode *node, char *out, size_t cap) {
    if (!node)
        return;
    if (node->type == WEB_TEXT) {
        if (node->text)
            append(out, node->text, cap);
        return;
    }
    for (WebNode *c = node->first_child; c; c = c->next)
        collect_text(c, out, cap);
}
void web_node_text(WebNode *node, char *out, size_t cap) {
    if (!cap)
        return;
    out[0] = 0;
    collect_text(node, out, cap);
}
WebNode *web_find_by_id(WebNode *root, const char *id) {
    if (!root || !id || !*id)
        return 0;
    if (root->type == WEB_ELEMENT) {
        const char *value = web_attr(root, "id");
        if (value && !strcmp(value, id))
            return root;
    }
    for (WebNode *c = root->first_child; c; c = c->next) {
        WebNode *found = web_find_by_id(c, id);
        if (found)
            return found;
    }
    return 0;
}
WebNode *web_find_first(WebNode *root, const char *tag) {
    if (!root)
        return 0;
    if (root->type == WEB_ELEMENT && !strcmp(root->tag, tag))
        return root;
    for (WebNode *c = root->first_child; c; c = c->next) {
        WebNode *found = web_find_first(c, tag);
        if (found)
            return found;
    }
    return 0;
}

static const char *const void_tags[] = {"area", "base",  "br",   "col",   "embed",  "hr",
                                        "img",  "input", "link", "meta",  "param",  "source",
                                        "track", "wbr",  0};
static const char *const raw_tags[] = {"script", "style", "textarea", "title", 0};
static const char *const block_tags[] = {"address", "article", "aside", "blockquote", "details",
                                         "div",     "dl",      "footer", "form",    "h1",
                                         "h2",      "h3",      "h4",     "h5",      "h6",
                                         "header",  "hr",      "main",   "nav",     "ol",
                                         "p",       "pre",     "section", "table",  "ul", 0};
bool web_is_void_tag(const char *tag) {
    for (int i = 0; void_tags[i]; i++)
        if (!strcmp(tag, void_tags[i]))
            return true;
    return false;
}
static bool is_raw_tag(const char *tag) {
    for (int i = 0; raw_tags[i]; i++)
        if (!strcmp(tag, raw_tags[i]))
            return true;
    return false;
}
static bool is_block_tag(const char *tag) {
    for (int i = 0; block_tags[i]; i++)
        if (!strcmp(tag, block_tags[i]))
            return true;
    return false;
}

/* Full WHATWG-compatible named character reference data, including the
 * legacy forms without a semicolon and two-codepoint references. */
typedef struct {
    const char *name;
    uint32_t first, second;
} WebNamedEntity;
#include "web_entities.inc"
static const WebNamedEntity *find_named_entity(const char *name, size_t n) {
    if (!n || n > 32)
        return 0;
    char key[33];
    memcpy(key, name, n);
    key[n] = 0;
    int lo = 0, hi = (int)(sizeof(web_named_entities) / sizeof(web_named_entities[0])) - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = strcmp(key, web_named_entities[mid].name);
        if (!cmp)
            return &web_named_entities[mid];
        if (cmp < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return 0;
}
static bool entity_alnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}
static void utf8_encode(char *out, unsigned code) {
    if (code < 0x80) {
        out[0] = (char)code;
        out[1] = 0;
    } else if (code < 0x800) {
        out[0] = (char)(0xc0 | (code >> 6));
        out[1] = (char)(0x80 | (code & 0x3f));
        out[2] = 0;
    } else if (code < 0x10000) {
        out[0] = (char)(0xe0 | (code >> 12));
        out[1] = (char)(0x80 | ((code >> 6) & 0x3f));
        out[2] = (char)(0x80 | (code & 0x3f));
        out[3] = 0;
    } else {
        out[0] = (char)(0xf0 | (code >> 18));
        out[1] = (char)(0x80 | ((code >> 12) & 0x3f));
        out[2] = (char)(0x80 | ((code >> 6) & 0x3f));
        out[3] = (char)(0x80 | (code & 0x3f));
        out[4] = 0;
    }
}
static bool is_hex(char c) {
    return (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'f');
}
/* Decodes entities in place into a fresh buffer. */
static char *decode_entities(const char *src, size_t len, size_t *out_len,
                             bool attribute_context) {
    /* Some references expand into two UTF-8 codepoints and can be longer
     * than their encoded spelling (e.g. &acE;). */
    if (len > ((size_t)-1 - 1) / 2)
        kernel_fault();
    char *out = web_malloc(len * 2 + 1);
    size_t o = 0;
    for (size_t i = 0; i < len;) {
        if (src[i] != '&') {
            out[o++] = src[i++];
            continue;
        }
        size_t j = i + 1;
        if (j < len && src[j] == '#') {
            j++;
            bool hex = false, digits = false, overflow = false;
            uint32_t code = 0;
            if (j < len && (src[j] == 'x' || src[j] == 'X')) {
                hex = true;
                j++;
            }
            while (j < len && (hex ? is_hex(src[j]) : (src[j] >= '0' && src[j] <= '9'))) {
                unsigned digit = (unsigned)(src[j] <= '9' ? src[j] - '0' :
                                                  (src[j] | 32) - 'a' + 10);
                unsigned base = hex ? 16u : 10u;
                if (code > (0x10ffffu - digit) / base)
                    overflow = true;
                else if (!overflow)
                    code = code * base + digit;
                digits = true;
                j++;
            }
            if (!digits) {
                out[o++] = '&';
                i++;
                continue;
            }
            if (j < len && src[j] == ';')
                j++;
            if (overflow || !code || code > 0x10ffffu ||
                (code >= 0xd800u && code <= 0xdfffu))
                code = 0xfffdu;
            /* Numeric references in the C1 range are remapped by HTML. */
            if (code >= 0x80u && code <= 0x9fu) {
                static const uint16_t c1[] = {
                    8364,0,8218,402,8222,8230,8224,8225,
                    710,8240,352,8249,338,0,381,0,
                    0,8216,8217,8220,8221,8226,8211,8212,
                    732,8482,353,8250,339,0,382,376
                };
                if (c1[code - 0x80u])
                    code = c1[code - 0x80u];
            }
            char tmp[5];
            utf8_encode(tmp, code);
            size_t n = strlen(tmp);
            memcpy(out + o, tmp, n);
            o += n;
            i = j;
            continue;
        }
        const WebNamedEntity *matched = 0;
        size_t matched_len = 0, limit = len - j;
        if (limit > 32)
            limit = 32;
        for (size_t n = 1; n <= limit; n++) {
            char c = src[j + n - 1];
            if (!entity_alnum(c) && c != ';')
                break;
            const WebNamedEntity *candidate = find_named_entity(src + j, n);
            if (candidate) {
                matched = candidate;
                matched_len = n;
            }
            if (c == ';')
                break;
        }
        if (matched) {
            bool semicolon = matched->name[matched_len - 1] == ';';
            char after = j + matched_len < len ? src[j + matched_len] : 0;
            if (!attribute_context || semicolon || (!entity_alnum(after) && after != '=')) {
                char tmp[5];
                utf8_encode(tmp, matched->first);
                size_t n = strlen(tmp);
                memcpy(out + o, tmp, n);
                o += n;
                if (matched->second) {
                    utf8_encode(tmp, matched->second);
                    n = strlen(tmp);
                    memcpy(out + o, tmp, n);
                    o += n;
                }
                i = j + matched_len;
                continue;
            }
        }
        out[o++] = '&';
        i++;
    }
    out[o] = 0;
    *out_len = o;
    return out;
}

typedef struct {
    const char *p, *end;
    WebNode *open[64];
    int depth;
    bool preformatted;
    char *error;
    size_t ecap;
    bool failed;
} WebParser;

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static bool name_char(char c) {
    return is_alpha(c) || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':';
}
static WebNode *current(WebParser *ps) {
    if (!ps->depth)
        return 0;
    WebNode *top = ps->open[ps->depth - 1];
    return top->template_content ? top->template_content : top;
}

static void add_text(WebParser *ps, const char *text, size_t len) {
    if (!len)
        return;
    WebNode *parent = current(ps);
    if (!parent)
        return;
    size_t value_len;
    /* Whitespace belongs to the DOM. CSS white-space decides how it is
     * painted, including text nodes between inline elements. */
    char *value = decode_entities(text, len, &value_len, false);
    WebNode *node = web_node_new(WEB_TEXT);
    node->text = value;
    web_node_append(parent, node);
}

static void push(WebParser *ps, WebNode *node) {
    if (ps->depth >= 63) {
        ps->failed = true;
        if (ps->error)
            append(ps->error, "Document nesting is too deep.", ps->ecap);
        return;
    }
    ps->open[ps->depth++] = node;
    if (!strcmp(node->tag, "pre") || !strcmp(node->tag, "textarea"))
        ps->preformatted = true;
}
static void pop(WebParser *ps) {
    if (!ps->depth)
        return;
    WebNode *node = ps->open[--ps->depth];
    if (!strcmp(node->tag, "pre") || !strcmp(node->tag, "textarea")) {
        ps->preformatted = false;
        for (int i = 0; i < ps->depth; i++)
            if (!strcmp(ps->open[i]->tag, "pre") || !strcmp(ps->open[i]->tag, "textarea"))
                ps->preformatted = true;
    }
}
/* Returns true when the open element must be closed by the incoming tag. */
static bool implied_close(const char *open, const char *incoming) {
    if (!strcmp(open, "p"))
        return is_block_tag(incoming);
    if (!strcmp(open, "li"))
        return !strcmp(incoming, "li");
    if (!strcmp(open, "rt") || !strcmp(open, "rp"))
        return !strcmp(incoming, "rt") || !strcmp(incoming, "rp");
    if (!strcmp(open, "optgroup"))
        return !strcmp(incoming, "optgroup");
    if (!strcmp(open, "dt") || !strcmp(open, "dd"))
        return !strcmp(incoming, "dt") || !strcmp(incoming, "dd");
    if (!strcmp(open, "option"))
        return !strcmp(incoming, "option") || !strcmp(incoming, "optgroup");
    if (!strcmp(open, "td") || !strcmp(open, "th"))
        return !strcmp(incoming, "td") || !strcmp(incoming, "th") || !strcmp(incoming, "tr");
    if (!strcmp(open, "tr"))
        return !strcmp(incoming, "tr") || !strcmp(incoming, "thead") || !strcmp(incoming, "tbody");
    if (!strcmp(open, "thead") || !strcmp(open, "tbody") || !strcmp(open, "tfoot"))
        return !strcmp(incoming, "thead") || !strcmp(incoming, "tbody") ||
               !strcmp(incoming, "tfoot");
    if (!strcmp(open, "colgroup"))
        return !strcmp(incoming, "colgroup");
    if (!strcmp(open, "h1") || !strcmp(open, "h2") || !strcmp(open, "h3") ||
        !strcmp(open, "h4") || !strcmp(open, "h5") || !strcmp(open, "h6"))
        return !strcmp(incoming, "h1") || !strcmp(incoming, "h2") ||
               !strcmp(incoming, "h3") || !strcmp(incoming, "h4") ||
               !strcmp(incoming, "h5") || !strcmp(incoming, "h6");
    return false;
}
static void close_implied(WebParser *ps, const char *incoming) {
    while (ps->depth) {
        WebNode *top = ps->open[ps->depth - 1];
        if (!implied_close(top->tag, incoming))
            break;
        pop(ps);
    }
}
static void close_to(WebParser *ps, const char *tag) {
    int match = -1;
    for (int i = ps->depth - 1; i >= 0; i--)
        if (!strcmp(ps->open[i]->tag, tag)) {
            match = i;
            break;
        }
    if (match < 0)
        return;
    while (ps->depth > match)
        pop(ps);
}

static void parse_attributes(WebParser *ps, WebNode *node) {
    while (ps->p < ps->end) {
        while (ps->p < ps->end && is_space(*ps->p))
            ps->p++;
        if (ps->p >= ps->end || *ps->p == '>' || (*ps->p == '/' && ps->p + 1 < ps->end && ps->p[1] == '>'))
            break;
        const char *name_start = ps->p;
        while (ps->p < ps->end && !is_space(*ps->p) && *ps->p != '=' &&
               *ps->p != '>' && !(*ps->p == '/' && ps->p + 1 < ps->end && ps->p[1] == '>'))
            ps->p++;
        size_t name_len = (size_t)(ps->p - name_start);
        if (!name_len) {
            ps->p++;
            continue;
        }
        char *name = web_strndup(name_start, name_len);
        for (char *q = name; *q; q++)
            if (*q >= 'A' && *q <= 'Z')
                *q = (char)(*q + 32);
        char *value = 0;
        while (ps->p < ps->end && is_space(*ps->p))
            ps->p++;
        if (ps->p < ps->end && *ps->p == '=') {
            ps->p++;
            while (ps->p < ps->end && is_space(*ps->p))
                ps->p++;
            if (ps->p < ps->end && (*ps->p == '"' || *ps->p == '\'')) {
                char quote = *ps->p++;
                const char *value_start = ps->p;
                while (ps->p < ps->end && *ps->p != quote)
                    ps->p++;
                size_t raw_len = (size_t)(ps->p - value_start);
                if (ps->p < ps->end)
                    ps->p++;
                size_t decoded_len;
                value = decode_entities(value_start, raw_len, &decoded_len, true);
            } else {
                const char *value_start = ps->p;
                while (ps->p < ps->end && !is_space(*ps->p) && *ps->p != '>')
                    ps->p++;
                size_t raw_len = (size_t)(ps->p - value_start);
                size_t decoded_len;
                value = decode_entities(value_start, raw_len, &decoded_len, true);
            }
        } else {
            value = web_strdup("");
        }
        /* HTML uses the first occurrence of a duplicate attribute. */
        if (!web_attr_has(node, name))
            web_set_attr(node, name, value);
        web_free(name);
        web_free(value);
    }
}

static void parse_start_tag(WebParser *ps) {
    ps->p++; /* '<' */
    const char *name_start = ps->p;
    while (ps->p < ps->end && name_char(*ps->p))
        ps->p++;
    size_t name_len = (size_t)(ps->p - name_start);
    if (!name_len) {
        add_text(ps, "<", 1);
        return;
    }
    char *tag = web_strndup(name_start, name_len);
    for (char *q = tag; *q; q++)
        if (*q >= 'A' && *q <= 'Z')
            *q = (char)(*q + 32);
    close_implied(ps, tag);
    WebNode *node = web_node_new(WEB_ELEMENT);
    node->tag = tag;
    WebNode *parent = current(ps);
    if (parent) {
        node->namespace_id = parent->namespace_id;
        if (parent->namespace_id == WEB_NS_SVG &&
            (!strcmp(parent->tag, "foreignobject") ||
             !strcmp(parent->tag, "desc") ||
             !strcmp(parent->tag, "title")))
            node->namespace_id = WEB_NS_HTML;
        if (!strcmp(tag, "svg") && node->namespace_id == WEB_NS_HTML)
            node->namespace_id = WEB_NS_SVG;
        else if (!strcmp(tag, "math") && node->namespace_id == WEB_NS_HTML)
            node->namespace_id = WEB_NS_MATHML;
    }
    if (!parent) {
        web_free_tree(node);
        return;
    }
    web_node_append(parent, node);
    parse_attributes(ps, node);
    bool self_closing = false;
    if (ps->p < ps->end && *ps->p == '/') {
        ps->p++;
        self_closing = true;
    }
    if (ps->p < ps->end && *ps->p == '>')
        ps->p++;
    /* A trailing slash is ignored on non-void HTML elements. */
    (void)self_closing;
    if (web_is_void_tag(node->tag))
        return;
    if (node->namespace_id == WEB_NS_HTML && !strcmp(node->tag, "template"))
        node->template_content = web_node_new(WEB_DOCUMENT_FRAGMENT);
    if (is_raw_tag(node->tag)) {
        /* Raw script/style and RCDATA textarea/title end at a matching,
         * ASCII-case-insensitive end tag; whitespace before '>' is allowed. */
        const char *hit = 0, *after = 0;
        size_t n = strlen(node->tag);
        for (const char *q = ps->p; q + n + 2 <= ps->end; q++) {
            if (q[0] != '<' || q[1] != '/')
                continue;
            size_t k = 0;
            while (k < n && q + 2 + k < ps->end) {
                char ch = q[2 + k];
                if (ch >= 'A' && ch <= 'Z')
                    ch = (char)(ch + 32);
                if (ch != node->tag[k])
                    break;
                k++;
            }
            if (k != n)
                continue;
            const char *tail = q + 2 + n;
            if (tail < ps->end && *tail != '>' && !is_space(*tail))
                continue;
            while (tail < ps->end && is_space(*tail))
                tail++;
            if (tail < ps->end && *tail == '>') {
                hit = q;
                after = tail + 1;
                break;
            }
        }
        const char *content_end = hit ? hit : ps->end;
        if (content_end > ps->p) {
            WebNode *text = web_node_new(WEB_TEXT);
            size_t length = (size_t)(content_end - ps->p);
            if (!strcmp(node->tag, "textarea") || !strcmp(node->tag, "title")) {
                size_t decoded_len;
                text->text = decode_entities(ps->p, length, &decoded_len, false);
            } else {
                text->text = web_strndup(ps->p, length);
            }
            web_node_append(node, text);
        }
        ps->p = hit ? after : ps->end;
        return;
    }
    push(ps, node);
}

static void parse_end_tag(WebParser *ps) {
    ps->p += 2;
    const char *name_start = ps->p;
    while (ps->p < ps->end && name_char(*ps->p))
        ps->p++;
    size_t name_len = (size_t)(ps->p - name_start);
    char *tag = web_strndup(name_start, name_len);
    for (char *q = tag; *q; q++)
        if (*q >= 'A' && *q <= 'Z')
            *q = (char)(*q + 32);
    while (ps->p < ps->end && *ps->p != '>')
        ps->p++;
    if (ps->p < ps->end)
        ps->p++;
    close_to(ps, tag);
    web_free(tag);
}

#ifdef LUNA_USE_LEXBOR
/* The Lexbor tokenizer and tree builder are authoritative when enabled. */
bool web_parse_lexbor(const char *html, size_t len, WebNode **root,
                      char *error, size_t ecap);
#endif
bool web_parse(const char *html, size_t len, WebNode **root, char *error, size_t ecap) {
#ifdef LUNA_USE_LEXBOR
    return web_parse_lexbor(html, len, root, error, ecap);
#else
    WebParser ps;
    memset(&ps, 0, sizeof(ps));
    ps.p = html;
    ps.end = html + len;
    ps.error = error;
    ps.ecap = ecap;
    WebNode *document = web_node_new(WEB_ELEMENT);
    document->tag = web_strdup("#document");
    ps.open[ps.depth++] = document;
    while (ps.p < ps.end && !ps.failed) {
        if (*ps.p == '<') {
            if (ps.p + 3 < ps.end && !strncmp(ps.p, "<!--", 4)) {
                const char *begin = ps.p + 4;
                const char *end = begin;
                while (end + 2 < ps.end && strncmp(end, "-->", 3))
                    end++;
                bool closed = end + 2 < ps.end;
                const char *content_end = closed ? end : ps.end;
                WebNode *comment = web_node_new(WEB_COMMENT);
                comment->text = web_strndup(begin, (size_t)(content_end - begin));
                web_node_append(current(&ps), comment);
                ps.p = closed ? end + 3 : ps.end;
                continue;
            }
            if (ps.p + 1 < ps.end && ps.p[1] == '!') {
                const char *end = ps.p + 2;
                while (end < ps.end && *end != '>')
                    end++;
                ps.p = end < ps.end ? end + 1 : ps.end;
                continue;
            }
            if (ps.p + 1 < ps.end && ps.p[1] == '/') {
                parse_end_tag(&ps);
                continue;
            }
            if (ps.p + 1 < ps.end && is_alpha(ps.p[1])) {
                parse_start_tag(&ps);
                continue;
            }
            add_text(&ps, "<", 1);
            ps.p++;
            continue;
        }
        const char *start = ps.p;
        while (ps.p < ps.end && *ps.p != '<')
            ps.p++;
        add_text(&ps, start, (size_t)(ps.p - start));
    }
    *root = document;
    if (ps.failed && error)
        append(error, "Document structure was truncated.", ecap);
    return !ps.failed;
#endif
}
