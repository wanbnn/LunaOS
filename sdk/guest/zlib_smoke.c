#include "posix.h"
#include "zlib.h"

/* Real upstream zlib compression/decompression entirely in LunaOS ring 3.
 * Guest memory allocator is a temporary monotonic test-only implementation. */
static const unsigned char payload[] =
    "zlib compress and decompress on LunaOS native i386 with Git-compatible "
    "DEFLATE streams, isolated memory pages and no Linux execution relay.";
static unsigned char compressed[2048];
static unsigned char restored[2048];
int main(void) {
    uLongf n = sizeof(compressed);
    if (compress2(compressed, &n, payload, sizeof(payload), Z_BEST_COMPRESSION) != Z_OK)
        return 61;
    if (!n || n >= sizeof(payload))
        return 62;
    uLongf plain = sizeof(restored);
    if (uncompress(restored, &plain, compressed, n) != Z_OK)
        return 63;
    if (plain != sizeof(payload))
        return 64;
    for (uLong i = 0; i < plain; i++)
        if (restored[i] != payload[i])
            return 65;
    static const char marker[] = "LUNA ZLIB V1.4 GUEST OK\n";
    if (write(1, marker, sizeof(marker) - 1u) != (long)(sizeof(marker) - 1u))
        return 66;
    return 0;
}
