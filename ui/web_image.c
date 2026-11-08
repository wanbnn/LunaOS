/* Memory-only PNG decoding for replaced HTML image elements. */
#include "webengine.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS
#define STBI_MAX_DIMENSIONS 2048
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../third_party/stb/stb_image.h"
#pragma GCC diagnostic pop

bool web_decode_image(WebNode *node, const unsigned char *data, size_t len) {
    int width, height, channels;
    if (!node || !data || !len || len > 524288)
        return false;
    if (!stbi_info_from_memory(data, (int)len, &width, &height, &channels) ||
        width <= 0 || height <= 0 || width > 2048 || height > 2048 ||
        (unsigned)width * (unsigned)height > 262144)
        return false;
    unsigned char *pixels = stbi_load_from_memory(data, (int)len, &width, &height, &channels, 4);
    if (!pixels)
        return false;
    web_free(node->image);
    node->image = pixels;
    node->image_w = width;
    node->image_h = height;
    return true;
}

/* Bounded decoder shared with the authenticated updater package format. */
int update_inflate(char *out, int cap, const char *data, int bytes) {
    return stbi_zlib_decode_buffer(out, cap, data, bytes);
}
