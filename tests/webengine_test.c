/* Native unit check for the Luna web engine: HTML tree, CSS cascade and
 * line layout. Built and run on the host, outside the guest image. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ui/webengine.h"

int failures;

static void check(bool condition, const char *what) {
    if (!condition) {
        printf("FAIL %s\n", what);
        failures++;
    } else
        printf("ok   %s\n", what);
}

/* Stubs for the framebuffer primitives used by the layout module. */
void kernel_fault(void) {
    printf("kernel fault\n");
    abort();
}
void append(char *d, const char *s, size_t cap) {
    size_t n = strlen(d);
    while (*s && n + 1 < cap)
        d[n++] = *s++;
    d[n] = 0;
}
void number(char *s, uint32_t value) {
    sprintf(s, "%u", value);
}
int text_width(const char *s, int size) {
    (void)size;
    return (int)strlen(s) * 8;
}
void text(int x, int y, const char *s, int size, uint32_t color) {
    (void)x;
    (void)y;
    (void)s;
    (void)size;
    (void)color;
}
void rect(int x, int y, int w, int h, uint32_t c) {
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)c;
}
void line(int x0, int y0, int x1, int y1, uint32_t c) {
    (void)x0;
    (void)y0;
    (void)x1;
    (void)y1;
    (void)c;
}

static const char *document_html =
    "<!doctype html><html><head><style>"
    "body { margin: 0; color: #112233; }"
    "h1 { font-size: 32px; color: red; }"
    ".note { color: blue; padding: 4px; }"
    "div p { font-weight: 700; }"
    "</style></head><body>"
    "<h1 id=\"title\">Hello</h1>"
    "<p class=\"note\">World &amp; friends</p>"
    "<div><p>Nested</p></div>"
    "<ul><li>First</li><li>Second</li></ul>"
    "<script>var x = 1;</script>"
    "</body></html>";

int main(void) {
    WebNode *root = 0;
    char error[128];
    check(web_parse(document_html, strlen(document_html), &root, error, sizeof(error)) && root,
          "document parses");
    WebNode *title = web_find_by_id(root, "title");
    check(title != 0 && !strcmp(title->tag, "h1"), "element with id is found");
    char text[128];
    web_node_text(title, text, sizeof(text));
    check(!strcmp(text, "Hello"), "text content is collected");
    WebNode *body = web_find_first(root, "body");
    check(body != 0, "body element exists");
    check(web_find_first(root, "script") != 0, "script element is parsed");

    char sheet[1024];
    sheet[0] = 0;
    WebNode *style = web_find_first(root, "style");
    if (style)
        web_node_text(style, sheet, sizeof(sheet));
    WebRule *rules = 0;
    int count = 0;
    check(css_parse_sheet(sheet, &rules, &count) && count == 4, "stylesheet parses four rules");
    css_apply(root, rules, count);

    check(title->style.color == 0xff0000, "h1 color comes from the type selector");
    check(title->style.font_size == 32, "h1 font size is applied");
    WebNode *note = 0;
    for (WebNode *c = body->first_child; c; c = c->next)
        if (c->type == WEB_ELEMENT && !strcmp(c->tag, "p"))
            note = c;
    check(note != 0, "paragraph exists");
    if (note) {
        check(note->style.color == 0x0000ff, "class selector wins over the type selector");
        check(note->style.padding[0] == 4, "class padding is applied");
        char value[64];
        web_node_text(note, value, sizeof(value));
        check(!strcmp(value, "World & friends"), "entities are decoded");
    }
    WebNode *nested = 0;
    WebNode *div = web_find_first(root, "div");
    if (div)
        nested = web_find_first(div, "p");
    check(nested != 0, "nested paragraph exists");
    if (nested)
        check(nested->style.font_weight == 700, "descendant selector applies");
    check(body->style.color == 0x112233, "body color is applied");

    web_layout(root, 400, 300);
    check(web_document_height() > 0, "document has a height");
    WebNode *first_p = note;
    if (first_p) {
        check(first_p->y > title->y, "blocks are stacked vertically");
        check(first_p->content_x >= 0, "content box has a position");
    }
    WebNode *list = web_find_first(root, "ul");
    check(list != 0 && list->style.padding[3] == 40, "list keeps its left padding");

    /* Selector matching used by querySelector. */
    WebNode *by_class = 0;
    for (WebNode *c = root->first_child; c; c = c->next)
        if (c->type == WEB_ELEMENT)
            by_class = c;
    check(css_match_selector("h1#title", title), "compound selector matches");
    check(!css_match_selector("h2#title", title), "wrong type does not match");
    check(css_match_selector("body h1", title), "descendant selector matches");
    check(css_match_selector("div p", nested), "descendant with two levels matches");
    check(css_match_selector("h1 > span", title) == false, "child selector does not match");

    css_free_sheet(rules, count);
    web_free_tree(root);
    printf(failures ? "\n%d check(s) failed\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
