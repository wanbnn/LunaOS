/* Native unit check for the Luna web engine: HTML tree, CSS cascade and
 * line layout. Built and run on the host, outside the guest image. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ui/webengine.h"

int failures;
void gfx_rgba_image(int x, int y, int w, int h, const unsigned char *rgba, int sw, int sh) {
    (void)x; (void)y; (void)w; (void)h; (void)rgba; (void)sw; (void)sh;
}
void gfx_get_clip(int out[4]) { out[0] = out[1] = 0; out[2] = 1024; out[3] = 768; }
void gfx_clip(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }

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

static void layout_regressions(void) {
    const char *html =
        "<html><head></head><body>"
        "<div id='row'><div id='one' class='col'>One</div>\n"
        "<div id='two' class='col'>Two</div><div id='three' class='col'>Three</div></div>"
        "<p id='after'>After</p><div id='reader'>Hidden label</div>"
        "<pre id='code'>&gt;&gt;&gt; x\n    y</pre>"
        "<div id='cascade' class='notice' style='color: red'>Cascade</div>"
        "</body></html>";
    const char *sheet =
        "* {box-sizing: border-box;} body {margin:0;}"
        "#row {overflow:hidden;} .col {float:left; width:33.333333%; padding:8px; border:1px solid #123456;}"
        "#after {clear:both;} #reader {position:absolute; width:1px; height:1px; clip:rect(0 0 0 0);}"
        "#absent, .notice {color:blue;} div.notice {color:green !important;}"
        "#cascade {/* a comment inside a declaration block */ background-color:black;}"
        "* html #row {height:1%;}";
    WebNode *root = 0;
    WebRule *rules = 0;
    int count = 0;
    char error[128] = {0};
    check(web_parse(html, strlen(html), &root, error, sizeof(error)), "column fixture parses");
    css_parse_sheet(sheet, &rules, &count);
    css_apply(root, rules, count);
    web_layout(root, 600, 400);
    WebNode *one = web_find_by_id(root, "one"), *two = web_find_by_id(root, "two");
    WebNode *three = web_find_by_id(root, "three"), *row = web_find_by_id(root, "row");
    check(one->y == two->y && two->y == three->y && one->x < two->x && two->x < three->x,
          "fractional percentage floats share one horizontal row");
    check(one->w == 199 && one->content_w == 181, "border-box accounts for padding and borders");
    check(row->h >= one->h && row->h > 6, "float container encloses columns and ignores the IE star-html hack");
    check(web_find_by_id(root, "after")->y >= one->y + one->h, "clear places the next block below floats");
    WebNode *reader = web_find_by_id(root, "reader");
    check(reader->style.position == 2 && reader->style.clip_empty, "screen-reader label is positioned and clipped");
    WebNode *code = web_find_by_id(root, "code");
    char value[80];
    web_node_text(code, value, sizeof(value));
    check(!strcmp(value, ">>> x\n    y") && code->h >= 40, "pre preserves lines and indentation and decodes entities");
    WebNode *cascade = web_find_by_id(root, "cascade");
    check(cascade->style.color == 0x008000, "author important overrides normal inline color");
    check(cascade->style.background_set && cascade->style.background == 0,
          "comments in declarations are skipped and black is an opaque background");
    check(css_match_selector(".col:first-child", one), "first-child ignores whitespace text siblings");
    check(css_match_selector("div:not(.notice)", one) && !css_match_selector("div:not(.notice)", cascade),
          "functional not selectors exclude the matching class");
    css_free_sheet(rules, count);
    css_parse_sheet("#absent, .notice {color:blue;} div.notice {color:green;}", &rules, &count);
    css_apply(root, rules, count);
    /* Remove inline red before checking selector-list specificity. */
    web_set_attr(cascade, "style", "");
    css_apply(root, rules, count);
    check(cascade->style.color == 0x008000, "selector lists use the specificity of the selector that matched");
    css_free_sheet(rules, count);
    web_free_fragments();
    web_free_tree(root);
}


static void html_dom_regressions(void) {
    const char *html =
        "<!doctype html><html><head><title>A &amp; B</TITLE  ></head><body>"
        "<section id='tree'>first <b>second</b> third <i>fourth</i></section>"
        "<input disabled checked='' id='control' id='ignored'>"
        "<div id='outer'/><span id='nested'>inside</span></div>"
        "<textarea id='area'>A &lt; B &amp; C</TEXTAREA >"
        "<script id='script'>if (a < b) { x = '&amp;'; }</SCRIPT >"
        "<p id='unicode'>&#x1F600; &#128512; &#x110000; &#0; &#xZZ; tail</p>"
        "<p id='end'>before</p><p id='next'>after</p>"
        "</body></html>";
    WebNode *root = 0;
    char error[128] = {0};
    check(web_parse(html, strlen(html), &root, error, sizeof(error)),
          "HTML/DOM regression fixture parses");
    if (!root)
        return;
    char value[256];
    WebNode *tree = web_find_by_id(root, "tree");
    web_node_text(tree, value, sizeof(value));
    check(!strcmp(value, "first second third fourth"),
          "textContent collects all text children in document order");
    WebNode *control = web_find_by_id(root, "control");
    check(control && !strcmp(web_attr(control, "disabled"), "") &&
          !strcmp(web_attr(control, "checked"), "") &&
          !strcmp(web_attr(control, "id"), "control"),
          "boolean HTML attributes are empty and duplicate attributes keep first value");
    WebNode *nested = web_find_by_id(root, "nested");
    check(nested && nested->parent == web_find_by_id(root, "outer"),
          "self-closing slash does not close a non-void HTML element");
    WebNode *area = web_find_by_id(root, "area");
    web_node_text(area, value, sizeof(value));
    check(!strcmp(value, "A < B & C"),
          "textarea RCDATA decodes entities and recognizes case-insensitive close");
    WebNode *script = web_find_by_id(root, "script");
    web_node_text(script, value, sizeof(value));
    check(!strcmp(value, "if (a < b) { x = '&amp;'; }"),
          "script raw text remains undecoded until case-insensitive end tag");
    WebNode *title = web_find_first(root, "title");
    web_node_text(title, value, sizeof(value));
    check(!strcmp(value, "A & B"), "title RCDATA decodes entities");
    WebNode *unicode = web_find_by_id(root, "unicode");
    web_node_text(unicode, value, sizeof(value));
    check(strstr(value, "\\xF0\\x9F\\x98\\x80") != 0 &&
          strstr(value, "\\xEF\\xBF\\xBD") != 0 &&
          strstr(value, "&#xZZ; tail") != 0,
          "numeric entities support supplementary planes and preserve malformed references");
    check(web_find_by_id(root, "next")->parent == web_find_by_id(root, "end")->parent,
          "explicit paragraph close preserves sibling structure");
    web_free_tree(root);
}

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
    check(css_match_selector("h1#title", title), "compound selector matches");
    check(!css_match_selector("h2#title", title), "wrong type does not match");
    check(css_match_selector("body h1", title), "descendant selector matches");
    check(css_match_selector("div p", nested), "descendant with two levels matches");
    check(css_match_selector("h1 > span", title) == false, "child selector does not match");

    check(css_length("1.5em", 20, 0, 0) == 30, "fractional em uses the font size");
    check(css_length(".75rem", 32, 0, 0) == 12, "rem uses the root font baseline");
    check(css_length("-2.5px", 16, 0, 0) == -2, "negative fractional pixels parse");
    check(css_length("12bogus", 16, 0, 0) == 0, "unsupported units are rejected");
    css_free_sheet(rules, count);
    rules = 0;
    count = 0;
    const char *responsive =
        "h1 {color: red; width: auto; margin: 0 7px 0 19px; padding: .5em;}"
        "@media print {h1 {color: black;}}"
        "@media screen and (min-width: 600px) {h1 {color: blue;}}"
        "@media screen and (max-width: 599px) {h1 {color: green;}}"
        "p {color: purple;}";
    css_set_viewport(800);
    check(!css_media_matches("print"), "print media is excluded");
    check(css_media_matches("only screen and (min-width: 40em)"), "relative media widths parse");
    css_parse_sheet(responsive, &rules, &count);
    css_apply(root, rules, count);
    web_layout(root, 800, 600);
    check(title->style.color == 0x0000ff, "desktop screen media applies without print styles");
    check(title->style.width == -1 && title->w > 0, "auto width does not collapse to zero");
    check(title->x == body->content_x + 19, "asymmetric left margin uses the left side");
    check(title->style.padding[0] == title->style.font_size / 2, "padding supports fractional em");
    check(note->style.color == 0x800080, "rules after media blocks are preserved");
    css_free_sheet(rules, count);
    css_set_viewport(400);
    css_parse_sheet(responsive, &rules, &count);
    css_apply(root, rules, count);
    web_layout(root, 400, 300);
    check(title->style.color == 0x008000, "narrow viewport selects the mobile media rule");
    for (int i = 0; i < 10; i++)
        web_layout(root, 400, 300);
    web_free_fragments();

    css_free_sheet(rules, count);
    web_free_tree(root);
    layout_regressions();
    html_dom_regressions();
    printf(failures ? "\n%d check(s) failed\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
