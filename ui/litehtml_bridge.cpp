/* litehtml v0.10 / Luna graphics adapter (host and freestanding guest).
 * Source of the renderer: third_party/litehtml, BSD-3-Clause license.
 * No kernel headers or Linux APIs are used in this C++ translation unit.
 */
#include "litehtml_bridge.h"
#include <litehtml.h>
#include <cctype>
#include <cstring>
#include <memory>
#include <string>
#include <algorithm>

using namespace litehtml;
static uint32_t rgb(web_color c) {
    return ((uint32_t)c.red << 16) | ((uint32_t)c.green << 8) | c.blue;
}
static int px(pixel_t value) { return (int)(value + (value >= 0 ? 0.5f : -0.5f)); }
class LunaContainer final : public document_container {
public:
    LunaLiteGraphics gfx{};
    int width = 800;
    int height = 600;
    explicit LunaContainer(const LunaLiteGraphics& g, int w, int h) :
        gfx(g), width(w), height(h) {}
    uint_ptr create_font(const font_description& fd, const document*, font_metrics* fm) override {
        const int size = fd.size > 0 ? fd.size : 16;
        fm->font_size = size;
        fm->height = size + 5;
        fm->ascent = size;
        fm->descent = 5;
        fm->x_height = size / 2;
        fm->ch_width = size / 2;
        fm->draw_spaces = true;
        fm->sub_shift = size / 3;
        fm->super_shift = size / 3;
        return (uint_ptr)size;
    }
    void delete_font(uint_ptr) override {}
    pixel_t text_width(const char* t, uint_ptr font) override {
        if (!t) return 0;
        if (gfx.text_width)
            return (pixel_t)gfx.text_width(gfx.context, t, (int)font);
        return (pixel_t)(std::strlen(t) * ((int)font / 2));
    }
    void draw_text(uint_ptr, const char* t, uint_ptr font, web_color color,
                   const position& pos) override {
        if (gfx.draw_text && t)
            gfx.draw_text(gfx.context, px(pos.x), px(pos.y), t, (int)font, rgb(color));
    }
    pixel_t pt_to_px(float pt) const override { return pt * (96.0f / 72.0f); }
    pixel_t get_default_font_size() const override { return 16; }
    const char* get_default_font_name() const override { return "Luna"; }
    void draw_list_marker(uint_ptr, const list_marker& marker) override {
        (void)marker; /* Marker rasterization is a separate Luna gfx primitive. */
    }
    void load_image(const char*, const char*, bool) override {}
    void get_image_size(const char*, const char*, litehtml::size& sz) override {
        sz.width = sz.height = 0;
    }
    void draw_image(uint_ptr, const background_layer& layer,
                    const std::string& url, const std::string&) override {
        if (gfx.draw_image)
            gfx.draw_image(gfx.context, px(layer.border_box.x), px(layer.border_box.y),
                           px(layer.border_box.width), px(layer.border_box.height), url.c_str());
    }
    void draw_solid_fill(uint_ptr, const background_layer& layer,
                         const web_color& color) override {
        if (gfx.fill_rect && color.alpha)
            gfx.fill_rect(gfx.context, px(layer.clip_box.x), px(layer.clip_box.y),
                          px(layer.clip_box.width), px(layer.clip_box.height), rgb(color));
    }
    void draw_linear_gradient(uint_ptr, const background_layer&,
                              const background_layer::linear_gradient&) override {}
    void draw_radial_gradient(uint_ptr, const background_layer&,
                              const background_layer::radial_gradient&) override {}
    void draw_conic_gradient(uint_ptr, const background_layer&,
                             const background_layer::conic_gradient&) override {}
    void draw_borders(uint_ptr, const borders& b, const position& p, bool) override {
        if (!gfx.fill_rect) return;
        auto rect = [&](float x, float y, float w, float h, web_color c) {
            if (w > 0 && h > 0 && c.alpha)
                gfx.fill_rect(gfx.context, px(x), px(y), px(w), px(h), rgb(c));
        };
        rect(p.x, p.y, p.width, b.top.width, b.top.color);
        rect(p.x, p.bottom() - b.bottom.width, p.width, b.bottom.width, b.bottom.color);
        rect(p.x, p.y, b.left.width, p.height, b.left.color);
        rect(p.right() - b.right.width, p.y, b.right.width, p.height, b.right.color);
    }
    void set_caption(const char*) override {}
    void set_base_url(const char*) override {}
    void link(const std::shared_ptr<document>&, const element::ptr&) override {}
    void on_anchor_click(const char*, const element::ptr&) override {}
    void on_mouse_event(const element::ptr&, mouse_event) override {}
    void set_cursor(const char*) override {}
    void transform_text(string& s, text_transform tt) override {
        if (tt == text_transform_uppercase || tt == text_transform_lowercase) {
            for (char& c : s) {
                unsigned char ch = (unsigned char)c;
                if (ch < 128)
                    c = (char)(tt == text_transform_uppercase ?
                               std::toupper(ch) : std::tolower(ch));
            }
        }
    }
    void import_css(string& css, const string&, string& baseurl) override {
        css.clear(); baseurl.clear(); /* External CSS resolved by Luna Browser. */
    }
    void set_clip(const position& p, const border_radiuses&) override {
        if (gfx.push_clip)
            gfx.push_clip(gfx.context, px(p.x), px(p.y), px(p.width), px(p.height));
    }
    void del_clip() override { if (gfx.pop_clip) gfx.pop_clip(gfx.context); }
    void get_viewport(position& p) const override {
        p = position(0, 0, (pixel_t)width, (pixel_t)height);
    }
    element::ptr create_element(const char*, const string_map&,
                                const std::shared_ptr<document>&) override {
        return nullptr;
    }
    void get_media_features(media_features& out) const override {
        out = media_features{};
        out.type = media_type_screen;
        out.width = out.device_width = (pixel_t)width;
        out.height = out.device_height = (pixel_t)height;
        out.color = 8;
        out.resolution = 96;
    }
    void get_language(string& language, string& culture) const override {
        language = "pt"; culture = "pt-BR";
    }
};

struct LunaLiteDocument {
    LunaContainer container;
    document::ptr doc;
    LunaLiteDocument(const LunaLiteGraphics& g, int w, int h) :
        container(g, w, h) {}
};
extern "C" LunaLiteDocument *luna_lite_create(const char *html,
                                               const LunaLiteGraphics *graphics,
                                               int width, int height) {
    if (!graphics || !html || width <= 0 || height <= 0) return nullptr;
    auto *out = new LunaLiteDocument(*graphics, width, height);
    out->doc = document::createFromString(estring(html), &out->container);
    if (!out->doc) { delete out; return nullptr; }
    out->doc->render((pixel_t)width);
    return out;
}
extern "C" void luna_lite_destroy(LunaLiteDocument *d) {
    delete d;
}
extern "C" int luna_lite_layout(LunaLiteDocument *d, int width, int height) {
    if (!d || width <= 0 || height <= 0) return -1;
    d->container.width = width;
    d->container.height = height;
    d->doc->render((pixel_t)width);
    return (int)d->doc->height();
}
extern "C" void luna_lite_draw(LunaLiteDocument *d, int x, int y) {
    if (!d) return;
    position clip(0, 0, (pixel_t)d->container.width, (pixel_t)d->container.height);
    d->doc->draw(0, (pixel_t)x, (pixel_t)y, &clip);
}
extern "C" void luna_lite_add_css(LunaLiteDocument *d, const char *css) {
    if (!d || !d->doc || !css || !*css) return;
    d->doc->add_stylesheet(css, "", "screen");
    d->doc->render((pixel_t)d->container.width);
}
extern "C" int luna_lite_document_height(const LunaLiteDocument *d) {
    return d && d->doc ? (int)d->doc->height() : 0;
}
