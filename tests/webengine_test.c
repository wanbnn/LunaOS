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



static void dom_mutation_regressions(void) {
    WebNode *document = web_node_new(WEB_ELEMENT);
    document->tag = web_strdup("#document");
    WebNode *parent = web_node_new(WEB_ELEMENT);
    parent->tag = web_strdup("main");
    WebNode *first = web_node_new(WEB_ELEMENT);
    first->tag = web_strdup("a");
    WebNode *second = web_node_new(WEB_ELEMENT);
    second->tag = web_strdup("b");
    WebNode *third = web_node_new(WEB_ELEMENT);
    third->tag = web_strdup("c");
    web_node_append(document, parent);
    web_node_append(parent, first);
    web_node_append(parent, second);
    web_node_append(parent, third);
    check(web_node_contains(document, third) && !web_node_contains(third, parent),
          "DOM ancestry follows parent pointers");
    check(!web_node_insert_before(first, parent, 0) &&
          !web_node_insert_before(parent, parent, 0),
          "DOM cannot move an ancestor under its descendant or itself");
    check(web_node_insert_before(parent, third, first) &&
          parent->first_child == third && third->next == first &&
          first->prev == third && parent->last_child == second,
          "insertBefore moves nodes and repairs sibling links");
    check(web_node_insert_before(parent, first, first) &&
          third->next == first && first->next == second,
          "insertBefore of a node before itself is a no-op");
    check(!web_node_insert_before(parent, second, document) &&
          second->parent == parent,
          "insertBefore rejects a reference outside the parent");
    web_node_detach(first);
    check(!first->parent && !first->next && !first->prev &&
          third->next == second && second->prev == third,
          "detach preserves remaining links and leaves node reusable");
    check(web_node_insert_before(parent, first, 0) &&
          parent->last_child == first && first->prev == second,
          "detached node can be reattached at the end");
    web_set_attr(parent, "data-state", "source");
    WebNode *shallow = web_node_clone(parent, false);
    WebNode *deep = web_node_clone(parent, true);
    check(shallow && !shallow->first_child &&
          !strcmp(web_attr(shallow, "data-state"), "source"),
          "shallow clone copies attributes but not children");
    check(deep && deep->first_child && deep->first_child != parent->first_child &&
          deep->first_child->parent == deep,
          "deep clone owns independent descendant nodes");
    web_set_attr(deep, "data-state", "copy");
    check(!strcmp(web_attr(parent, "data-state"), "source"),
          "cloned attributes do not alias the original strings");
    web_free_tree(shallow);
    web_free_tree(deep);
    web_free_tree(document);
}

static void foreign_namespace_regressions(void) {
    const char *html = "<html><body><svg id='svg'><g id='group'>"
                       "<foreignObject><div id='foreign-html'>text</div></foreignObject>"
                       "</g></svg><math id='math'><mi id='math-child'>x</mi></math></body></html>";
    WebNode *root = 0;
    char error[128] = {0};
    check(web_parse(html, strlen(html), &root, error, sizeof(error)),
          "foreign namespace fixture parses");
    if (!root)
        return;
    WebNode *svg = web_find_by_id(root, "svg");
    WebNode *group = web_find_by_id(root, "group");
    WebNode *html_child = web_find_by_id(root, "foreign-html");
    WebNode *math = web_find_by_id(root, "math");
    WebNode *math_child = web_find_by_id(root, "math-child");
    check(svg && group && svg->namespace_id == WEB_NS_SVG &&
          group->namespace_id == WEB_NS_SVG,
          "SVG child nodes inherit SVG namespace");
    check(html_child && html_child->namespace_id == WEB_NS_HTML,
          "foreignObject integration point returns to HTML namespace");
    check(math && math_child && math->namespace_id == WEB_NS_MATHML &&
          math_child->namespace_id == WEB_NS_MATHML,
          "MathML children inherit MathML namespace");
    WebNode *clone = web_node_clone(svg, true);
    check(clone && clone->namespace_id == WEB_NS_SVG &&
          clone->first_child && clone->first_child->namespace_id == WEB_NS_SVG,
          "DOM cloning preserves namespace identities");
    web_free_tree(clone);
    web_free_tree(root);
}


static void template_shadow_regressions(void) {
    const char *html = "<html><body><template id='template'><p id='inert'>"
                       "inert text</p><span>more</span></template><p id='live'>live</p></body></html>";
    WebNode *document = 0;
    char error[128] = {0};
    check(web_parse(html, strlen(html), &document, error, sizeof(error)),
          "HTML template fixture parses");
    if (!document)
        return;
    WebNode *tmpl = web_find_by_id(document, "template");
    check(tmpl && tmpl->template_content &&
          tmpl->template_content->type == WEB_DOCUMENT_FRAGMENT &&
          !tmpl->first_child && tmpl->template_content->first_child,
          "template descendants are stored in an inert DocumentFragment");
    check(web_find_by_id(document, "inert") == 0 &&
          web_find_by_id(tmpl->template_content, "inert") != 0,
          "template content is excluded from document tree searches");
    WebNode *clone = web_node_clone(tmpl, true);
    check(clone && clone->template_content && clone->template_content->first_child &&
          clone->template_content->first_child != tmpl->template_content->first_child,
          "cloning a template copies its content subtree");
    web_free_tree(clone);
    WebNode *host = web_find_by_id(document, "live");
    WebNode *shadow = web_attach_shadow(host, true);
    check(shadow && shadow->type == WEB_SHADOW_ROOT && shadow->host == host &&
          host->shadow_root == shadow && host->shadow_open,
          "open ShadowRoot is owned by its host");
    check(web_attach_shadow(host, false) == 0,
          "host cannot attach a second ShadowRoot");
    WebNode *span = web_node_new(WEB_ELEMENT);
    span->tag = web_strdup("span");
    web_node_append(shadow, span);
    check(!web_find_first(document, "span") && span->parent == shadow,
          "shadow descendants remain outside host light-DOM traversal");
    check(web_node_root(span) == shadow &&
          web_node_root_composed(span) == document &&
          web_node_is_connected(span),
          "shadow nodes use their own root but remain composed-connected to document");
    web_node_detach(host);
    check(!web_node_is_connected(span) && web_node_root_composed(span) == host,
          "detaching shadow host disconnects composed subtree");
    web_node_append(web_find_first(document, "body"), host);
    WebNode *fragment = web_node_new(WEB_DOCUMENT_FRAGMENT);
    WebNode *one = web_node_new(WEB_ELEMENT);
    one->tag = web_strdup("b");
    WebNode *two = web_node_new(WEB_ELEMENT);
    two->tag = web_strdup("i");
    web_node_append(fragment, one);
    web_node_append(fragment, two);
    check(web_node_insert_before(host, fragment, 0) && !fragment->first_child &&
          host->last_child == two && one->next == two && two->prev == one &&
          one->parent == host && two->parent == host,
          "inserting DocumentFragment moves children in document order");
    web_free_tree(fragment);
    web_free_tree(document);
}


static void dom_core_phase2_regressions(void) {
    WebNode *document = web_node_new(WEB_ELEMENT);
    document->tag = web_strdup("#document");
    WebNode *parent = web_node_new(WEB_ELEMENT);
    parent->tag = web_strdup("div");
    WebNode *first = web_node_new(WEB_ELEMENT);
    first->tag = web_strdup("first");
    WebNode *replacement = web_node_new(WEB_ELEMENT);
    replacement->tag = web_strdup("replacement");
    web_node_append(document, parent);
    web_node_append(parent, first);
    check(web_node_root(first) == document &&
          web_node_is_connected(first),
          "getRootNode and isConnected follow attached tree");
    check(web_node_replace_child(parent, replacement, first) &&
          first->parent == 0 && parent->first_child == replacement &&
          parent->last_child == replacement,
          "replaceChild detaches old child without freeing it");
    check(!web_node_is_connected(first) && web_node_root(first) == first,
          "removed subtree remains independently accessible");
    check(!web_node_replace_child(parent, parent, replacement) &&
          replacement->parent == parent,
          "replaceChild blocks ancestor cycles");
    WebNode *a = web_node_new(WEB_TEXT);
    a->text = web_strdup("alpha");
    WebNode *empty = web_node_new(WEB_TEXT);
    empty->text = web_strdup("");
    WebNode *b = web_node_new(WEB_TEXT);
    b->text = web_strdup("beta");
    web_node_append(parent, a);
    web_node_append(parent, empty);
    web_node_append(parent, b);
    web_node_normalize(parent);
    check(a->next == 0 && !strcmp(a->text, "alphabeta"),
          "normalize merges adjacent text nodes and removes empty nodes");
    check(!web_node_is_connected(first), "removed node stays detached after normalization");
    web_free_tree(first);
    web_free_tree(document);
}


static void html_entities_full_regressions(void) {
    const char *html =
        "<html><body>"
        "<p id='named'>&acE; &notin; &Aacute &fjlig; &CounterClockwiseContourIntegral;</p>"
        "<p id='numeric'>&#128; &#x80; &#x1F600;</p>"
        "<div id='attr' data-ambiguous='&notit;' data-legacy='&copy text' "
        "data-escaped='a&amp;b'></div>"
        "</body></html>";
    WebNode *root = 0;
    char error[128] = {0};
    check(web_parse(html, strlen(html), &root, error, sizeof(error)),
          "full named entity fixture parses");
    if (!root)
        return;
    WebNode *named = web_find_by_id(root, "named");
    char value[256];
    web_node_text(named, value, sizeof(value));
    check(strstr(value, "\xe2\x88\xbe\xcc\xb3") != 0 &&
          strstr(value, "\xe2\x88\x89") != 0 &&
          strstr(value, "\xc3\x81") != 0 &&
          strstr(value, "fj") != 0,
          "named entities support two codepoints, legacy and uncommon references");
    WebNode *numeric = web_find_by_id(root, "numeric");
    web_node_text(numeric, value, sizeof(value));
    check(strstr(value, "\xe2\x82\xac") != 0 &&
          strstr(value, "\xf0\x9f\x98\x80") != 0,
          "numeric references remap C1 characters and decode astral planes");
    WebNode *attr = web_find_by_id(root, "attr");
    check(attr && !strcmp(web_attr(attr, "data-ambiguous"), "&notit;") &&
          !strcmp(web_attr(attr, "data-legacy"), "\xc2\xa9 text") &&
          !strcmp(web_attr(attr, "data-escaped"), "a&b"),
          "attribute context refuses ambiguous semicolonless named references");
    web_free_tree(root);
}

static void html_dom_regressions(void) {
    const char *html =
        "<!doctype html><html><head><title>A &amp; B</TITLE  ></head><body>"
        "<section id='tree'>first <b>second</b> third <i>fourth</i></section>"
        "<!-- a real DOM comment -->"
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
    WebNode *comment = tree ? tree->next : 0;
    check(comment && comment->type == WEB_COMMENT &&
          !strcmp(comment->text, " a real DOM comment "),
          "HTML comments become DOM comment nodes with preserved data");
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
    check(strstr(value, "\xF0\x9F\x98\x80") != 0 &&
          strstr(value, "\xEF\xBF\xBD") != 0 &&
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
    dom_mutation_regressions();
    foreign_namespace_regressions();
    template_shadow_regressions();
    dom_core_phase2_regressions();
    html_entities_full_regressions();
    printf(failures ? "\n%d check(s) failed\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
