#include "update.h"
#include "install.h"
#include "bearssl.h"
static uint32_t word(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
bool update_digest_valid(const void *data, size_t n, const uint8_t *expected) {
    br_sha256_context hash;
    uint8_t result[32];
    br_sha256_init(&hash); br_sha256_update(&hash, data, n); br_sha256_out(&hash, result);
    return !memcmp(result, expected, sizeof(result));
}
bool update_manifest_valid(const uint8_t *m, size_t n, const char *commit) {
    if (n != UPDATE_MANIFEST_BYTES || strlen(commit) != 40 ||
        memcmp(m, "LUNAUPD1", 8) || word(m + 8) != 1 ||
        word(m + 12) != LUNA_IMAGE_BYTES || word(m + 16) != UPDATE_CHUNK_BYTES ||
        word(m + 20) != UPDATE_RECORDS || memcmp(m + 24, commit, 40)) return false;
    for (unsigned i = 0; i < 40; i++)
        if (!((commit[i] >= '0' && commit[i] <= '9') || (commit[i] >= 'a' && commit[i] <= 'f'))) return false;
    for (unsigned i = 0; i < UPDATE_RECORDS; i++) {
        const uint8_t *r = m + 64 + i * 40;
        unsigned sector = i * (UPDATE_CHUNK_BYTES / 512);
        if (sector >= LUNA_VOLUME_START)
            sector += LUNA_LIVE_SECTORS;
        if (word(r) != sector ||
            word(r + 4) > UPDATE_CHUNK_BYTES + 1024) return false;
    }
    return true;
}
