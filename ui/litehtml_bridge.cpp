/* litehtml v0.10 / Luna graphics adapter (host and freestanding guest).
 * Source of the renderer: third_party/litehtml, BSD-3-Clause license.
 * No kernel headers or Linux APIs are used in this C++ translation unit.
 */
#include "litehtml_bridge.h"
#include "web_dom_types.h"
#include <litehtml.h>
#include <litehtml/el_space.h>
#include <litehtml/el_text.h>
#include <litehtml/html_tag.h>
#include <litehtml/render_item.h>
#include <cctype>
#include <cstring>
#include <memory>
#include <map>
#include <new>
#include <string>
#include <algorithm>

using namespace litehtml;
static uint32_t rgb(web_color c) {
    return ((uint32_t)c.red << 16) | ((uint32_t)c.green << 8) | c.blue;
}
static int px(pixel_t value) { return (int)(value + (value >= 0 ? 0.5f : -0.5f)); }

#ifdef LUNA_KERNEL_LITEHTML
extern "C" uint32_t luna_cxx_render_begin();
extern "C" void luna_cxx_render_end(uint32_t previous);
#endif
class RenderBudget {
#ifdef LUNA_KERNEL_LITEHTML
    uint32_t previous = luna_cxx_render_begin();
public:
    ~RenderBudget() { luna_cxx_render_end(previous); }
#endif
};

/* litehtml v0.10 initializes its layout through createFromString. Its
 * container hook supplies our root and discards the empty seed's children. */
class LunaRoot final : public html_tag {
public:
    using html_tag::html_tag;
    bool appendChild(const element::ptr&) override { return false; }
};

class LunaContainer final : public document_container {
public:
    LunaLiteGraphics gfx{};
    int width = 800;
    int height = 600;
    WebNode *source_root;
    std::shared_ptr<LunaRoot> seed_root;
    std::map<const element*, WebNode*> source_nodes;
    explicit LunaContainer(const LunaLiteGraphics& g, int w, int h, WebNode *root) :
        gfx(g), width(w), height(h), source_root(root) {}
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
    void get_image_size(const char* url, const char*, litehtml::size& sz) override {
        int w = 0, h = 0;
        if (gfx.image_size)
            gfx.image_size(gfx.context, url, &w, &h);
        sz.width = w;
        sz.height = h;
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
    void link(const std::shared_ptr<document>& doc, const element::ptr& link) override {
        const char *href = link->get_attr("href");
        const char *rel = link->get_attr("rel");
        if (!href || !rel || std::strcmp(rel, "stylesheet") || !gfx.stylesheet)
            return;
        const char *css = gfx.stylesheet(gfx.context, href);
        if (!css)
            return;

        // litehtml tokenizes an entire stylesheet at once. Feed batches
        // of complete top-level rules to keep its temporary allocations small.
        constexpr size_t batch_size = 16 * 1024;
        const char *batch = css;
        const char *rule = css;
        const char *first_token = nullptr;
        unsigned depth = 0;
        char quote = 0;
        auto emit = [&](const char *end) {
            if (end != batch) {
                string text(batch, end);
                doc->add_stylesheet(text.c_str(), href, link->get_attr("media"));
            }
        };
        for (; *css; ++css) {
            if (!quote && css[0] == '/' && css[1] == '*') {
                css += 2;
                while (*css && !(css[0] == '*' && css[1] == '/'))
                    ++css;
                if (!*css)
                    break;
                ++css;
                continue;
            }
            if (!first_token && !std::isspace(static_cast<unsigned char>(*css)))
                first_token = css;
            if (*css == '\\' && css[1]) {
                ++css;
                continue;
            }
            if (quote) {
                if (*css == quote)
                    quote = 0;
                continue;
            }
            if (*css == '\'' || *css == '"') {
                quote = *css;
                continue;
            }
            if (*css == '(' || *css == '[' || *css == '{') {
                ++depth;
                continue;
            }
            if (depth && (*css == ')' || *css == ']' || *css == '}'))
                --depth;
            if (depth || (*css != '}' && *css != ';'))
                continue;
            const char *end = css + 1;
            // Luna's font callback uses built-in fonts. Avoid passing embedded
            // font payloads through a tokenizer that cannot use them.
            if (first_token && !std::strncmp(first_token, "@font-face", 10)) {
                emit(rule);
                batch = end;
            } else if (static_cast<size_t>(end - batch) > batch_size && rule != batch) {
                emit(rule);
                batch = rule;
            }
            rule = end;
            first_token = nullptr;
        }
        emit(css);
    }
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
        css.clear();
        baseurl.clear();
    }
    void set_clip(const position& p, const border_radiuses&) override {
        if (gfx.push_clip)
            gfx.push_clip(gfx.context, px(p.x), px(p.y), px(p.width), px(p.height));
    }
    void del_clip() override { if (gfx.pop_clip) gfx.pop_clip(gfx.context); }
    void get_viewport(position& p) const override {
        p = position(0, 0, (pixel_t)width, (pixel_t)height);
    }
    void import_children(const WebNode *node, const element::ptr& parent,
                         const document::ptr& doc, bool root = false) {
        auto append = [&](const element::ptr& child) {
            if (root)
                return static_cast<LunaRoot&>(*parent).html_tag::appendChild(child);
            return parent->appendChild(child);
        };
        for (WebNode *child = node->first_child; child; child = child->next) {
            if (child->type == WEB_ELEMENT && child->tag) {
                string_map attributes;
                for (const WebAttr *attr = child->attrs; attr; attr = attr->next)
                    attributes[attr->name] = attr->value;
                auto element = doc->create_element(child->tag, attributes);
                if (append(element)) {
                    source_nodes.emplace(element.get(), child);
                    if (element->tag() == _input_) {
                        string type = lowcase(element->get_attr("type", "text"));
                        const char *label = element->get_attr("value", "");
                        if (!*label)
                            label = element->get_attr("placeholder", "");
                        if (*label && type != "hidden" && type != "password")
                            element->appendChild(std::make_shared<el_text>(label, doc));
                    }
                    import_children(child, element, doc);
                }
            } else if (child->type == WEB_TEXT && child->text) {
                if (parent->tag() == _style_ || parent->tag() == _script_) {
                    append(std::make_shared<el_text>(child->text, doc));
                } else {
                    bool last_space = false;
                    const bool collapse = node->style.white_space == 0 || node->style.white_space == 3;
                    split_text(child->text,
                               [&](const char *s) {
                                   last_space = false;
                                   append(std::make_shared<el_text>(s, doc));
                               },
                               [&](const char *s) {
                                   if (!collapse || !last_space)
                                       append(std::make_shared<el_space>(collapse ? " " : s, doc));
                                   last_space = true;
                               });
                }
            }
        }
    }
    element::ptr create_element(const char* tag, const string_map&,
                                const document::ptr& doc) override {
        if (!source_root || seed_root || std::strcmp(tag, "html"))
            return nullptr;
        seed_root = std::make_shared<LunaRoot>(doc);
        return seed_root;
    }
    void get_media_features(media_features& out) const override {
        // litehtml frees the Gumbo seed before its first media query.
        if (seed_root)
            const_cast<LunaContainer *>(this)->import_document();
        out = media_features{};
        out.type = media_type_screen;
        out.width = out.device_width = (pixel_t)width;
        out.height = out.device_height = (pixel_t)height;
        out.color = 8;
        out.resolution = 96;
    }
    void import_document() {
        auto root = std::move(seed_root);
        const WebNode *source = source_root;
        source_root = nullptr;
        for (const WebAttr *attr = source->attrs; attr; attr = attr->next)
            root->set_attr(attr->name, attr->value);
        import_children(source, root, root->get_document(), true);
    }
    void get_language(string& language, string& culture) const override {
        language = "pt"; culture = "pt-BR";
    }
};

struct LunaLiteDocument {
    LunaContainer container;
    document::ptr doc;
    LunaLiteDocument(const LunaLiteGraphics& g, int w, int h, WebNode *root) :
        container(g, w, h, root) {}
};
extern "C" LunaLiteDocument *luna_lite_create(WebNode *root,
                                               const LunaLiteGraphics *graphics,
                                               int width, int height) {
    if (!graphics || !root || width <= 0 || height <= 0) return nullptr;
    WebNode *html = root->first_child;
    while (html && (!html->tag || std::strcmp(html->tag, "html")))
        html = html->next;
    [[maybe_unused]] RenderBudget budget;
    try {
        auto out = std::make_unique<LunaLiteDocument>(*graphics, width, height, html ? html : root);
        out->doc = document::createFromString(estring("<!doctype html>", encoding::utf_8),
                                               &out->container);
        out->doc->render((pixel_t)width);
        return out.release();
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}
extern "C" void luna_lite_destroy(LunaLiteDocument *d) {
    delete d;
}
extern "C" int luna_lite_layout(LunaLiteDocument *d, int width, int height) {
    if (!d || width <= 0 || height <= 0) return -1;
    d->container.width = width;
    d->container.height = height;
    [[maybe_unused]] RenderBudget budget;
    try {
        d->doc->media_changed();
        d->doc->render((pixel_t)width);
        return (int)d->doc->height();
    } catch (const std::bad_alloc&) {
        return -1;
    }
}
extern "C" bool luna_lite_draw(LunaLiteDocument *d, int x, int y) {
    if (!d)
        return false;
    position clip(0, 0, (pixel_t)d->container.width, (pixel_t)d->container.height);
    [[maybe_unused]] RenderBudget budget;
    try {
        d->doc->draw(0, (pixel_t)x, (pixel_t)y, &clip);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}
extern "C" WebNode *luna_lite_hit_link(LunaLiteDocument *d, int x, int y, int scroll) {
    auto render = d->doc->root_render();
    if (!render)
        return nullptr;
    [[maybe_unused]] RenderBudget budget;
    try {
        auto hit = render->get_element_by_point(x, y, x, y - scroll, nullptr);
        for (; hit; hit = hit->parent()) {
            if (hit->tag() == _a_ && hit->get_attr("href")) {
                auto source = d->container.source_nodes.find(hit.get());
                if (source != d->container.source_nodes.end())
                    return source->second;
            }
        }
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
    return nullptr;
}
extern "C" int luna_lite_document_height(const LunaLiteDocument *d) {
    return d && d->doc ? (int)d->doc->height() : 0;
}
