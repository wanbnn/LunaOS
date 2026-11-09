#include "../ui/litehtml_bridge.h"
#include "../ui/web_dom_types.h"
#include <cstdio>
#include <cstring>
struct DrawCounts { int glyphs, boxes, clips; };
static int width(void *, const char *s, int font) { return (int)std::strlen(s) * font / 2; }
static void text(void *ctx,int,int,const char *s,int,uint32_t) {
    auto *d=(DrawCounts*)ctx;
    if (s && *s) d->glyphs++;
}
static void rect(void *ctx,int,int,int w,int h,uint32_t) {
    if(w>0 && h>0) ((DrawCounts*)ctx)->boxes++;
}
static void clip(void *ctx,int,int,int,int) { ((DrawCounts*)ctx)->clips++; }
static void unclip(void *ctx) { ((DrawCounts*)ctx)->clips--; }
int main() {
    DrawCounts counts{};
    LunaLiteGraphics gfx{};
    gfx.context=&counts; gfx.text_width=width; gfx.draw_text=text;
    gfx.fill_rect=rect; gfx.push_clip=clip; gfx.pop_clip=unclip;
    auto element = [](const char *tag, WebNode *child = nullptr, WebNode *next = nullptr) {
        WebNode node{};
        node.type = WEB_ELEMENT;
        node.tag = const_cast<char *>(tag);
        node.first_child = child;
        node.next = next;
        return node;
    };
    auto text_node = [](const char *value) {
        WebNode node{};
        node.type = WEB_TEXT;
        node.text = const_cast<char *>(value);
        return node;
    };
    auto style_text = text_node("body{margin:0}main{display:flex;gap:16px;background:#abc}"
                                "main>a{padding:10px;width:80px;background:#ff0000}");
    auto style = element("style", &style_text);
    auto beta_text = text_node("Beta");
    auto beta = element("a", &beta_text);
    auto alpha_text = text_node("Alpha");
    auto alpha = element("a", &alpha_text, &beta);
    WebAttr href{nullptr, const_cast<char *>("href"), const_cast<char *>("/beta")};
    beta.attrs = &href;
    auto main = element("main", &alpha);
    auto body = element("body", &main);
    auto head = element("head", &style, &body);
    auto html = element("html", &head);
    auto root = element("#document", &html);
    LunaLiteDocument *d=luna_lite_create(&root,&gfx,640,480);
    if(!d) return 1;
    int h=luna_lite_layout(d,640,480);
    luna_lite_draw(d,0,0);
    std::printf("litehtml: height=%d glyphs=%d boxes=%d clips=%d\n",
                h,counts.glyphs,counts.boxes,counts.clips);
    bool ok=h>0 && counts.glyphs>=2 && counts.boxes>=2 && counts.clips==0 &&
            luna_lite_hit_link(d, 125, 12, 0) == &beta;
    luna_lite_destroy(d);
    if (!ok)
        return 2;

    auto wide = element("a");
    WebAttr wide_href{nullptr, const_cast<char *>("href"), const_cast<char *>("/wide")};
    WebAttr placement{&wide_href, const_cast<char *>("style"), const_cast<char *>("grid-column:1 / -1")};
    wide.attrs = &placement;
    beta.next = &wide;
    style_text.text = const_cast<char *>(
        "body{margin:0}main{display:grid;grid-template-columns:repeat(2,1fr);gap:10px}"
        "main>a{height:20px}");
    d = luna_lite_create(&root, &gfx, 640, 480);
    if (!d)
        return 3;
    ok = luna_lite_hit_link(d, 500, 10, 0) == &beta &&
         luna_lite_hit_link(d, 620, 35, 0) == &wide &&
         !luna_lite_hit_link(d, 620, 25, 0);
    luna_lite_destroy(d);
    if (!ok)
        return 4;

    beta.next = nullptr;
    style_text.text = const_cast<char *>(
        "body{margin:0}main{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:10px}"
        "main>a{height:20px}");
    d = luna_lite_create(&root, &gfx, 640, 480);
    if (!d)
        return 5;
    ok = luna_lite_hit_link(d, 540, 10, 0) == &beta &&
         luna_lite_layout(d, 400, 480) > 0 &&
         luna_lite_hit_link(d, 390, 35, 0) == &beta;
    luna_lite_destroy(d);
    if (ok)
        std::puts("PASS: grid tracks, gaps, full-width placement and responsive auto-fit");
    return ok?0:2;
}
