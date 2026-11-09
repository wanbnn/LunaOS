/* QuickJS bindings for the Luna Browser DOM. Scripts run inside the kernel
 * service with a memory and time budget, the same runtime used by lunaSH. */
#include "quickjs.h"
#include "heap.h"
#include "web.h"
#include "webengine.h"

typedef struct WebListener {
    char *type;
    JSValue fn;
    struct WebListener *next;
} WebListener;

typedef struct WebJsState {
    JSValue object;
    WebListener *listeners;
} WebJsState;

typedef struct WebTimer {
    JSValue fn;
    int id;
    uint32_t due;
    uint32_t interval;
    bool cancelled;
    struct WebTimer *next;
} WebTimer;
typedef struct WebMediaListener {
    JSValue query, callback;
    bool matches;
    struct WebMediaListener *next;
} WebMediaListener;

static JSRuntime *js_runtime;
static JSContext *js_context;
static JSClassID js_node_class, js_style_class, js_media_class;
static WebNode *js_document;
static WebNode *js_current_script;
static WebTimer *js_timers;
static WebMediaListener *js_media;
static bool js_resize_pending;
/* Keep detached nodes alive for JS references until document teardown.
 * Reattached nodes are removed from this list so the document owns them. */
typedef struct WebDetachedNode {
    WebNode *node;
    struct WebDetachedNode *next;
} WebDetachedNode;
static WebDetachedNode *js_detached;
static void untrack_detached(WebNode *node) {
    WebDetachedNode **link = &js_detached;
    while (*link) {
        if ((*link)->node == node) {
            WebDetachedNode *entry = *link;
            *link = entry->next;
            web_free(entry);
            return;
        }
        link = &(*link)->next;
    }
}
static void track_detached(WebNode *node) {
    for (WebDetachedNode *it = js_detached; it; it = it->next)
        if (it->node == node)
            return;
    WebDetachedNode *entry = web_malloc(sizeof(*entry));
    entry->node = node;
    entry->next = js_detached;
    js_detached = entry;
}
static int js_timer_id;
static bool js_document_dirty;
static const char *js_ready_state;
static uint32_t js_deadline;

static int interrupt_js(JSRuntime *rt, void *opaque) {
    (void)rt;
    (void)opaque;
    return (int32_t)(frame_clock - js_deadline) >= 0;
}

static WebNode *node_from_value(JSContext *ctx, JSValueConst value) {
    (void)ctx;
    return JS_GetOpaque(value, js_node_class);
}
static WebJsState *state_of(WebNode *node) {
    if (!node)
        return 0;
    return (WebJsState *)node->js;
}

static void install_element(JSContext *ctx, JSValue object);

static JSValue make_element(JSContext *ctx, WebNode *node) {
    WebJsState *state = state_of(node);
    if (state && !JS_IsUndefined(state->object))
        return JS_DupValue(ctx, state->object);
    JSValue object = JS_NewObjectClass(ctx, js_node_class);
    if (JS_IsException(object))
        return object;
    JS_SetOpaque(object, node);
    if (!state) {
        state = web_malloc(sizeof(WebJsState));
        memset(state, 0, sizeof(*state));
        state->object = JS_UNDEFINED;
        node->js = state;
    }
    state->object = JS_DupValue(ctx, object);
    return object;
}

static void mark_dirty(void) { js_document_dirty = true; }
static void install_style(JSContext *ctx, JSValue object);

static bool remove_attribute(WebNode *node, const char *name) {
    for (WebAttr **link = &node->attrs; *link; link = &(*link)->next) {
        if (!web_strcasecmp((*link)->name, name)) {
            WebAttr *dead = *link;
            *link = dead->next;
            web_free(dead->name);
            web_free(dead->value);
            web_free(dead);
            return true;
        }
    }
    return false;
}

static void log_callback_exception(const char *context) {
    JSValue exception = JS_GetException(js_context);
    const char *message = JS_ToCString(js_context, exception);
    char line[256] = "";
    append(line, message ? message : "unknown error", sizeof(line));
    serial_write(context);
    serial_write(line);
    serial_write("\n");
    if (message)
        JS_FreeCString(js_context, message);
    if (JS_IsObject(exception)) {
        JSValue stack = JS_GetPropertyStr(js_context, exception, "stack");
        if (JS_IsString(stack)) {
            const char *text = JS_ToCString(js_context, stack);
            if (text) {
                line[0] = 0;
                append(line, text, sizeof(line));
                serial_write(line);
                serial_write("\n");
                JS_FreeCString(js_context, text);
            }
        }
        JS_FreeValue(js_context, stack);
    }
    JS_FreeValue(js_context, exception);
}

static WebNode *listener_node(JSContext *ctx, JSValueConst value) {
    WebNode *node = node_from_value(ctx, value);
    if (!node && JS_IsObject(value)) {
        JSValue global = JS_GetGlobalObject(ctx);
        if (JS_VALUE_GET_PTR(value) == JS_VALUE_GET_PTR(global))
            node = js_document;
        JS_FreeValue(ctx, global);
    }
    return node;
}

/* ---------- element properties ---------- */

enum {
    PROP_TAG_NAME = 0,
    PROP_ID,
    PROP_CLASS_NAME,
    PROP_TEXT_CONTENT,
    PROP_INNER_HTML,
    PROP_STYLE,
    PROP_CHILDREN,
    PROP_CHILD_COUNT,
    PROP_PARENT,
    PROP_HREF,
    PROP_TITLE,
    PROP_NODE_TYPE,
    PROP_NODE_NAME,
    PROP_NODE_VALUE,
    PROP_OWNER_DOCUMENT,
    PROP_PARENT_NODE,
    PROP_CHILD_NODES,
    PROP_CHECKED,
    PROP_DISABLED,
    PROP_OFFSET_WIDTH,
    PROP_OFFSET_HEIGHT,
    PROP_CSS_TEXT,
    PROP_IS_CONNECTED,
    PROP_VALUE,
    PROP_FIRST_CHILD,
    PROP_LAST_CHILD,
    PROP_NEXT_SIBLING,
    PROP_PREVIOUS_SIBLING,
    PROP_TEMPLATE_CONTENT,
    PROP_SHADOW_ROOT,
    PROP_READY_STATE,
    PROP_BODY,
    PROP_DOCUMENT_ELEMENT,
    PROP_HEAD,
    PROP_CURRENT_SCRIPT,
    PROP_LOCATION,
    PROP_INNER_WIDTH,
    PROP_INNER_HEIGHT,
    PROP_STYLE_BASE = 100
};

static const char *style_property_name(int magic) {
    switch (magic) {
        case PROP_STYLE_BASE + 0:
            return "color";
        case PROP_STYLE_BASE + 1:
            return "background-color";
        case PROP_STYLE_BASE + 2:
            return "display";
        case PROP_STYLE_BASE + 3:
            return "font-size";
        case PROP_STYLE_BASE + 4:
            return "font-weight";
        case PROP_STYLE_BASE + 5:
            return "text-align";
        case PROP_STYLE_BASE + 6:
            return "margin";
        case PROP_STYLE_BASE + 7:
            return "margin-top";
        case PROP_STYLE_BASE + 8:
            return "margin-right";
        case PROP_STYLE_BASE + 9:
            return "margin-bottom";
        case PROP_STYLE_BASE + 10:
            return "margin-left";
        case PROP_STYLE_BASE + 11:
            return "padding";
        case PROP_STYLE_BASE + 12:
            return "padding-top";
        case PROP_STYLE_BASE + 13:
            return "padding-right";
        case PROP_STYLE_BASE + 14:
            return "padding-bottom";
        case PROP_STYLE_BASE + 15:
            return "padding-left";
        case PROP_STYLE_BASE + 16:
            return "width";
        case PROP_STYLE_BASE + 17:
            return "height";
        case PROP_STYLE_BASE + 18:
            return "text-decoration";
        case PROP_STYLE_BASE + 19:
            return "visibility";
        case PROP_STYLE_BASE + 20:
            return "line-height";
        case PROP_STYLE_BASE + 21:
            return "background";
        case PROP_STYLE_BASE + 22:
            return "font-weight";
        case PROP_STYLE_BASE + 23:
            return "border";
        case PROP_STYLE_BASE + 24:
            return "overflow";
        case PROP_STYLE_BASE + 25:
            return "float";
        case PROP_STYLE_BASE + 26:
            return "opacity";
        default:
            return 0;
    }
}
static void color_string(char *out, uint32_t color) {
    static const char digits[] = "0123456789abcdef";
    out[0] = '#';
    for (int i = 0; i < 6; i++)
        out[1 + i] = digits[(color >> ((5 - i) * 4)) & 15];
    out[7] = 0;
}
static JSValue style_get(JSContext *ctx, WebNode *node, int magic) {
    const char *name = style_property_name(magic);
    if (!name)
        return JS_UNDEFINED;
    char declared[128];
    css_get_property(node, name, declared, sizeof(declared));
    if (*declared)
        return JS_NewString(ctx, declared);
    WebStyle *s = &node->style;
    char buffer[32];
    if (!strcmp(name, "color")) {
        color_string(buffer, s->color);
        return JS_NewString(ctx, buffer);
    }
    if (!strcmp(name, "background-color") || !strcmp(name, "background")) {
        if (!s->background)
            return JS_NewString(ctx, "transparent");
        color_string(buffer, s->background);
        return JS_NewString(ctx, buffer);
    }
    if (!strcmp(name, "display"))
        return JS_NewString(ctx, s->display == WEB_DISPLAY_NONE      ? "none"
                             : s->display == WEB_DISPLAY_INLINE      ? "inline"
                             : s->display == WEB_DISPLAY_INLINE_BLOCK ? "inline-block"
                                                                     : "block");
    if (!strcmp(name, "font-size")) {
        number(buffer, s->font_size);
        return JS_NewString(ctx, buffer);
    }
    if (!strcmp(name, "font-weight")) {
        number(buffer, s->font_weight);
        return JS_NewString(ctx, buffer);
    }
    if (!strcmp(name, "text-align"))
        return JS_NewString(ctx, s->text_align == WEB_ALIGN_CENTER ? "center"
                             : s->text_align == WEB_ALIGN_RIGHT    ? "right"
                                                                   : "left");
    if (!strcmp(name, "line-height")) {
        number(buffer, s->line_height ? s->line_height : s->font_size * 12 / 10);
        return JS_NewString(ctx, buffer);
    }
    if (!strcmp(name, "text-decoration"))
        return JS_NewString(ctx, s->underline ? "underline" : "none");
    if (!strcmp(name, "visibility"))
        return JS_NewString(ctx, s->hidden ? "hidden" : "visible");
    if (!strcmp(name, "width")) {
        number(buffer, s->width < 0 ? 0 : s->width);
        return JS_NewString(ctx, buffer);
    }
    if (!strcmp(name, "height")) {
        number(buffer, s->height < 0 ? 0 : s->height);
        return JS_NewString(ctx, buffer);
    }
    if (name[0] == 'm' || name[0] == 'p') {
        int index = 0;
        if (!strcmp(name, "margin-top") || !strcmp(name, "padding-top"))
            index = 0;
        else if (!strcmp(name, "margin-right") || !strcmp(name, "padding-right"))
            index = 1;
        else if (!strcmp(name, "margin-bottom") || !strcmp(name, "padding-bottom"))
            index = 2;
        else if (!strcmp(name, "margin-left") || !strcmp(name, "padding-left"))
            index = 3;
        else
            index = -1;
        if (index >= 0) {
            int value = name[0] == 'm' ? s->margin[index] : s->padding[index];
            number(buffer, value < 0 ? 0 : value);
            return JS_NewString(ctx, buffer);
        }
        number(buffer, 0);
        return JS_NewString(ctx, buffer);
    }
    return JS_NewString(ctx, "");
}
static JSValue element_getter(JSContext *ctx, JSValueConst this_val, int magic) {
    WebNode *node = magic == PROP_CSS_TEXT || magic >= PROP_STYLE_BASE ?
                    JS_GetOpaque(this_val, js_style_class) : node_from_value(ctx, this_val);
    if (!node)
        return JS_UNDEFINED;
    if (magic >= PROP_STYLE_BASE)
        return style_get(ctx, node, magic);
    char buffer[512];
    switch (magic) {
        case PROP_TAG_NAME: {
            const char *tag = node->tag ? node->tag : "";
            size_t i = 0;
            for (; tag[i] && i < sizeof(buffer) - 1; i++) {
                buffer[i] = tag[i] >= 'a' && tag[i] <= 'z' ? (char)(tag[i] - 32) : tag[i];
            }
            buffer[i] = 0;
            return JS_NewString(ctx, buffer);
        }
        case PROP_ID: {
            const char *id = web_attr(node, "id");
            return JS_NewString(ctx, id ? id : "");
        }
        case PROP_CLASS_NAME: {
            const char *value = web_attr(node, "class");
            return JS_NewString(ctx, value ? value : "");
        }
        case PROP_TEXT_CONTENT: {
            buffer[0] = 0;
            web_node_text(node, buffer, sizeof(buffer));
            return JS_NewString(ctx, buffer);
        }
        case PROP_INNER_HTML: {
            buffer[0] = 0;
            for (WebNode *c = node->first_child; c; c = c->next) {
                if (c->type == WEB_TEXT) {
                    append(buffer, c->text, sizeof(buffer));
                } else if (c->type == WEB_ELEMENT) {
                    char tag[64];
                    size_t i = 0;
                    for (; c->tag[i] && i < sizeof(tag) - 2; i++)
                        tag[i] = c->tag[i] >= 'A' && c->tag[i] <= 'Z' ? (char)(c->tag[i] + 32)
                                                                   : c->tag[i];
                    tag[i] = 0;
                    char open[80];
                    strcpy(open, "<");
                    strcat(open, tag);
                    strcat(open, ">");
                    append(buffer, open, sizeof(buffer));
                    char inner[512];
                    inner[0] = 0;
                    web_node_text(c, inner, sizeof(inner));
                    append(buffer, inner, sizeof(buffer));
                    char close[80];
                    strcpy(close, "</");
                    strcat(close, tag);
                    strcat(close, ">");
                    append(buffer, close, sizeof(buffer));
                }
            }
            return JS_NewString(ctx, buffer);
        }
        case PROP_STYLE: {
            JSValue object = JS_NewObjectClass(ctx, js_style_class);
            if (JS_IsException(object))
                return object;
            JS_SetOpaque(object, node);
            return object;
        }
        case PROP_CHILDREN:
        case PROP_CHILD_NODES: {
            JSValue array = JS_NewArray(ctx);
            uint32_t index = 0;
            for (WebNode *c = node->first_child; c; c = c->next)
                if (magic == PROP_CHILD_NODES || c->type == WEB_ELEMENT)
                    JS_SetPropertyUint32(ctx, array, index++, make_element(ctx, c));
            return array;
        }
        case PROP_CHILD_COUNT: {
            uint32_t count = 0;
            for (WebNode *c = node->first_child; c; c = c->next)
                if (c->type == WEB_ELEMENT)
                    count++;
            return JS_NewInt32(ctx, (int32_t)count);
        }
        case PROP_PARENT:
            if (node->parent && node->parent->type == WEB_ELEMENT)
                return make_element(ctx, node->parent);
            return JS_NULL;
        case PROP_PARENT_NODE:
            return node->parent ? make_element(ctx, node->parent) : JS_NULL;
        case PROP_OWNER_DOCUMENT:
            return node == js_document ? JS_NULL : make_element(ctx, js_document);
        case PROP_HREF: {
            const char *href = web_attr(node, "href");
            return JS_NewString(ctx, href ? href : "");
        }
        case PROP_TITLE: {
            const char *title = web_attr(node, "title");
            return JS_NewString(ctx, title ? title : "");
        }
        case PROP_NODE_TYPE:
            return JS_NewInt32(ctx, node == js_document ? 9 :
                                    node->type == WEB_ELEMENT ? 1 :
                                    node->type == WEB_TEXT ? 3 :
                                    node->type == WEB_COMMENT ? 8 : 11);
        case PROP_NODE_NAME:
            if (node->type == WEB_TEXT || node->type == WEB_COMMENT)
                return JS_NewString(ctx, node->type == WEB_TEXT ? "#text" : "#comment");
            if (node == js_document || node->type != WEB_ELEMENT)
                return JS_NewString(ctx, node->tag ? node->tag : "");
            return element_getter(ctx, this_val, PROP_TAG_NAME);
        case PROP_NODE_VALUE:
            return node->text ? JS_NewString(ctx, node->text) : JS_NULL;
        case PROP_CHECKED:
            return JS_NewBool(ctx, web_attr_has(node, "checked"));
        case PROP_DISABLED:
            return JS_NewBool(ctx, web_attr_has(node, "disabled"));
        case PROP_OFFSET_WIDTH:
            return JS_NewInt32(ctx, node->w);
        case PROP_OFFSET_HEIGHT:
            return JS_NewInt32(ctx, node->h);
        case PROP_CSS_TEXT: {
            const char *style = web_attr(node, "style");
            return JS_NewString(ctx, style ? style : "");
        }
        case PROP_IS_CONNECTED:
            return web_node_is_connected(node) ? JS_TRUE : JS_FALSE;
        case PROP_TEMPLATE_CONTENT:
            return node->template_content ? make_element(ctx, node->template_content) : JS_UNDEFINED;
        case PROP_SHADOW_ROOT:
            return node->shadow_root && node->shadow_open ?
                   make_element(ctx, node->shadow_root) : JS_NULL;
        case PROP_VALUE: {
            const char *value = web_attr(node, "value");
            return JS_NewString(ctx, value ? value : "");
        }
        case PROP_FIRST_CHILD:
            if (node->first_child)
                return make_element(ctx, node->first_child);
            return JS_NULL;
        case PROP_LAST_CHILD:
            if (node->last_child)
                return make_element(ctx, node->last_child);
            return JS_NULL;
        case PROP_NEXT_SIBLING:
            if (node->next)
                return make_element(ctx, node->next);
            return JS_NULL;
        case PROP_PREVIOUS_SIBLING:
            if (node->prev)
                return make_element(ctx, node->prev);
            return JS_NULL;
        default:
            return JS_UNDEFINED;
    }
}
static JSValue element_setter(JSContext *ctx, JSValueConst this_val, JSValueConst value,
                              int magic) {
    WebNode *node = magic == PROP_CSS_TEXT || magic >= PROP_STYLE_BASE ?
                    JS_GetOpaque(this_val, js_style_class) : node_from_value(ctx, this_val);
    if (!node)
        return JS_UNDEFINED;
    if (magic == PROP_CHECKED || magic == PROP_DISABLED) {
        const char *name = magic == PROP_CHECKED ? "checked" : "disabled";
        if (JS_ToBool(ctx, value))
            web_set_attr(node, name, "");
        else
            remove_attribute(node, name);
        if (web_node_is_connected(node))
            mark_dirty();
        return JS_UNDEFINED;
    }
    const char *text = JS_ToCString(ctx, value);
    if (!text)
        return JS_EXCEPTION;
    if (magic == PROP_INNER_HTML || magic == PROP_TEXT_CONTENT || magic == PROP_CSS_TEXT) {
        if (magic == PROP_CSS_TEXT) {
            web_set_attr(node, "style", text);
        } else if (magic == PROP_TEXT_CONTENT && (node->type == WEB_TEXT || node->type == WEB_COMMENT)) {
            web_free(node->text);
            node->text = web_strdup(text);
        } else {
            WebNode *fragment = 0;
            if (magic == PROP_INNER_HTML) {
                fragment = web_parse_fragment(node->tag ? node->tag : "div", text, strlen(text));
                if (!fragment) {
                    JS_FreeCString(ctx, text);
                    return JS_ThrowTypeError(ctx, "Cannot parse HTML fragment");
                }
            }
            while (node->first_child) {
                WebNode *child = node->first_child;
                web_node_detach(child);
                track_detached(child);
            }
            if (fragment) {
                while (fragment->first_child)
                    web_node_append(node, fragment->first_child);
                web_free_tree(fragment);
            } else if (*text) {
                WebNode *child = web_node_new(WEB_TEXT);
                child->text = web_strdup(text);
                web_node_append(node, child);
            }
        }
        JS_FreeCString(ctx, text);
        if (web_node_is_connected(node))
            mark_dirty();
        return JS_UNDEFINED;
    }
    char buffer[1024];
    size_t n = strlen(text);
    if (n >= sizeof(buffer))
        n = sizeof(buffer) - 1;
    memcpy(buffer, text, n);
    buffer[n] = 0;
    JS_FreeCString(ctx, text);
    if (magic >= PROP_STYLE_BASE) {
        const char *name = style_property_name(magic);
        if (name) {
            css_set_property(node, name, buffer);
            mark_dirty();
        }
        return JS_UNDEFINED;
    }
    switch (magic) {
        case PROP_ID:
            web_set_attr(node, "id", buffer);
            mark_dirty();
            break;
        case PROP_CLASS_NAME:
            web_set_attr(node, "class", buffer);
            mark_dirty();
            break;
        case PROP_HREF:
            web_set_attr(node, "href", buffer);
            mark_dirty();
            break;
        case PROP_TITLE:
            web_set_attr(node, "title", buffer);
            mark_dirty();
            break;
        case PROP_VALUE:
            web_set_attr(node, "value", buffer);
            mark_dirty();
            break;
        default:
            break;
    }
    return JS_UNDEFINED;
}

/* ---------- element methods ---------- */

static JSValue js_add_event_listener(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv) {
    WebNode *node = listener_node(ctx, this_val);
    if (!node || argc < 2)
        return JS_UNDEFINED;
    const char *type = JS_ToCString(ctx, argv[0]);
    if (!type)
        return JS_UNDEFINED;
    if (!JS_IsFunction(ctx, argv[1])) {
        JS_FreeCString(ctx, type);
        return JS_UNDEFINED;
    }
    char *type_copy = web_strdup(type);
    JS_FreeCString(ctx, type);
    WebJsState *state = state_of(node);
    if (!state) {
        state = web_malloc(sizeof(WebJsState));
        memset(state, 0, sizeof(*state));
        state->object = JS_UNDEFINED;
        node->js = state;
    }
    for (WebListener *l = state->listeners; l; l = l->next)
        if (!strcmp(l->type, type_copy) && JS_VALUE_GET_PTR(l->fn) == JS_VALUE_GET_PTR(argv[1])) {
            web_free(type_copy);
            return JS_UNDEFINED;
        }
    WebListener *listener = web_malloc(sizeof(WebListener));
    listener->type = type_copy;
    listener->fn = JS_DupValue(ctx, argv[1]);
    listener->next = state->listeners;
    state->listeners = listener;
    return JS_UNDEFINED;
}
static JSValue js_remove_event_listener(JSContext *ctx, JSValueConst this_val, int argc,
                                         JSValueConst *argv) {
    WebNode *node = listener_node(ctx, this_val);
    if (!node || argc < 2)
        return JS_UNDEFINED;
    const char *type = JS_ToCString(ctx, argv[0]);
    if (!type)
        return JS_UNDEFINED;
    WebJsState *state = state_of(node);
    WebListener **link = state ? &state->listeners : 0;
    while (link && *link) {
        WebListener *listener = *link;
        if (!strcmp(listener->type, type) &&
            JS_VALUE_GET_PTR(listener->fn) == JS_VALUE_GET_PTR(argv[1])) {
            *link = listener->next;
            JS_FreeValue(ctx, listener->fn);
            web_free(listener->type);
            web_free(listener);
            continue;
        }
        link = &listener->next;
    }
    JS_FreeCString(ctx, type);
    return JS_UNDEFINED;
}
static void collect_matching(WebNode *node, const char *selector, WebNode **out, int *count,
                             int limit) {
    if (!node || *count >= limit)
        return;
    if (node->type == WEB_ELEMENT && css_match_selector(selector, node))
        out[(*count)++] = node;
    for (WebNode *c = node->first_child; c; c = c->next)
        collect_matching(c, selector, out, count, limit);
}
static JSValue js_query_selector(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
    WebNode *root = node_from_value(ctx, this_val);
    if (!argc || !root)
        return JS_NULL;
    const char *selector = JS_ToCString(ctx, argv[0]);
    if (!selector)
        return JS_NULL;
    WebNode *matches[64];
    int count = 0;
    for (WebNode *child = root->first_child; child && !count; child = child->next)
        collect_matching(child, selector, matches, &count, 1);
    JS_FreeCString(ctx, selector);
    if (!count)
        return JS_NULL;
    return make_element(ctx, matches[0]);
}
static JSValue js_query_selector_all(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv) {
    WebNode *root = node_from_value(ctx, this_val);
    if (!argc || !root)
        return JS_NewArray(ctx);
    const char *selector = JS_ToCString(ctx, argv[0]);
    if (!selector)
        return JS_NewArray(ctx);
    WebNode *matches[256];
    int count = 0;
    for (WebNode *child = root->first_child; child && count < 256; child = child->next)
        collect_matching(child, selector, matches, &count, 256);
    JS_FreeCString(ctx, selector);
    JSValue array = JS_NewArray(ctx);
    for (int i = 0; i < count; i++)
        JS_SetPropertyUint32(ctx, array, (uint32_t)i, make_element(ctx, matches[i]));
    return array;
}
static bool collect_tags(JSContext *ctx, WebNode *root, const char *tag,
                         JSValueConst result, uint32_t *count) {
    for (WebNode *node = root->first_child; node; node = node->next) {
        if (node->type == WEB_ELEMENT && (!strcmp(tag, "*") || !web_strcasecmp(node->tag, tag)) &&
            JS_SetPropertyUint32(ctx, result, (*count)++, make_element(ctx, node)) < 0)
            return false;
        if (!collect_tags(ctx, node, tag, result, count))
            return false;
    }
    return true;
}
static JSValue js_get_elements_by_tag(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv) {
    WebNode *root = node_from_value(ctx, this_val);
    if (!root || !argc)
        return JS_NewArray(ctx);
    const char *tag = JS_ToCString(ctx, argv[0]);
    if (!tag)
        return JS_EXCEPTION;
    JSValue result = JS_NewArray(ctx);
    uint32_t count = 0;
    bool ok = !JS_IsException(result) && collect_tags(ctx, root, tag, result, &count);
    JS_FreeCString(ctx, tag);
    if (!ok) {
        JS_FreeValue(ctx, result);
        return JS_EXCEPTION;
    }
    return result;
}
static JSValue js_has_child_nodes(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
    (void)argc; (void)argv;
    WebNode *node = node_from_value(ctx, this_val);
    return JS_NewBool(ctx, node && node->first_child);
}
static JSValue js_matches(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    WebNode *node = node_from_value(ctx, this_val);
    if (!node || node->type != WEB_ELEMENT || !argc)
        return JS_FALSE;
    const char *selector = JS_ToCString(ctx, argv[0]);
    if (!selector)
        return JS_EXCEPTION;
    bool match = css_match_selector(selector, node);
    JS_FreeCString(ctx, selector);
    return JS_NewBool(ctx, match);
}
static JSValue js_element_constructor(JSContext *ctx, JSValueConst new_target, int argc,
                                      JSValueConst *argv) {
    (void)new_target; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}
static JSValue js_get_element_by_id(JSContext *ctx, JSValueConst this_val, int argc,
                                    JSValueConst *argv) {
    (void)this_val;
    if (!argc || !js_document)
        return JS_NULL;
    const char *id = JS_ToCString(ctx, argv[0]);
    if (!id)
        return JS_NULL;
    WebNode *found = web_find_by_id(js_document, id);
    JS_FreeCString(ctx, id);
    if (!found)
        return JS_NULL;
    return make_element(ctx, found);
}
static JSValue js_create_element(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
    (void)this_val;
    if (!argc)
        return JS_NULL;
    const char *tag = JS_ToCString(ctx, argv[0]);
    if (!tag)
        return JS_NULL;
    char name[64];
    size_t n = strlen(tag);
    if (n >= sizeof(name))
        n = sizeof(name) - 1;
    memcpy(name, tag, n);
    name[n] = 0;
    for (char *p = name; *p; p++)
        if (*p >= 'A' && *p <= 'Z')
            *p = (char)(*p + 32);
    JS_FreeCString(ctx, tag);
    WebNode *node = web_node_new(WEB_ELEMENT);
    node->tag = web_strdup(name);
    track_detached(node);
    return make_element(ctx, node);
}
static JSValue js_create_document_fragment(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    WebNode *node = web_node_new(WEB_DOCUMENT_FRAGMENT);
    track_detached(node);
    return make_element(ctx, node);
}
static JSValue js_create_character_node(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv, int type) {
    (void)this_val;
    if (!argc)
        return JS_NULL;
    const char *text = JS_ToCString(ctx, argv[0]);
    if (!text)
        return JS_NULL;
    WebNode *node = web_node_new(type);
    node->text = web_strdup(text);
    track_detached(node);
    JS_FreeCString(ctx, text);
    return make_element(ctx, node);
}
/* Tree mutations are centralized in web_dom.c. Reject moves that would
 * make an ancestor its own descendant instead of corrupting the DOM. */
static JSValue js_append_child(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
    WebNode *parent = node_from_value(ctx, this_val);
    WebNode *child = argc ? node_from_value(ctx, argv[0]) : 0;
    bool moving_connected = web_node_is_connected(child);
    if (!parent || !child || child == js_document || !web_node_insert_before(parent, child, 0))
        return JS_ThrowTypeError(ctx, "Invalid appendChild tree mutation");
    if (child->parent)
        untrack_detached(child);
    if (moving_connected || web_node_is_connected(parent))
        mark_dirty();
    return make_element(ctx, child);
}
static JSValue js_attach_shadow(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
    WebNode *host = node_from_value(ctx, this_val);
    if (!host || argc < 1)
        return JS_ThrowTypeError(ctx, "attachShadow requires an options object");
    JSValue mode_value = JS_GetPropertyStr(ctx, argv[0], "mode");
    const char *mode = JS_ToCString(ctx, mode_value);
    bool valid = mode && (!strcmp(mode, "open") || !strcmp(mode, "closed"));
    bool open_mode = mode && !strcmp(mode, "open");
    if (mode)
        JS_FreeCString(ctx, mode);
    JS_FreeValue(ctx, mode_value);
    if (!valid)
        return JS_ThrowTypeError(ctx, "ShadowRoot mode must be open or closed");
    WebNode *root = web_attach_shadow(host, open_mode);
    if (!root)
        return JS_ThrowTypeError(ctx, "Cannot attach ShadowRoot to this element");
    return make_element(ctx, root);
}
static JSValue js_clone_node(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
    WebNode *source = node_from_value(ctx, this_val);
    if (!source)
        return JS_NULL;
    bool deep = argc > 0 && JS_ToBool(ctx, argv[0]);
    WebNode *copy = web_node_clone(source, deep);
    track_detached(copy);
    return make_element(ctx, copy);
}
static JSValue js_insert_before(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
    WebNode *parent = node_from_value(ctx, this_val);
    WebNode *child = argc ? node_from_value(ctx, argv[0]) : 0;
    WebNode *reference = argc > 1 ? node_from_value(ctx, argv[1]) : 0;
    bool moving_connected = web_node_is_connected(child);
    if (!parent || !child || child == js_document || !web_node_insert_before(parent, child, reference))
        return JS_ThrowTypeError(ctx, "Invalid insertBefore tree mutation");
    if (child->parent)
        untrack_detached(child);
    if (moving_connected || web_node_is_connected(parent))
        mark_dirty();
    return make_element(ctx, child);
}
static JSValue js_remove_child(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
    WebNode *parent = node_from_value(ctx, this_val);
    WebNode *child = argc ? node_from_value(ctx, argv[0]) : 0;
    if (!parent || !child || child->parent != parent)
        return JS_ThrowTypeError(ctx, "removeChild requires a direct child");
    web_node_detach(child);
    track_detached(child);
    if (web_node_is_connected(parent))
        mark_dirty();
    return make_element(ctx, child);
}
static JSValue js_replace_child(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
    WebNode *parent = node_from_value(ctx, this_val);
    WebNode *next = argc > 0 ? node_from_value(ctx, argv[0]) : 0;
    WebNode *old = argc > 1 ? node_from_value(ctx, argv[1]) : 0;
    bool moving_connected = web_node_is_connected(next);
    if (!parent || !next || !old || next == js_document || !web_node_replace_child(parent, next, old))
        return JS_ThrowTypeError(ctx, "Invalid replaceChild tree mutation");
    if (next->parent)
        untrack_detached(next);
    if (next != old) {
        track_detached(old);
        if (moving_connected || web_node_is_connected(parent))
            mark_dirty();
    }
    return make_element(ctx, old);
}
static JSValue js_get_root_node(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
    WebNode *node = node_from_value(ctx, this_val);
    bool composed = false;
    if (argc > 0 && !JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0])) {
        JSValue option = JS_GetPropertyStr(ctx, argv[0], "composed");
        composed = JS_ToBool(ctx, option) > 0;
        JS_FreeValue(ctx, option);
    }
    WebNode *root = composed ? web_node_root_composed(node) : web_node_root(node);
    return root ? make_element(ctx, root) : JS_NULL;
}
static JSValue js_remove_element(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
    (void)ctx; (void)argc; (void)argv;
    WebNode *node = node_from_value(ctx, this_val);
    if (node && node->parent) {
        web_node_detach(node);
        track_detached(node);
        mark_dirty();
    }
    return JS_UNDEFINED;
}
static JSValue js_set_attribute(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
    WebNode *node = node_from_value(ctx, this_val);
    if (!node || argc < 2)
        return JS_UNDEFINED;
    const char *name = JS_ToCString(ctx, argv[0]);
    const char *value = JS_ToCString(ctx, argv[1]);
    if (name && value) {
        char lower[64];
        size_t n = strlen(name);
        if (n >= sizeof(lower))
            n = sizeof(lower) - 1;
        memcpy(lower, name, n);
        lower[n] = 0;
        for (char *p = lower; *p; p++)
            if (*p >= 'A' && *p <= 'Z')
                *p = (char)(*p + 32);
        web_set_attr(node, lower, value);
        if (web_node_is_connected(node))
            mark_dirty();
    }
    if (name)
        JS_FreeCString(ctx, name);
    if (value)
        JS_FreeCString(ctx, value);
    return JS_UNDEFINED;
}
static JSValue js_get_attribute(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
    WebNode *node = node_from_value(ctx, this_val);
    if (!node || argc < 1)
        return JS_NULL;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_NULL;
    const char *value = web_attr(node, name);
    JS_FreeCString(ctx, name);
    if (!value)
        return JS_NULL;
    return JS_NewString(ctx, value);
}
static JSValue js_has_attribute(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
    WebNode *node = node_from_value(ctx, this_val);
    if (!node || argc < 1)
        return JS_FALSE;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_FALSE;
    bool found = web_attr_has(node, name);
    JS_FreeCString(ctx, name);
    return found ? JS_TRUE : JS_FALSE;
}
static JSValue js_remove_attribute(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
    WebNode *node = node_from_value(ctx, this_val);
    if (!node || argc < 1)
        return JS_UNDEFINED;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (name) {
        if (remove_attribute(node, name) && web_node_is_connected(node))
            mark_dirty();
        JS_FreeCString(ctx, name);
    }
    return JS_UNDEFINED;
}
static JSValue js_get_bounding_rect(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv) {
    (void)argc;
    (void)argv;
    WebNode *node = node_from_value(ctx, this_val);
    JSValue rect = JS_NewObject(ctx);
    if (!node)
        return rect;
    JS_SetPropertyStr(ctx, rect, "x", JS_NewInt32(ctx, node->x));
    JS_SetPropertyStr(ctx, rect, "y", JS_NewInt32(ctx, node->y));
    JS_SetPropertyStr(ctx, rect, "width", JS_NewInt32(ctx, node->w));
    JS_SetPropertyStr(ctx, rect, "height", JS_NewInt32(ctx, node->h));
    JS_SetPropertyStr(ctx, rect, "top", JS_NewInt32(ctx, node->y));
    JS_SetPropertyStr(ctx, rect, "left", JS_NewInt32(ctx, node->x));
    return rect;
}
static void computed_pixels(JSContext *ctx, JSValueConst style, const char *name, int value) {
    char text[24];
    number(text, value < 0 ? 0 : (unsigned)value);
    strcat(text, "px");
    JS_SetPropertyStr(ctx, style, name, JS_NewString(ctx, text));
}
static JSValue computed_value(JSContext *ctx, JSValueConst style, int argc, JSValueConst *argv) {
    if (!argc)
        return JS_NewString(ctx, "");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    char property[64];
    size_t used = 0;
    for (const char *p = name; *p && used < sizeof(property) - 1; p++) {
        if (*p == '-' && p[1] >= 'a' && p[1] <= 'z') {
            p++;
            property[used++] = (char)(*p - 'a' + 'A');
        } else {
            property[used++] = *p;
        }
    }
    property[used] = 0;
    JS_FreeCString(ctx, name);
    JSValue value = JS_GetPropertyStr(ctx, style, property);
    if (JS_IsString(value) || JS_IsException(value))
        return value;
    JS_FreeValue(ctx, value);
    return JS_NewString(ctx, "");
}
static JSValue js_computed_style(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
    (void)this_val;
    WebNode *node = argc ? node_from_value(ctx, argv[0]) : 0;
    if (!node || node->type != WEB_ELEMENT || node == js_document)
        return JS_ThrowTypeError(ctx, "getComputedStyle requires an element");
    JSValue result = JS_NewObject(ctx);
    if (JS_IsException(result))
        return result;
    JS_SetPropertyStr(ctx, result, "getPropertyValue",
                      JS_NewCFunction(ctx, computed_value, "getPropertyValue", 1));
    if (argc > 1 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1])) {
        const char *pseudo = JS_ToCString(ctx, argv[1]);
        if (pseudo && *pseudo == ':') {
            const char *name = pseudo + (pseudo[1] == ':' ? 2 : 1);
            const char *content = web_pseudo_property(node, name, "content");
            JS_SetPropertyStr(ctx, result, "content", JS_NewString(ctx, *content ? content : "none"));
            JS_FreeCString(ctx, pseudo);
            return result;
        }
        if (pseudo)
            JS_FreeCString(ctx, pseudo);
    }
    const WebStyle *style = &node->style;
    JS_SetPropertyStr(ctx, result, "display", style_get(ctx, node, PROP_STYLE_BASE + 2));
    JS_SetPropertyStr(ctx, result, "boxSizing",
                      JS_NewString(ctx, style->border_box ? "border-box" : "content-box"));
    static const char *const positions[] = {"static", "relative", "absolute", "fixed"};
    JS_SetPropertyStr(ctx, result, "position", JS_NewString(ctx, positions[style->position & 3]));
    computed_pixels(ctx, result, "width", style->border_box ? node->w : node->content_w);
    computed_pixels(ctx, result, "height", style->border_box ? node->h : node->content_h);
    static const char *const margin[] = {"marginTop", "marginRight", "marginBottom", "marginLeft"};
    static const char *const padding[] = {"paddingTop", "paddingRight", "paddingBottom", "paddingLeft"};
    static const char *const border[] = {"borderTopWidth", "borderRightWidth", "borderBottomWidth", "borderLeftWidth"};
    for (int side = 0; side < 4; side++) {
        computed_pixels(ctx, result, margin[side], style->margin[side]);
        computed_pixels(ctx, result, padding[side], style->padding[side]);
        computed_pixels(ctx, result, border[side], style->border_shown[side] ? style->border[side] : 0);
    }
    return result;
}

static JSValue js_media_matches(JSContext *ctx, JSValueConst query) {
    JSValue media = JS_GetPropertyStr(ctx, query, "media");
    const char *text = JS_ToCString(ctx, media);
    bool matches = text && css_media_matches(text);
    if (text)
        JS_FreeCString(ctx, text);
    JS_FreeValue(ctx, media);
    return JS_NewBool(ctx, matches);
}
static JSValue js_media_listener(JSContext *ctx, JSValueConst query, int argc,
                                 JSValueConst *argv, int remove) {
    if (JS_GetClassID(query) != js_media_class || !argc || !JS_IsFunction(ctx, argv[0]))
        return JS_UNDEFINED;
    for (WebMediaListener **link = &js_media; *link; link = &(*link)->next) {
        WebMediaListener *entry = *link;
        if (JS_VALUE_GET_PTR(entry->query) != JS_VALUE_GET_PTR(query) ||
            JS_VALUE_GET_PTR(entry->callback) != JS_VALUE_GET_PTR(argv[0]))
            continue;
        if (remove) {
            *link = entry->next;
            JS_FreeValue(ctx, entry->query);
            JS_FreeValue(ctx, entry->callback);
            web_free(entry);
        }
        return JS_UNDEFINED;
    }
    if (!remove) {
        WebMediaListener *entry = web_malloc(sizeof(*entry));
        entry->query = JS_DupValue(ctx, query);
        entry->callback = JS_DupValue(ctx, argv[0]);
        entry->matches = JS_ToBool(ctx, js_media_matches(ctx, query));
        entry->next = js_media;
        js_media = entry;
    }
    return JS_UNDEFINED;
}
static JSValue js_match_media(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
    (void)this_val;
    JSValue media = argc ? JS_ToString(ctx, argv[0]) : JS_NewString(ctx, "");
    if (JS_IsException(media))
        return media;
    JSValue query = JS_NewObjectClass(ctx, js_media_class);
    if (JS_IsException(query)) {
        JS_FreeValue(ctx, media);
        return query;
    }
    static const JSCFunctionListEntry methods[] = {
        JS_CGETSET_DEF("matches", js_media_matches, 0),
        JS_CFUNC_MAGIC_DEF("addListener", 1, js_media_listener, 0),
        JS_CFUNC_MAGIC_DEF("removeListener", 1, js_media_listener, 1)};
    JS_DefinePropertyValueStr(ctx, query, "media", media, JS_PROP_ENUMERABLE);
    JS_SetPropertyFunctionList(ctx, query, methods, sizeof(methods) / sizeof(methods[0]));
    return query;
}
static JSValue js_noop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_UNDEFINED;
}
static JSValue js_click(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc;
    (void)argv;
    WebNode *node = node_from_value(ctx, this_val);
    if (node)
        web_js_dispatch(node, "click", js_document);
    return JS_UNDEFINED;
}
static JSValue js_style_set_property(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv) {
    WebNode *node = JS_GetOpaque(this_val, js_style_class);
    if (!node || argc < 2)
        return JS_UNDEFINED;
    const char *name = JS_ToCString(ctx, argv[0]);
    const char *value = JS_ToCString(ctx, argv[1]);
    if (name && value) {
        css_set_property(node, name, value);
        mark_dirty();
    }
    if (name)
        JS_FreeCString(ctx, name);
    if (value)
        JS_FreeCString(ctx, value);
    return JS_UNDEFINED;
}
static JSValue js_style_get_property(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv) {
    WebNode *node = JS_GetOpaque(this_val, js_style_class);
    if (!node || argc < 1)
        return JS_NewString(ctx, "");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name)
        return JS_NewString(ctx, "");
    char buffer[256];
    buffer[0] = 0;
    css_get_property(node, name, buffer, sizeof(buffer));
    JS_FreeCString(ctx, name);
    return JS_NewString(ctx, buffer);
}
static JSValue js_style_remove_property(JSContext *ctx, JSValueConst this_val, int argc,
                                        JSValueConst *argv) {
    WebNode *node = JS_GetOpaque(this_val, js_style_class);
    if (!node || argc < 1)
        return JS_UNDEFINED;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (name) {
        css_remove_property(node, name);
        mark_dirty();
        JS_FreeCString(ctx, name);
    }
    return JS_UNDEFINED;
}

/* ---------- timers ---------- */

static JSValue schedule_timer(JSContext *ctx, JSValueConst callback, uint32_t delay,
                              uint32_t interval) {
    WebTimer *timer = web_malloc(sizeof(*timer));
    *timer = (WebTimer){.fn = JS_DupValue(ctx, callback), .id = ++js_timer_id,
                        .due = frame_clock + delay, .interval = interval};
    WebTimer **link = &js_timers;
    while (*link && (int32_t)((*link)->due - timer->due) <= 0)
        link = &(*link)->next;
    timer->next = *link;
    *link = timer;
    return JS_NewInt32(ctx, timer->id);
}
static JSValue js_set_timer(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv, int repeat) {
    (void)this_val;
    if (!argc || !JS_IsFunction(ctx, argv[0]))
        return JS_UNDEFINED;
    int32_t delay = 0;
    if (argc > 1)
        if (JS_ToInt32(ctx, &delay, argv[1]))
            return JS_EXCEPTION;
    if (delay < 0)
        delay = 0;
    return schedule_timer(ctx, argv[0], (uint32_t)delay, repeat ? (uint32_t)(delay ? delay : 1) : 0);
}
static JSValue js_clear_timeout(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
    (void)this_val;
    if (!argc)
        return JS_UNDEFINED;
    int32_t id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    for (WebTimer *timer = js_timers; timer; timer = timer->next)
        if (timer->id == id)
            timer->cancelled = true;
    return JS_UNDEFINED;
}
static JSValue js_request_animation_frame(JSContext *ctx, JSValueConst this_val, int argc,
                                          JSValueConst *argv) {
    (void)this_val;
    if (!argc || !JS_IsFunction(ctx, argv[0]))
        return JS_UNDEFINED;
    return schedule_timer(ctx, argv[0], 16, 0);
}

/* ---------- events ---------- */

static JSValue js_prevent_default(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
    (void)argc;
    (void)argv;
    JS_SetPropertyStr(ctx, this_val, "__prevented", JS_TRUE);
    return JS_UNDEFINED;
}
static JSValue js_stop_propagation(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
    (void)argc;
    (void)argv;
    JS_SetPropertyStr(ctx, this_val, "__stopped", JS_TRUE);
    return JS_UNDEFINED;
}
static JSValue js_dispatch_event(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
    WebNode *node = node_from_value(ctx, this_val);
    if (!node || !argc)
        return JS_FALSE;
    const char *type = JS_ToCString(ctx, argv[0]);
    if (!type)
        return JS_FALSE;
    char name[64];
    size_t n = strlen(type);
    if (n >= sizeof(name))
        n = sizeof(name) - 1;
    memcpy(name, type, n);
    name[n] = 0;
    JS_FreeCString(ctx, type);
    web_js_dispatch(node, name, js_document);
    return JS_TRUE;
}

void web_js_dispatch(WebNode *target, const char *type, WebNode *root) {
    if (!js_context || !target)
        return;
    JSValue event = JS_NewObject(js_context);
    JS_SetPropertyStr(js_context, event, "type", JS_NewString(js_context, type));
    JS_SetPropertyStr(js_context, event, "target", make_element(js_context, target));
    JS_SetPropertyStr(js_context, event, "preventDefault",
                       JS_NewCFunction(js_context, js_prevent_default, "preventDefault", 0));
    JS_SetPropertyStr(js_context, event, "stopPropagation",
                       JS_NewCFunction(js_context, js_stop_propagation, "stopPropagation", 0));
    for (WebNode *node = target; node; node = node->parent) {
        WebJsState *state = state_of(node);
        if (!state || !state->listeners)
            continue;
        JSValue current = make_element(js_context, node);
        JS_SetPropertyStr(js_context, event, "currentTarget", JS_DupValue(js_context, current));
        JSValue callbacks = JS_NewArray(js_context);
        uint32_t count = 0;
        for (WebListener *listener = state ? state->listeners : 0; listener;
             listener = listener->next) {
            if (strcmp(listener->type, type))
                continue;
            if (JS_SetPropertyUint32(js_context, callbacks, count,
                                    JS_DupValue(js_context, listener->fn)) < 0)
                break;
            count++;
        }
        for (uint32_t i = 0; i < count; i++) {
            JSValue callback = JS_GetPropertyUint32(js_context, callbacks, i);
            JSValue result = JS_Call(js_context, callback, current, 1, (JSValueConst *)&event);
            JS_FreeValue(js_context, callback);
            if (JS_IsException(result))
                log_callback_exception("Browser: JavaScript event: ");
            JS_FreeValue(js_context, result);
        }
        JS_FreeValue(js_context, callbacks);
        JS_FreeValue(js_context, current);
        JSValue stopped = JS_GetPropertyStr(js_context, event, "__stopped");
        bool halted = JS_ToBool(js_context, stopped);
        JS_FreeValue(js_context, stopped);
        if (halted)
            break;
        if (root && node == root)
            break;
    }
    JS_FreeValue(js_context, event);
}

/* ---------- document and window ---------- */

static JSValue document_getter(JSContext *ctx, JSValueConst this_val, int magic) {
    (void)this_val;
    switch (magic) {
        case PROP_BODY: {
            WebNode *body = web_find_first(js_document, "body");
            if (body)
                return make_element(ctx, body);
            return JS_NULL;
        }
        case PROP_DOCUMENT_ELEMENT: {
            WebNode *html = web_find_first(js_document, "html");
            if (html)
                return make_element(ctx, html);
            return JS_NULL;
        }
        case PROP_HEAD: {
            WebNode *head = web_find_first(js_document, "head");
            if (head)
                return make_element(ctx, head);
            return JS_NULL;
        }
        case PROP_READY_STATE:
            return JS_NewString(ctx, js_ready_state);
        case PROP_CURRENT_SCRIPT:
            return js_current_script ? make_element(ctx, js_current_script) : JS_NULL;
        case PROP_TITLE: {
            WebNode *title = web_find_first(js_document, "title");
            if (title) {
                char buffer[256];
                buffer[0] = 0;
                web_node_text(title, buffer, sizeof(buffer));
                return JS_NewString(ctx, buffer);
            }
            return JS_NewString(ctx, "");
        }
        case PROP_LOCATION:
            return JS_UNDEFINED;
        default:
            return JS_UNDEFINED;
    }
}
static JSValue js_console_log(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
    (void)this_val;
    for (int i = 0; i < argc; i++) {
        const char *text = JS_ToCString(ctx, argv[i]);
        if (text) {
            serial_write("[page] ");
            serial_write(text);
            serial_write("\n");
            JS_FreeCString(ctx, text);
        }
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry element_functions[] = {
    JS_CFUNC_DEF("addEventListener", 2, js_add_event_listener),
    JS_CFUNC_DEF("removeEventListener", 2, js_remove_event_listener),
    JS_CFUNC_DEF("querySelector", 1, js_query_selector),
    JS_CFUNC_DEF("querySelectorAll", 1, js_query_selector_all),
    JS_CFUNC_DEF("getElementsByTagName", 1, js_get_elements_by_tag),
    JS_CFUNC_DEF("hasChildNodes", 0, js_has_child_nodes),
    JS_CFUNC_DEF("matches", 1, js_matches),
    JS_CFUNC_DEF("appendChild", 1, js_append_child),
    JS_CFUNC_DEF("insertBefore", 2, js_insert_before),
    JS_CFUNC_DEF("cloneNode", 1, js_clone_node),
    JS_CFUNC_DEF("attachShadow", 1, js_attach_shadow),
    JS_CFUNC_DEF("removeChild", 1, js_remove_child),
    JS_CFUNC_DEF("replaceChild", 2, js_replace_child),
    JS_CFUNC_DEF("getRootNode", 0, js_get_root_node),
    JS_CFUNC_DEF("setAttribute", 2, js_set_attribute),
    JS_CFUNC_DEF("getAttribute", 1, js_get_attribute),
    JS_CFUNC_DEF("hasAttribute", 1, js_has_attribute),
    JS_CFUNC_DEF("removeAttribute", 1, js_remove_attribute),
    JS_CFUNC_DEF("getBoundingClientRect", 0, js_get_bounding_rect),
    JS_CFUNC_DEF("click", 0, js_click),
    JS_CFUNC_DEF("focus", 0, js_noop),
    JS_CFUNC_DEF("blur", 0, js_noop),
    JS_CFUNC_DEF("dispatchEvent", 1, js_dispatch_event),
    JS_CFUNC_DEF("remove", 0, js_remove_element),
    {0, 0, 0, 0}};

static void install_element(JSContext *ctx, JSValue object) {
    static const JSCFunctionListEntry properties[] = {
        JS_CGETSET_MAGIC_DEF("tagName", element_getter, element_setter, PROP_TAG_NAME),
        JS_CGETSET_MAGIC_DEF("id", element_getter, element_setter, PROP_ID),
        JS_CGETSET_MAGIC_DEF("className", element_getter, element_setter, PROP_CLASS_NAME),
        JS_CGETSET_MAGIC_DEF("textContent", element_getter, element_setter, PROP_TEXT_CONTENT),
        JS_CGETSET_MAGIC_DEF("innerHTML", element_getter, element_setter, PROP_INNER_HTML),
        JS_CGETSET_MAGIC_DEF("style", element_getter, 0, PROP_STYLE),
        JS_CGETSET_MAGIC_DEF("children", element_getter, 0, PROP_CHILDREN),
        JS_CGETSET_MAGIC_DEF("childElementCount", element_getter, 0, PROP_CHILD_COUNT),
        JS_CGETSET_MAGIC_DEF("parentElement", element_getter, 0, PROP_PARENT),
        JS_CGETSET_MAGIC_DEF("href", element_getter, element_setter, PROP_HREF),
        JS_CGETSET_MAGIC_DEF("title", element_getter, element_setter, PROP_TITLE),
        JS_CGETSET_MAGIC_DEF("nodeType", element_getter, 0, PROP_NODE_TYPE),
        JS_CGETSET_MAGIC_DEF("nodeName", element_getter, 0, PROP_NODE_NAME),
        JS_CGETSET_MAGIC_DEF("nodeValue", element_getter, 0, PROP_NODE_VALUE),
        JS_CGETSET_MAGIC_DEF("ownerDocument", element_getter, 0, PROP_OWNER_DOCUMENT),
        JS_CGETSET_MAGIC_DEF("parentNode", element_getter, 0, PROP_PARENT_NODE),
        JS_CGETSET_MAGIC_DEF("childNodes", element_getter, 0, PROP_CHILD_NODES),
        JS_CGETSET_MAGIC_DEF("checked", element_getter, element_setter, PROP_CHECKED),
        JS_CGETSET_MAGIC_DEF("disabled", element_getter, element_setter, PROP_DISABLED),
        JS_CGETSET_MAGIC_DEF("offsetWidth", element_getter, 0, PROP_OFFSET_WIDTH),
        JS_CGETSET_MAGIC_DEF("offsetHeight", element_getter, 0, PROP_OFFSET_HEIGHT),
        JS_CGETSET_MAGIC_DEF("isConnected", element_getter, 0, PROP_IS_CONNECTED),
        JS_CGETSET_MAGIC_DEF("content", element_getter, 0, PROP_TEMPLATE_CONTENT),
        JS_CGETSET_MAGIC_DEF("shadowRoot", element_getter, 0, PROP_SHADOW_ROOT),
        JS_CGETSET_MAGIC_DEF("value", element_getter, element_setter, PROP_VALUE),
        JS_CGETSET_MAGIC_DEF("firstChild", element_getter, 0, PROP_FIRST_CHILD),
        JS_CGETSET_MAGIC_DEF("lastChild", element_getter, 0, PROP_LAST_CHILD),
        JS_CGETSET_MAGIC_DEF("nextSibling", element_getter, 0, PROP_NEXT_SIBLING),
        JS_CGETSET_MAGIC_DEF("previousSibling", element_getter, 0, PROP_PREVIOUS_SIBLING),
        {0, 0, 0, 0, 0}};
    JS_SetPropertyFunctionList(ctx, object, properties,
                                (int)(sizeof(properties) / sizeof(properties[0])) - 1);
    JS_SetPropertyFunctionList(ctx, object, element_functions,
                                (int)(sizeof(element_functions) / sizeof(element_functions[0])) - 1);
}

static const JSCFunctionListEntry style_functions[] = {
    JS_CFUNC_DEF("setProperty", 2, js_style_set_property),
    JS_CFUNC_DEF("getPropertyValue", 1, js_style_get_property),
    JS_CFUNC_DEF("removeProperty", 1, js_style_remove_property),
    {0, 0, 0, 0}};

static void install_style(JSContext *ctx, JSValue object) {
    static const JSCFunctionListEntry properties[] = {
        JS_CGETSET_MAGIC_DEF("cssText", element_getter, element_setter, PROP_CSS_TEXT),
        JS_CGETSET_MAGIC_DEF("color", element_getter, element_setter, PROP_STYLE_BASE + 0),
        JS_CGETSET_MAGIC_DEF("backgroundColor", element_getter, element_setter, PROP_STYLE_BASE + 1),
        JS_CGETSET_MAGIC_DEF("display", element_getter, element_setter, PROP_STYLE_BASE + 2),
        JS_CGETSET_MAGIC_DEF("fontSize", element_getter, element_setter, PROP_STYLE_BASE + 3),
        JS_CGETSET_MAGIC_DEF("fontWeight", element_getter, element_setter, PROP_STYLE_BASE + 4),
        JS_CGETSET_MAGIC_DEF("textAlign", element_getter, element_setter, PROP_STYLE_BASE + 5),
        JS_CGETSET_MAGIC_DEF("margin", element_getter, element_setter, PROP_STYLE_BASE + 6),
        JS_CGETSET_MAGIC_DEF("marginTop", element_getter, element_setter, PROP_STYLE_BASE + 7),
        JS_CGETSET_MAGIC_DEF("marginRight", element_getter, element_setter, PROP_STYLE_BASE + 8),
        JS_CGETSET_MAGIC_DEF("marginBottom", element_getter, element_setter, PROP_STYLE_BASE + 9),
        JS_CGETSET_MAGIC_DEF("marginLeft", element_getter, element_setter, PROP_STYLE_BASE + 10),
        JS_CGETSET_MAGIC_DEF("padding", element_getter, element_setter, PROP_STYLE_BASE + 11),
        JS_CGETSET_MAGIC_DEF("paddingTop", element_getter, element_setter, PROP_STYLE_BASE + 12),
        JS_CGETSET_MAGIC_DEF("paddingRight", element_getter, element_setter, PROP_STYLE_BASE + 13),
        JS_CGETSET_MAGIC_DEF("paddingBottom", element_getter, element_setter, PROP_STYLE_BASE + 14),
        JS_CGETSET_MAGIC_DEF("paddingLeft", element_getter, element_setter, PROP_STYLE_BASE + 15),
        JS_CGETSET_MAGIC_DEF("width", element_getter, element_setter, PROP_STYLE_BASE + 16),
        JS_CGETSET_MAGIC_DEF("height", element_getter, element_setter, PROP_STYLE_BASE + 17),
        JS_CGETSET_MAGIC_DEF("textDecoration", element_getter, element_setter, PROP_STYLE_BASE + 18),
        JS_CGETSET_MAGIC_DEF("visibility", element_getter, element_setter, PROP_STYLE_BASE + 19),
        JS_CGETSET_MAGIC_DEF("lineHeight", element_getter, element_setter, PROP_STYLE_BASE + 20),
        JS_CGETSET_MAGIC_DEF("background", element_getter, element_setter, PROP_STYLE_BASE + 21),
        JS_CGETSET_MAGIC_DEF("border", element_getter, element_setter, PROP_STYLE_BASE + 23),
        JS_CGETSET_MAGIC_DEF("overflow", element_getter, element_setter, PROP_STYLE_BASE + 24),
        JS_CGETSET_MAGIC_DEF("cssFloat", element_getter, element_setter, PROP_STYLE_BASE + 25),
        JS_CGETSET_MAGIC_DEF("opacity", element_getter, element_setter, PROP_STYLE_BASE + 26),
        {0, 0, 0, 0, 0}};
    JS_SetPropertyFunctionList(ctx, object, properties,
                                (int)(sizeof(properties) / sizeof(properties[0])) - 1);
    JS_SetPropertyFunctionList(ctx, object, style_functions,
                                (int)(sizeof(style_functions) / sizeof(style_functions[0])) - 1);
}

static void build_document(JSContext *ctx, WebNode *root) {
    JSValue document = make_element(ctx, root);
    JS_SetPropertyStr(ctx, document, "getElementById",
                       JS_NewCFunction(ctx, js_get_element_by_id, "getElementById", 1));
    JS_SetPropertyStr(ctx, document, "querySelector",
                       JS_NewCFunction(ctx, js_query_selector, "querySelector", 1));
    JS_SetPropertyStr(ctx, document, "querySelectorAll",
                       JS_NewCFunction(ctx, js_query_selector_all, "querySelectorAll", 1));
    JS_SetPropertyStr(ctx, document, "createElement",
                       JS_NewCFunction(ctx, js_create_element, "createElement", 1));
    JS_SetPropertyStr(ctx, document, "createTextNode",
                       JS_NewCFunctionMagic(ctx, js_create_character_node, "createTextNode", 1,
                                            JS_CFUNC_generic_magic, WEB_TEXT));
    JS_SetPropertyStr(ctx, document, "createComment",
                       JS_NewCFunctionMagic(ctx, js_create_character_node, "createComment", 1,
                                            JS_CFUNC_generic_magic, WEB_COMMENT));
    JS_SetPropertyStr(ctx, document, "createDocumentFragment",
                       JS_NewCFunction(ctx, js_create_document_fragment, "createDocumentFragment", 0));
    JS_SetPropertyStr(ctx, document, "addEventListener",
                       JS_NewCFunction(ctx, js_add_event_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, document, "removeEventListener",
                       JS_NewCFunction(ctx, js_remove_event_listener, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, document, "dispatchEvent",
                       JS_NewCFunction(ctx, js_dispatch_event, "dispatchEvent", 1));
    static const JSCFunctionListEntry properties[] = {
        JS_CGETSET_MAGIC_DEF("body", document_getter, 0, PROP_BODY),
        JS_CGETSET_MAGIC_DEF("documentElement", document_getter, 0, PROP_DOCUMENT_ELEMENT),
        JS_CGETSET_MAGIC_DEF("head", document_getter, 0, PROP_HEAD),
        JS_CGETSET_MAGIC_DEF("title", document_getter, 0, PROP_TITLE),
        JS_CGETSET_MAGIC_DEF("readyState", document_getter, 0, PROP_READY_STATE),
        JS_CGETSET_MAGIC_DEF("currentScript", document_getter, 0, PROP_CURRENT_SCRIPT),
        JS_PROP_STRING_DEF("compatMode", "CSS1Compat", 0)};
    JS_SetPropertyFunctionList(ctx, document, properties,
                                (int)(sizeof(properties) / sizeof(properties[0])));
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "document", document);
    JS_FreeValue(ctx, global);
}
static void build_window(JSContext *ctx, const char *url) {
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "addEventListener",
                       JS_NewCFunction(ctx, js_add_event_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, global, "removeEventListener",
                       JS_NewCFunction(ctx, js_remove_event_listener, "removeEventListener", 2));
    JSValue location = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, location, "href", JS_NewString(ctx, url ? url : ""));
    const char *protocol = url && !strncmp(url, "https://", 8) ? "https:" : "http:";
    JS_SetPropertyStr(ctx, location, "protocol", JS_NewString(ctx, protocol));
    const char *host = url ? url : "";
    if (!strncmp(host, "https://", 8))
        host += 8;
    else if (!strncmp(host, "http://", 7))
        host += 7;
    char hostname[256];
    size_t n = 0;
    while (host[n] && host[n] != '/' && host[n] != ':' && n < sizeof(hostname) - 1) {
        hostname[n] = host[n];
        n++;
    }
    hostname[n] = 0;
    JS_SetPropertyStr(ctx, location, "hostname", JS_NewString(ctx, hostname));
    JS_SetPropertyStr(ctx, global, "location", location);
    JSValue navigator = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, navigator, "userAgent", JS_NewString(ctx, "Luna Browser/0.3 (LunaOS)"));
    JS_SetPropertyStr(ctx, navigator, "platform", JS_NewString(ctx, "LunaOS i386"));
    JS_SetPropertyStr(ctx, global, "navigator", navigator);
    JS_SetPropertyStr(ctx, global, "window", JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, global, "self", JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, global, "getComputedStyle",
                       JS_NewCFunction(ctx, js_computed_style, "getComputedStyle", 1));
    JS_SetPropertyStr(ctx, global, "matchMedia",
                       JS_NewCFunction(ctx, js_match_media, "matchMedia", 1));
    JS_SetPropertyStr(ctx, global, "setTimeout",
                       JS_NewCFunctionMagic(ctx, js_set_timer, "setTimeout", 2, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, global, "setInterval",
                       JS_NewCFunctionMagic(ctx, js_set_timer, "setInterval", 2, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, global, "clearTimeout",
                       JS_NewCFunction(ctx, js_clear_timeout, "clearTimeout", 1));
    JS_SetPropertyStr(ctx, global, "clearInterval",
                       JS_NewCFunction(ctx, js_clear_timeout, "clearInterval", 1));
    JS_SetPropertyStr(ctx, global, "requestAnimationFrame",
                       JS_NewCFunction(ctx, js_request_animation_frame, "requestAnimationFrame", 1));
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log", JS_NewCFunction(ctx, js_console_log, "log", 1));
    JS_SetPropertyStr(ctx, console, "warn", JS_NewCFunction(ctx, js_console_log, "warn", 1));
    JS_SetPropertyStr(ctx, console, "error", JS_NewCFunction(ctx, js_console_log, "error", 1));
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_FreeValue(ctx, global);
}

/* ---------- runtime ---------- */

static void drain_jobs(void) {
    if (!js_runtime)
        return;
    JSContext *job_context;
    int jobs = 0;
    while (JS_IsJobPending(js_runtime) && jobs++ < 100) {
        if (JS_ExecutePendingJob(js_runtime, &job_context) < 0)
            break;
    }
}
static void collect_scripts(WebNode *node, WebNode **out, int *count) {
    if (!node || *count >= 32)
        return;
    if (node->type == WEB_ELEMENT && !strcmp(node->tag, "script")) {
        /* Only scripts of a JavaScript type are executed. Data blocks such as
         * application/ld+json are document data, not program text. */
        const char *type = web_attr(node, "type");
        bool is_program = true;
        if (type && *type) {
            is_program = !web_strcasecmp(type, "text/javascript") ||
                         !web_strcasecmp(type, "application/javascript") ||
                         !web_strcasecmp(type, "text/ecmascript") ||
                         !web_strcasecmp(type, "application/ecmascript");
        }
        if (is_program)
            out[(*count)++] = node;
    }
    for (WebNode *c = node->first_child; c; c = c->next)
        collect_scripts(c, out, count);
}

static bool fetch_script(const char *src, char *url, size_t cap, char **body, size_t *length) {
    char error[192] = "";
    if (!*src || !net_url_resolve(web_document_url(), src, url, cap) ||
        (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)))
        return false;
    return net_http_alloc(url, body, length, error, sizeof(error)) >= 0;
}

bool web_js_run(WebNode *root, char *error, size_t ecap) {
    if (error && ecap)
        error[0] = 0;
    if (!root)
        return false;
    web_js_release();
    WebNode *scripts[32];
    int count = 0;
    collect_scripts(root, scripts, &count);
    if (!count)
        return true;
    js_runtime = JS_NewRuntime();
    if (!js_runtime) {
        if (error)
            append(error, "JavaScript runtime could not be created.", ecap);
        return false;
    }
    /* Runtime allocations use the growable physical-memory-backed heap. */
    JS_SetMaxStackSize(js_runtime, 48 * 1024);
    JS_SetInterruptHandler(js_runtime, interrupt_js, 0);
    JS_NewClassID(&js_node_class);
    JS_NewClassID(&js_style_class);
    JS_NewClassID(&js_media_class);
    static const JSClassDef node_class = {.class_name = "Node"};
    static const JSClassDef style_class = {.class_name = "CSSStyleDeclaration"};
    static const JSClassDef media_class = {.class_name = "MediaQueryList"};
    if (!JS_NewClass(js_runtime, js_node_class, &node_class) &&
        !JS_NewClass(js_runtime, js_style_class, &style_class) &&
        !JS_NewClass(js_runtime, js_media_class, &media_class))
        js_context = JS_NewContext(js_runtime);
    if (!js_context) {
        JS_FreeRuntime(js_runtime);
        js_runtime = 0;
        if (error)
            append(error, "JavaScript context could not be created.", ecap);
        return false;
    }
    js_document = root;
    js_ready_state = "loading";
    JSValue node_prototype = JS_NewObject(js_context);
    install_element(js_context, node_prototype);
    JSValue element = JS_NewCFunction2(js_context, js_element_constructor, "Element", 0,
                                       JS_CFUNC_constructor, 0);
    JS_SetConstructor(js_context, element, node_prototype);
    JSValue global = JS_GetGlobalObject(js_context);
    JS_SetPropertyStr(js_context, global, "Element", element);
    JS_FreeValue(js_context, global);
    JS_SetClassProto(js_context, js_node_class, node_prototype);
    JSValue style_prototype = JS_NewObject(js_context);
    install_style(js_context, style_prototype);
    JS_SetClassProto(js_context, js_style_class, style_prototype);
    js_deadline = frame_clock + 250;
    build_document(js_context, root);
    build_window(js_context, web_document_url());
    bool failed = false;
    for (int i = 0; i < count; i++) {
        const char *src = web_attr(scripts[i], "src");
        char url[512] = "", *body = 0, *converted = 0;
        const char *source = "";
        size_t length = 0;
        if (src) {
            if (!fetch_script(src, url, sizeof(url), &body, &length)) {
                if (!failed && error) {
                    append(error, "JavaScript could not be loaded: ", ecap);
                    append(error, src, ecap);
                }
                failed = true;
                if (body)
                    heap_release(body);
                continue;
            }
            if (body) {
                converted = web_convert_legacy_text(body, &length);
                source = converted ? converted : body;
            }
        } else {
            const WebNode *child = scripts[i]->first_child;
            if (child && child->type == WEB_TEXT && child->text) {
                source = child->text;
                length = strlen(source);
            }
        }
        js_deadline = frame_clock + 250;
        js_current_script = scripts[i];
        JSValue value = JS_Eval(js_context, source, length, src ? url : "<page>", JS_EVAL_TYPE_GLOBAL);
        js_current_script = 0;
        web_free(converted);
        if (body)
            heap_release(body);
        if (JS_IsException(value)) {
            /* A failing script must not stop the page from being shown. */
            JSValue exception = JS_GetException(js_context);
            const char *text = JS_ToCString(js_context, exception);
            if (!failed) {
                failed = true;
                if (error) {
                    append(error, "JavaScript error: ", ecap);
                    append(error, text ? text : "unknown", ecap);
                }
            }
            serial_write("Browser: JavaScript error in ");
            serial_write(src ? url : "<page>");
            serial_write(": ");
            serial_write(text ? text : "unknown");
            serial_write("\n");
            if (text)
                JS_FreeCString(js_context, text);
            JS_FreeValue(js_context, exception);
        }
        JS_FreeValue(js_context, value);
        drain_jobs();
    }
    return !failed;
}
void web_js_finish_document(void) {
    if (!js_context || strcmp(js_ready_state, "loading"))
        return;
    js_deadline = frame_clock + 250;
    js_ready_state = "interactive";
    web_js_dispatch(js_document, "DOMContentLoaded", js_document);
    drain_jobs();
    js_ready_state = "complete";
    web_js_dispatch(js_document, "load", js_document);
    drain_jobs();
}
bool web_js_timer_ready(void) {
    if (js_resize_pending)
        return true;
    for (const WebTimer *timer = js_timers; timer; timer = timer->next)
        if (!timer->cancelled && (int32_t)(frame_clock - timer->due) >= 0)
            return true;
    return false;
}
void web_js_viewport_changed(void) {
    if (js_context)
        js_resize_pending = true;
}
static void dispatch_resize(void) {
    js_resize_pending = false;
    js_deadline = frame_clock + 250;
    JSValue pending = JS_NewArray(js_context);
    uint32_t count = 0;
    for (WebMediaListener *entry = js_media; entry; entry = entry->next) {
        bool matches = JS_ToBool(js_context, js_media_matches(js_context, entry->query));
        if (matches == entry->matches)
            continue;
        if (JS_SetPropertyUint32(js_context, pending, count, JS_DupValue(js_context, entry->query)) < 0 ||
            JS_SetPropertyUint32(js_context, pending, count + 1, JS_DupValue(js_context, entry->callback)) < 0)
            break;
        entry->matches = matches;
        count += 2;
    }
    for (uint32_t i = 0; i < count; i += 2) {
        JSValue query = JS_GetPropertyUint32(js_context, pending, i);
        JSValue callback = JS_GetPropertyUint32(js_context, pending, i + 1);
        JSValue result = JS_Call(js_context, callback, query, 1, (JSValueConst *)&query);
        if (JS_IsException(result))
            log_callback_exception("Browser: JavaScript media query: ");
        JS_FreeValue(js_context, result);
        JS_FreeValue(js_context, callback);
        JS_FreeValue(js_context, query);
    }
    JS_FreeValue(js_context, pending);
    web_js_dispatch(js_document, "resize", js_document);
}
void web_js_pump(void) {
    if (!js_context)
        return;
    bool ran = js_resize_pending;
    if (js_resize_pending)
        dispatch_resize();
    int last_timer = js_timer_id;
    for (WebTimer *timer = js_timers; timer; timer = timer->next) {
        if (timer->cancelled || timer->id > last_timer || (int32_t)(frame_clock - timer->due) < 0)
            continue;
        if (timer->interval)
            timer->due = frame_clock + timer->interval;
        else
            timer->cancelled = true;
        js_deadline = frame_clock + 100;
        JSValue result = JS_Call(js_context, timer->fn, JS_UNDEFINED, 0, 0);
        if (JS_IsException(result))
            log_callback_exception("Browser: JavaScript timer: ");
        JS_FreeValue(js_context, result);
        ran = true;
    }
    WebTimer **link = &js_timers;
    while (*link) {
        WebTimer *timer = *link;
        if (timer->cancelled) {
            *link = timer->next;
            JS_FreeValue(js_context, timer->fn);
            web_free(timer);
            continue;
        }
        link = &timer->next;
    }
    if (ran)
        drain_jobs();
}
bool web_js_dirty(void) { return js_document_dirty; }
void web_js_clear_dirty(void) { js_document_dirty = false; }

static void release_states(WebNode *node) {
    if (!node)
        return;
    WebJsState *state = (WebJsState *)node->js;
    if (state) {
        WebListener *listener = state->listeners;
        while (listener) {
            WebListener *next = listener->next;
            if (js_context)
                JS_FreeValue(js_context, listener->fn);
            web_free(listener->type);
            web_free(listener);
            listener = next;
        }
        if (js_context && !JS_IsUndefined(state->object))
            JS_FreeValue(js_context, state->object);
        web_free(state);
        node->js = 0;
    }
    for (WebNode *c = node->first_child; c; c = c->next)
        release_states(c);
    /* Template and shadow trees are separately owned, not light-DOM children. */
    if (node->template_content)
        release_states(node->template_content);
    if (node->shadow_root)
        release_states(node->shadow_root);
}
void web_js_release(void) {
    if (js_document)
        release_states(js_document);
    js_document = 0;
    /* Tear down detached JS states before releasing the JS context, then
     * reclaim the orphan trees that were kept alive for removeChild(). */
    while (js_detached) {
        WebDetachedNode *entry = js_detached;
        js_detached = entry->next;
        if (entry->node && !entry->node->parent) {
            release_states(entry->node);
            web_free_tree(entry->node);
        }
        web_free(entry);
    }
    WebTimer *timer = js_timers;
    while (timer) {
        WebTimer *next = timer->next;
        if (js_context)
            JS_FreeValue(js_context, timer->fn);
        web_free(timer);
        timer = next;
    }
    js_timers = 0;
    js_resize_pending = false;
    while (js_media) {
        WebMediaListener *entry = js_media;
        js_media = entry->next;
        JS_FreeValue(js_context, entry->query);
        JS_FreeValue(js_context, entry->callback);
        web_free(entry);
    }
    if (js_context) {
        JS_FreeContext(js_context);
        js_context = 0;
    }
    if (js_runtime) {
        JS_FreeRuntime(js_runtime);
        js_runtime = 0;
    }
}
