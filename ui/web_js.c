/* QuickJS bindings for the Luna Browser DOM. Scripts run inside the kernel
 * service with a memory and time budget, the same runtime used by lunaSH. */
#include "quickjs.h"
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
    bool cancelled;
    struct WebTimer *next;
} WebTimer;

static JSRuntime *js_runtime;
static JSContext *js_context;
static WebNode *js_document;
static WebTimer *js_timers;
static int js_timer_id;
static bool js_document_dirty;
static uint32_t js_deadline;

static int interrupt_js(JSRuntime *rt, void *opaque) {
    (void)rt;
    (void)opaque;
    return (int32_t)(frame_clock - js_deadline) >= 0;
}

static WebNode *node_from_value(JSContext *ctx, JSValueConst value) {
    if (JS_IsUndefined(value) || JS_IsNull(value))
        return 0;
    JSValue hidden = JS_GetPropertyStr(ctx, value, "__node");
    if (JS_IsUndefined(hidden) || JS_IsNull(hidden)) {
        JS_FreeValue(ctx, hidden);
        return 0;
    }
    int64_t pointer = 0;
    if (JS_ToInt64(ctx, &pointer, hidden) != 0)
        pointer = 0;
    JS_FreeValue(ctx, hidden);
    return (WebNode *)(uintptr_t)pointer;
}
static WebJsState *state_of(WebNode *node) {
    if (!node)
        return 0;
    return (WebJsState *)node->js;
}

static JSValue make_element(JSContext *ctx, WebNode *node) {
    WebJsState *state = state_of(node);
    if (state && !JS_IsUndefined(state->object))
        return JS_DupValue(ctx, state->object);
    JSValue object = JS_NewObject(ctx);
    if (JS_IsException(object))
        return object;
    JS_DefinePropertyValueStr(ctx, object, "__node",
                               JS_NewInt64(ctx, (int64_t)(uintptr_t)node), JS_PROP_CONFIGURABLE);
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
    PROP_VALUE,
    PROP_FIRST_CHILD,
    PROP_LAST_CHILD,
    PROP_NEXT_SIBLING,
    PROP_PREVIOUS_SIBLING,
    PROP_READY_STATE,
    PROP_BODY,
    PROP_DOCUMENT_ELEMENT,
    PROP_HEAD,
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
    WebNode *node = node_from_value(ctx, this_val);
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
            JSValue object = JS_NewObject(ctx);
            JS_DefinePropertyValueStr(ctx, object, "__node",
                                       JS_NewInt64(ctx, (int64_t)(uintptr_t)node),
                                       JS_PROP_CONFIGURABLE);
            install_style(ctx, object);
            return object;
        }
        case PROP_CHILDREN: {
            JSValue array = JS_NewArray(ctx);
            uint32_t index = 0;
            for (WebNode *c = node->first_child; c; c = c->next)
                if (c->type == WEB_ELEMENT)
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
        case PROP_HREF: {
            const char *href = web_attr(node, "href");
            return JS_NewString(ctx, href ? href : "");
        }
        case PROP_TITLE: {
            const char *title = web_attr(node, "title");
            return JS_NewString(ctx, title ? title : "");
        }
        case PROP_NODE_TYPE:
            return JS_NewInt32(ctx, node->type == WEB_ELEMENT ? 1 : node->type == WEB_TEXT ? 3 : 8);
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
    WebNode *node = node_from_value(ctx, this_val);
    if (!node)
        return JS_UNDEFINED;
    const char *text = JS_ToCString(ctx, value);
    if (!text)
        return JS_UNDEFINED;
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
            break;
        case PROP_CLASS_NAME:
            web_set_attr(node, "class", buffer);
            break;
        case PROP_HREF:
            web_set_attr(node, "href", buffer);
            break;
        case PROP_TITLE:
            web_set_attr(node, "title", buffer);
            break;
        case PROP_VALUE:
            web_set_attr(node, "value", buffer);
            break;
        case PROP_TEXT_CONTENT: {
            while (node->first_child) {
                WebNode *child = node->first_child;
                node->first_child = child->next;
                if (node->first_child)
                    node->first_child->prev = 0;
                else
                    node->last_child = 0;
                child->parent = 0;
                child->next = child->prev = 0;
                web_free_tree(child);
            }
            if (buffer[0]) {
                WebNode *text = web_node_new(WEB_TEXT);
                text->text = web_strdup(buffer);
                web_node_append(node, text);
            }
            mark_dirty();
            break;
        }
        case PROP_INNER_HTML: {
            while (node->first_child) {
                WebNode *child = node->first_child;
                node->first_child = child->next;
                if (node->first_child)
                    node->first_child->prev = 0;
                else
                    node->last_child = 0;
                child->parent = 0;
                child->next = child->prev = 0;
                web_free_tree(child);
            }
            WebNode *parsed = 0;
            char error[128];
            if (web_parse(buffer, (size_t)strlen(buffer), &parsed, error, sizeof(error)) &&
                parsed) {
                for (WebNode *child = parsed->first_child; child;) {
                    WebNode *next = child->next;
                    child->parent = 0;
                    child->next = child->prev = 0;
                    web_node_append(node, child);
                    child = next;
                }
                web_free_tree(parsed);
            }
            mark_dirty();
            break;
        }
        default:
            break;
    }
    return JS_UNDEFINED;
}

/* ---------- element methods ---------- */

static JSValue js_add_event_listener(JSContext *ctx, JSValueConst this_val, int argc,
                                      JSValueConst *argv) {
    WebNode *node = node_from_value(ctx, this_val);
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
    WebNode *node = node_from_value(ctx, this_val);
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
    (void)this_val;
    if (!argc || !js_document)
        return JS_NULL;
    const char *selector = JS_ToCString(ctx, argv[0]);
    if (!selector)
        return JS_NULL;
    WebNode *matches[64];
    int count = 0;
    collect_matching(js_document, selector, matches, &count, 64);
    JS_FreeCString(ctx, selector);
    if (!count)
        return JS_NULL;
    return make_element(ctx, matches[0]);
}
static JSValue js_query_selector_all(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv) {
    (void)this_val;
    if (!argc || !js_document)
        return JS_NewArray(ctx);
    const char *selector = JS_ToCString(ctx, argv[0]);
    if (!selector)
        return JS_NewArray(ctx);
    WebNode *matches[256];
    int count = 0;
    collect_matching(js_document, selector, matches, &count, 256);
    JS_FreeCString(ctx, selector);
    JSValue array = JS_NewArray(ctx);
    for (int i = 0; i < count; i++)
        JS_SetPropertyUint32(ctx, array, (uint32_t)i, make_element(ctx, matches[i]));
    return array;
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
    return make_element(ctx, node);
}
static JSValue js_create_text_node(JSContext *ctx, JSValueConst this_val, int argc,
                                   JSValueConst *argv) {
    (void)this_val;
    if (!argc)
        return JS_NULL;
    const char *text = JS_ToCString(ctx, argv[0]);
    if (!text)
        return JS_NULL;
    WebNode *node = web_node_new(WEB_TEXT);
    node->text = web_strdup(text);
    JS_FreeCString(ctx, text);
    return make_element(ctx, node);
}
static JSValue js_append_child(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
    WebNode *parent = node_from_value(ctx, this_val);
    if (!parent || !argc)
        return JS_UNDEFINED;
    WebNode *child = node_from_value(ctx, argv[0]);
    if (!child)
        return JS_UNDEFINED;
    if (child->parent) {
        WebNode *old = child->parent;
        WebNode **link = &old->first_child;
        while (*link && *link != child)
            link = &(*link)->next;
        if (*link) {
            *link = child->next;
            if (child->next)
                child->next->prev = child->prev;
            else
                old->last_child = child->prev;
        }
        child->parent = 0;
        child->prev = 0;
        child->next = 0;
    }
    web_node_append(parent, child);
    mark_dirty();
    return make_element(ctx, child);
}
static JSValue js_remove_child(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
    WebNode *parent = node_from_value(ctx, this_val);
    if (!parent || !argc)
        return JS_UNDEFINED;
    WebNode *child = node_from_value(ctx, argv[0]);
    if (!child || !child->parent)
        return JS_UNDEFINED;
    WebNode **link = &parent->first_child;
    while (*link && *link != child)
        link = &(*link)->next;
    if (*link) {
        *link = child->next;
        if (child->next)
            child->next->prev = child->prev;
        else
            parent->last_child = child->prev;
        child->parent = 0;
        child->prev = 0;
        child->next = 0;
        web_free_tree(child);
        mark_dirty();
    }
    return JS_UNDEFINED;
}
static JSValue js_remove_element(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
    (void)argc;
    (void)argv;
    WebNode *node = node_from_value(ctx, this_val);
    if (node && node->parent) {
        WebNode *parent = node->parent;
        WebNode **link = &parent->first_child;
        while (*link && *link != node)
            link = &(*link)->next;
        if (*link) {
            *link = node->next;
            if (node->next)
                node->next->prev = node->prev;
            else
                parent->last_child = node->prev;
            node->parent = 0;
            node->prev = 0;
            node->next = 0;
            web_free_tree(node);
            mark_dirty();
        }
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
        WebAttr **link = &node->attrs;
        while (*link) {
            if (!web_strcasecmp((*link)->name, name)) {
                WebAttr *dead = *link;
                *link = dead->next;
                web_free(dead->name);
                web_free(dead->value);
                web_free(dead);
                mark_dirty();
                continue;
            }
            link = &(*link)->next;
        }
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
    WebNode *node = node_from_value(ctx, this_val);
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
    WebNode *node = node_from_value(ctx, this_val);
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
    WebNode *node = node_from_value(ctx, this_val);
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

static JSValue js_set_timeout(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
    (void)this_val;
    if (!argc || !JS_IsFunction(ctx, argv[0]))
        return JS_UNDEFINED;
    int delay = 0;
    if (argc > 1)
        JS_ToInt32(ctx, &delay, argv[1]);
    if (delay < 0)
        delay = 0;
    WebTimer *timer = web_malloc(sizeof(WebTimer));
    memset(timer, 0, sizeof(*timer));
    timer->fn = JS_DupValue(ctx, argv[0]);
    timer->id = ++js_timer_id;
    timer->due = frame_clock + (uint32_t)delay;
    timer->next = js_timers;
    js_timers = timer;
    return JS_NewInt32(ctx, timer->id);
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
    WebTimer *timer = web_malloc(sizeof(WebTimer));
    memset(timer, 0, sizeof(*timer));
    timer->fn = JS_DupValue(ctx, argv[0]);
    timer->id = ++js_timer_id;
    timer->due = frame_clock + 16;
    timer->next = js_timers;
    js_timers = timer;
    return JS_NewInt32(ctx, timer->id);
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
        JS_SetPropertyStr(js_context, event, "currentTarget", make_element(js_context, node));
        WebJsState *state = state_of(node);
        for (WebListener *listener = state ? state->listeners : 0; listener;
             listener = listener->next) {
            if (strcmp(listener->type, type))
                continue;
            JSValue result = JS_Call(js_context, listener->fn, JS_UNDEFINED, 1, (JSValueConst *)&event);
            if (JS_IsException(result)) {
                JSValue error = JS_GetException(js_context);
                const char *text = JS_ToCString(js_context, error);
                if (text)
                    serial_write(text);
                if (text)
                    JS_FreeCString(js_context, text);
                JS_FreeValue(js_context, error);
            }
            JS_FreeValue(js_context, result);
        }
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
            return JS_NewString(ctx, "complete");
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
    JS_CFUNC_DEF("appendChild", 1, js_append_child),
    JS_CFUNC_DEF("removeChild", 1, js_remove_child),
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
        {0, 0, 0, 0, 0}};
    JS_SetPropertyFunctionList(ctx, object, properties,
                                (int)(sizeof(properties) / sizeof(properties[0])) - 1);
    JS_SetPropertyFunctionList(ctx, object, style_functions,
                                (int)(sizeof(style_functions) / sizeof(style_functions[0])) - 1);
}

static void build_document(JSContext *ctx, WebNode *root) {
    JSValue document = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, document, "getElementById",
                       JS_NewCFunction(ctx, js_get_element_by_id, "getElementById", 1));
    JS_SetPropertyStr(ctx, document, "querySelector",
                       JS_NewCFunction(ctx, js_query_selector, "querySelector", 1));
    JS_SetPropertyStr(ctx, document, "querySelectorAll",
                       JS_NewCFunction(ctx, js_query_selector_all, "querySelectorAll", 1));
    JS_SetPropertyStr(ctx, document, "createElement",
                       JS_NewCFunction(ctx, js_create_element, "createElement", 1));
    JS_SetPropertyStr(ctx, document, "createTextNode",
                       JS_NewCFunction(ctx, js_create_text_node, "createTextNode", 1));
    JS_SetPropertyStr(ctx, document, "addEventListener",
                       JS_NewCFunction(ctx, js_add_event_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, document, "removeEventListener",
                       JS_NewCFunction(ctx, js_remove_event_listener, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, document, "dispatchEvent",
                       JS_NewCFunction(ctx, js_dispatch_event, "dispatchEvent", 1));
    static const JSCFunctionListEntry properties[] = {
        {"body", JS_PROP_CONFIGURABLE, JS_DEF_CGETSET_MAGIC, PROP_BODY, 0},
        {"documentElement", JS_PROP_CONFIGURABLE, JS_DEF_CGETSET_MAGIC, PROP_DOCUMENT_ELEMENT, 0},
        {"head", JS_PROP_CONFIGURABLE, JS_DEF_CGETSET_MAGIC, PROP_HEAD, 0},
        {"title", JS_PROP_CONFIGURABLE, JS_DEF_CGETSET_MAGIC, PROP_TITLE, 0},
        {"readyState", JS_PROP_CONFIGURABLE, JS_DEF_CGETSET_MAGIC, PROP_READY_STATE, 0},
        {0, 0, 0, 0, 0}};
    JS_SetPropertyFunctionList(ctx, document, properties,
                                (int)(sizeof(properties) / sizeof(properties[0])) - 1);
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "document", document);
    JS_FreeValue(ctx, global);
}
static void build_window(JSContext *ctx, const char *url) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue window = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, window, "setTimeout",
                       JS_NewCFunction(ctx, js_set_timeout, "setTimeout", 2));
    JS_SetPropertyStr(ctx, window, "clearTimeout",
                       JS_NewCFunction(ctx, js_clear_timeout, "clearTimeout", 1));
    JS_SetPropertyStr(ctx, window, "requestAnimationFrame",
                       JS_NewCFunction(ctx, js_request_animation_frame, "requestAnimationFrame", 1));
    JS_SetPropertyStr(ctx, window, "addEventListener",
                       JS_NewCFunction(ctx, js_add_event_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, window, "removeEventListener",
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
    JS_SetPropertyStr(ctx, window, "location", location);
    JS_SetPropertyStr(ctx, global, "window", window);
    JS_SetPropertyStr(ctx, global, "setTimeout",
                       JS_NewCFunction(ctx, js_set_timeout, "setTimeout", 2));
    JS_SetPropertyStr(ctx, global, "clearTimeout",
                       JS_NewCFunction(ctx, js_clear_timeout, "clearTimeout", 1));
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
bool web_js_run(WebNode *root, char *error, size_t ecap) {
    if (error && ecap)
        error[0] = 0;
    if (!root)
        return false;
    web_js_release();
    js_runtime = JS_NewRuntime();
    if (!js_runtime) {
        if (error)
            append(error, "JavaScript runtime could not be created.", ecap);
        return false;
    }
    /* Runtime allocations use the growable physical-memory-backed heap. */
    JS_SetMaxStackSize(js_runtime, 48 * 1024);
    JS_SetInterruptHandler(js_runtime, interrupt_js, 0);
    js_context = JS_NewContext(js_runtime);
    if (!js_context) {
        JS_FreeRuntime(js_runtime);
        js_runtime = 0;
        if (error)
            append(error, "JavaScript context could not be created.", ecap);
        return false;
    }
    js_document = root;
    js_deadline = frame_clock + 250;
    build_document(js_context, root);
    build_window(js_context, web_document_url());
    WebNode *scripts[32];
    int count = 0;
    collect_scripts(root, scripts, &count);
    bool failed = false;
    for (int i = 0; i < count; i++) {
        const char *src = web_attr(scripts[i], "src");
        if (src)
            continue; /* external scripts are not fetched yet */
        char source[16384];
        source[0] = 0;
        web_node_text(scripts[i], source, sizeof(source));
        if (!source[0])
            continue;
        js_deadline = frame_clock + 250;
        JSValue value = JS_Eval(js_context, source, strlen(source), "<page>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(value)) {
            /* A failing script must not stop the page from being shown. */
            if (!failed) {
                failed = true;
                JSValue exception = JS_GetException(js_context);
                const char *text = JS_ToCString(js_context, exception);
                if (error) {
                    append(error, "JavaScript error: ", ecap);
                    append(error, text ? text : "unknown", ecap);
                }
                if (text)
                    JS_FreeCString(js_context, text);
                JS_FreeValue(js_context, exception);
            }
        }
        JS_FreeValue(js_context, value);
        drain_jobs();
    }
    if (!failed) {
        web_js_dispatch(root, "DOMContentLoaded", root);
        drain_jobs();
        web_js_dispatch(root, "load", root);
        drain_jobs();
    }
    js_document_dirty = true;
    return !failed;
}
void web_js_pump(void) {
    if (!js_context || !js_timers)
        return;
    bool ran = false;
    for (WebTimer *timer = js_timers; timer; timer = timer->next) {
        if (timer->cancelled || timer->due > frame_clock)
            continue;
        timer->cancelled = true;
        js_deadline = frame_clock + 100;
        JSValue result = JS_Call(js_context, timer->fn, JS_UNDEFINED, 0, 0);
        if (JS_IsException(result)) {
            JSValue exception = JS_GetException(js_context);
            const char *text = JS_ToCString(js_context, exception);
            if (text)
                serial_write(text);
            if (text)
                JS_FreeCString(js_context, text);
            JS_FreeValue(js_context, exception);
        }
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
    if (ran) {
        drain_jobs();
        js_document_dirty = true;
    }
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
}
void web_js_release(void) {
    if (js_document)
        release_states(js_document);
    js_document = 0;
    WebTimer *timer = js_timers;
    while (timer) {
        WebTimer *next = timer->next;
        if (js_context)
            JS_FreeValue(js_context, timer->fn);
        web_free(timer);
        timer = next;
    }
    js_timers = 0;
    if (js_context) {
        JS_FreeContext(js_context);
        js_context = 0;
    }
    if (js_runtime) {
        JS_FreeRuntime(js_runtime);
        js_runtime = 0;
    }
}
