#include "../ui/litehtml_bridge.h"
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
    const char *html="<!doctype html><html><head><style>"
        "body{margin:0}main{display:flex;gap:16px;background:#abc}"
        "main>div{padding:10px;width:80px;background:#ff0000}"
        "</style></head><body><main><div>Alpha</div><div>Beta</div>"
        "</main></body></html>";
    LunaLiteDocument *d=luna_lite_create(html,&gfx,640,480);
    if(!d) return 1;
    int h=luna_lite_layout(d,640,480);
    luna_lite_draw(d,0,0);
    std::printf("litehtml: height=%d glyphs=%d boxes=%d clips=%d\n",
                h,counts.glyphs,counts.boxes,counts.clips);
    bool ok=h>0 && counts.glyphs>=2 && counts.boxes>=2 && counts.clips==0;
    luna_lite_destroy(d);
    return ok?0:2;
}
