/* Host test: the width of a float container has to fit the content and the
 * float that share its line.
 *
 * litehtml takes the larger of the two, so the float lands on top of the
 * content. Luna carries a patch for it (tools/litehtml-float-width.patch).
 * Run with the patched renderer using make test-litehtml.
 */
#include "../ui/litehtml_bridge.h"
#include "../ui/web_dom_types.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

struct Rect { int x, y, w, h; uint32_t color; };
struct Draws { std::vector<Rect> rects; int donate_y = -1; };

static int text_width(void *, const char *s, int font) { return (int)std::strlen(s) * font / 2; }
static void draw_text(void *ctx, int, int y, const char *text, int, uint32_t) {
    if (!std::strcmp(text, "DONATE"))
        static_cast<Draws *>(ctx)->donate_y = y;
}
static void fill_rect(void *ctx, int x, int y, int w, int h, uint32_t c) {
    auto *d = (Draws *)ctx;
    if (w > 0 && h > 0) d->rects.push_back({x, y, w, h, c});
}
static void clip(void *, int, int, int, int) {}
static void unclip(void *) {}

static std::deque<WebNode> pool;
static std::deque<WebAttr> attrs;

static WebNode *element(const char *tag, const char *style, WebNode *child = nullptr) {
    pool.emplace_back();
    WebNode &node = pool.back();
    node.type = WEB_ELEMENT;
    node.tag = const_cast<char *>(tag);
    node.first_child = child;
    if (style) {
        attrs.emplace_back(WebAttr{nullptr, const_cast<char *>("style"), const_cast<char *>(style)});
        node.attrs = &attrs.back();
    }
    return &node;
}

static WebNode *text_node(const char *value) {
    pool.emplace_back();
    WebNode &node = pool.back();
    node.type = WEB_TEXT;
    node.text = const_cast<char *>(value);
    return &node;
}

static void render(WebNode *root, Draws *draws, int width = 400) {
    LunaLiteGraphics gfx{};
    gfx.context = draws;
    gfx.text_width = text_width;
    gfx.draw_text = draw_text;
    gfx.fill_rect = fill_rect;
    gfx.push_clip = clip;
    gfx.pop_clip = unclip;
    LunaLiteDocument *d = luna_lite_create(root, &gfx, width, 300);
    if (!d) {
        std::printf("FAIL: the document could not be created\n");
        std::exit(1);
    }
    luna_lite_layout(d, width, 300);
    luna_lite_draw(d, 0, 0);
    luna_lite_destroy(d);
}

static const Rect *find(const Draws &draws, uint32_t color) {
    for (const auto &r : draws.rects)
        if (r.color == color)
            return &r;
    return nullptr;
}

int main() {
    /* A wide float inside a shrink-to-fit container, with a short piece of
     * content before it. The container has to be as wide as both, so the
     * float sits beside the content and not on top of it. */
    Draws draws;
    WebNode *wide = element("div", "float:right;width:200px;height:36px;background:#222", text_node("BAR"));
    WebNode *label = element("a", "display:inline;background:#ffd343;color:#333", text_node("DONATE"));
    label->next = wide;
    WebNode *holder = element("div", "float:right;background:#333", label);
    WebNode *body = element("body", "margin:0", holder);
    WebNode *html = element("html", "margin:0", body);
    WebNode *root = element("#document", "margin:0", html);
    render(root, &draws);

    const Rect *container = find(draws, 0x333333);
    const Rect *floating = find(draws, 0x222222);
    const Rect *content = find(draws, 0xffd343);
    if (!container || !floating || !content) {
        std::printf("FAIL: a box is missing\n");
        return 1;
    }
    std::printf("container x=%d w=%d, float x=%d w=%d, content x=%d w=%d\n",
                container->x, container->w, floating->x, floating->w,
                content->x, content->w);
    /* The container holds the content and the float side by side. */
    if (container->w != content->w + floating->w) {
        std::printf("FAIL: the container is %d wide, the content and the float ask for %d\n",
                    container->w, content->w + floating->w);
        return 1;
    }
    if (floating->x < content->x + content->w) {
        std::printf("FAIL: the float covers the content\n");
        return 1;
    }
    std::printf("PASS: the float container fits the content and the float\n");
    label->attrs->value = const_cast<char *>(
        "display:inline;position:relative;top:38px;background:#ffd343;color:#333");
    Draws shifted;
    render(root, &shifted);
    const Rect *shifted_content = find(shifted, 0xffd343);
    if (!shifted_content || draws.donate_y < 0 || shifted.donate_y - draws.donate_y != 38 ||
        shifted_content->y - content->y != 38) {
        std::puts("FAIL: a relative inline must move its text and background once");
        return 1;
    }
    std::puts("PASS: relative inline text and background move together after relayout");
    return 0;
}
