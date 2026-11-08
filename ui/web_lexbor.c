/* Luna Browser Lexbor adapter.
 * Upstream: https://github.com/lexbor/lexbor (Apache-2.0), pinned in
 * third_party/lexbor at v3.0.0. This adapter copies the parsed DOM into the
 * temporary legacy WebNode model used by Luna's CSS/layout/QuickJS.
 *
 * This is NOT a native Lexbor-backed live DOM. DOM mutations still operate on
 * WebNode and do not propagate back into the Lexbor document.
 */
#include "webengine.h"
#include "lexbor/html/html.h"
#include "lexbor/dom/interfaces/character_data.h"
#include "lexbor/dom/interfaces/document_fragment.h"
#include "lexbor/html/interfaces/template_element.h"

static unsigned char luna_ns(uintptr_t ns) {
    if (ns == LXB_NS_SVG)
        return WEB_NS_SVG;
    if (ns == LXB_NS_MATH)
        return WEB_NS_MATHML;
    return WEB_NS_HTML;
}

static WebNode *convert_node(lxb_dom_node_t *src, unsigned depth) {
    if (!src || depth > 512)
        return NULL;

    WebNode *dst = NULL;
    if (src->type == LXB_DOM_NODE_TYPE_DOCUMENT) {
        dst = web_node_new(WEB_ELEMENT); /* legacy WebNode contract */
        dst->tag = web_strdup("#document");
    }
    else if (src->type == LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT) {
        dst = web_node_new(WEB_DOCUMENT_FRAGMENT);
    }
    else if (src->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        dst = web_node_new(WEB_ELEMENT);
        lxb_dom_element_t *el = lxb_dom_interface_element(src);
        size_t length = 0;
        const lxb_char_t *name = lxb_dom_element_local_name(el, &length);
        dst->tag = web_strndup((const char *) name, length);
        dst->namespace_id = luna_ns(src->ns);

        for (lxb_dom_attr_t *attr = lxb_dom_element_first_attribute(el);
             attr; attr = lxb_dom_element_next_attribute(attr)) {
            size_t nlen = 0, vlen = 0;
            const lxb_char_t *aname = lxb_dom_attr_qualified_name(attr, &nlen);
            const lxb_char_t *value = lxb_dom_attr_value(attr, &vlen);
            char *key = web_strndup((const char *) aname, nlen);
            char *val = value ? web_strndup((const char *) value, vlen) : web_strdup("");
            web_set_attr(dst, key, val);
            web_free(key);
            web_free(val);
        }

        /* Inert template contents live outside the ordinary DOM tree. */
        if (src->ns == LXB_NS_HTML && dst->tag &&
            !strcmp(dst->tag, "template")) {
            lxb_html_template_element_t *tpl =
                (lxb_html_template_element_t *) src;
            if (tpl->content) {
                dst->template_content = convert_node(
                    lxb_dom_interface_node(tpl->content), depth + 1);
            }
        }
    }
    else if (src->type == LXB_DOM_NODE_TYPE_TEXT ||
             src->type == LXB_DOM_NODE_TYPE_COMMENT) {
        dst = web_node_new(src->type == LXB_DOM_NODE_TYPE_TEXT ? WEB_TEXT : WEB_COMMENT);
        const lxb_dom_character_data_t *data =
            (const lxb_dom_character_data_t *) src;
        dst->text = web_strndup((const char *) data->data.data, data->data.length);
    }
    else {
        /* The legacy renderer does not draw doctype nodes. */
        return NULL;
    }

    for (lxb_dom_node_t *child = src->first_child; child; child = child->next) {
        WebNode *converted = convert_node(child, depth + 1);
        if (converted)
            web_node_append(dst, converted);
    }
    return dst;
}

bool web_parse_lexbor(const char *html, size_t len, WebNode **root,
                      char *error, size_t ecap) {
    if (!root || (!html && len))
        return false;
    *root = NULL;
    lxb_html_document_t *document = lxb_html_document_create();
    if (!document) {
        if (error && ecap)
            append(error, "Lexbor document allocation failed.", ecap);
        return false;
    }
    lxb_status_t result = lxb_html_document_parse(
        document, (const lxb_char_t *)(html ? html : ""), len);
    if (result == LXB_STATUS_OK)
        *root = convert_node(lxb_dom_interface_node(document), 0);
    lxb_html_document_destroy(document);
    if (!*root && error && ecap)
        append(error, "Lexbor HTML parse failed.", ecap);
    return *root != NULL;
}
