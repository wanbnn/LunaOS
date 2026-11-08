/* Host integration tests for the Lexbor-backed HTML parser.
 * Run with LUNA_USE_LEXBOR, linked to pinned Lexbor v3.0.0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ui/webengine.h"

static int failures;
void kernel_fault(void) { abort(); }
void append(char *dest, const char *text, size_t cap) {
    size_t n = strlen(dest);
    if (!cap) return;
    while (*text && n + 1 < cap)
        dest[n++] = *text++;
    dest[n] = 0;
}
static void test(bool value, const char *description) {
    printf("%s %s\n", value ? "PASS" : "FAIL", description);
    if (!value) failures++;
}

static void parser_smoke(void) {
    const char *html = "<!doctype html><title>Test</title>"
                       "<p id='one'>first <b>bold</b><p id='two'>second &acE;";
    WebNode *root = NULL;
    char error[160] = {0};
    test(web_parse(html, strlen(html), &root, error, sizeof(error)),
         "Lexbor parses HTML5 document with omitted html/head/body tags");
    if (!root) return;
    WebNode *html_node = web_find_first(root, "html");
    WebNode *head = web_find_first(root, "head");
    WebNode *body = web_find_first(root, "body");
    WebNode *one = web_find_by_id(root, "one");
    WebNode *two = web_find_by_id(root, "two");
    test(html_node && head && body, "HTML5 creates implied html/head/body");
    test(one && two && one->parent == two->parent,
         "HTML5 tree builder implicitly closes p before next p");
    char text[160] = {0};
    web_node_text(two, text, sizeof(text));
    test(strstr(text, "second ") && strstr(text, "\xe2\x88\xbe\xcc\xb3"),
         "Lexbor decodes multiple Unicode code points in named character reference");
    web_free_tree(root);
}

static void template_and_foreign(void) {
    const char *html = "<template id='t'><p id='inert'>quiet</p></template>"
                       "<svg id='s'><linearGradient id='g'/>"
                       "<foreignObject><div id='foreign'>hello</div></foreignObject></svg>"
                       "<math id='m'><mi id='mi'>x</mi></math>";
    WebNode *root = NULL;
    char error[160] = {0};
    test(web_parse(html, strlen(html), &root, error, sizeof(error)),
         "Lexbor parses template and foreign content");
    if (!root) return;
    WebNode *tmpl = web_find_by_id(root, "t");
    WebNode *svg = web_find_by_id(root, "s");
    WebNode *grad = web_find_by_id(root, "g");
    WebNode *foreign = web_find_by_id(root, "foreign");
    WebNode *math = web_find_by_id(root, "m");
    test(tmpl && tmpl->template_content &&
         web_find_by_id(tmpl->template_content, "inert") &&
         !web_find_by_id(root, "inert"),
         "template descendants remain in inert document fragment");
    test(svg && grad && svg->namespace_id == WEB_NS_SVG &&
         grad->namespace_id == WEB_NS_SVG,
         "SVG descendants retain SVG namespace");
    test(foreign && foreign->namespace_id == WEB_NS_HTML,
         "foreignObject descendants switch back to HTML namespace");
    test(math && math->namespace_id == WEB_NS_MATHML,
         "MathML elements retain MathML namespace");
    web_free_tree(root);
}

int main(void) {
    parser_smoke();
    template_and_foreign();
    printf("%d Lexbor bridge failure(s)\n", failures);
    return failures ? 1 : 0;
}
