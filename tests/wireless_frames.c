/* Run on the build host: gcc -std=c11 -Wall -Wextra -Werror ... */
#include <assert.h>
#include <string.h>
#include "../include/net/luna80211.h"

static void test_beacon(void) {
    uint8_t beacon[64] = {0};
    beacon[0] = 0x80; /* beacon subtype */
    uint8_t ap[6] = {1, 2, 3, 4, 5, 6};
    memcpy(beacon + 16, ap, 6);
    beacon[34] = 0x11;
    beacon[35] = 0x04;
    size_t p = 36;
    beacon[p++] = 0; beacon[p++] = 4;
    memcpy(beacon + p, "test", 4); p += 4;
    beacon[p++] = 3; beacon[p++] = 1; beacon[p++] = 11;
    Luna80211Bss b;
    assert(luna80211_bss(beacon, p, &b) == L80211_OK);
    assert(b.ssid_len == 4 && memcmp(b.ssid, "test", 4) == 0);
    assert(b.channel == 11 && b.capability == 0x0411);
    assert(memcmp(b.bssid, ap, 6) == 0);
    assert(luna80211_bss(beacon, p - 1, &b) == L80211_EINVAL);
    beacon[p++] = 0; beacon[p++] = 0; /* duplicate SSID */
    assert(luna80211_bss(beacon, p, &b) == L80211_EINVAL);
}

static void test_data(void) {
    const uint8_t ethernet[] = {
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25,
        0x08, 0x00, 0xde, 0xad, 0xbe, 0xef
    };
    const uint8_t ap[6] = {0x30, 0x31, 0x32, 0x33, 0x34, 0x35};
    uint8_t raw[100] = {0}, got[100] = {0};
    size_t n = 0, neth = 0;
    assert(luna80211_from_ethernet(ethernet, sizeof(ethernet), ap, 0x123,
                                   raw, sizeof(raw), &n) == L80211_OK);
    assert(n == 36 && raw[22] == 0x30 && raw[23] == 0x12);
    Luna80211Header h;
    assert(luna80211_header(raw, n, &h) == L80211_OK);
    assert(h.type == 2 && h.to_ds == 1 && h.from_ds == 0 && h.header_len == 24);
    assert(luna80211_to_ethernet(raw, n, got, sizeof(got), &neth) == L80211_OK);
    assert(neth == sizeof(ethernet) && memcmp(got, ethernet, neth) == 0);
    assert(luna80211_to_ethernet(raw, n, got, 15, &neth) == L80211_ENOSPC);
    raw[1] |= 0x40; /* protected traffic must be decrypted before decapsulation */
    assert(luna80211_to_ethernet(raw, n, got, sizeof(got), &neth) == L80211_ENOTSUP);
    raw[1] &= (uint8_t)~0x40;
    raw[0] = 0x88; /* QoS data: insert 2-byte QoS control */
    memmove(raw + 26, raw + 24, n - 24);
    raw[24] = raw[25] = 0;
    n += 2;
    assert(luna80211_to_ethernet(raw, n, got, sizeof(got), &neth) == L80211_OK);
    assert(neth == sizeof(ethernet) && memcmp(got, ethernet, neth) == 0);
    raw[1] = 0x03; /* WDS: address4 between original header and QoS */
    memmove(raw + 30, raw + 24, n - 24);
    memcpy(raw + 24, ethernet + 6, 6);
    memcpy(raw + 16, ethernet, 6);
    n += 6;
    assert(luna80211_to_ethernet(raw, n, got, sizeof(got), &neth) == L80211_OK);
    assert(neth == sizeof(ethernet) && memcmp(got, ethernet, neth) == 0);
    assert(luna80211_header(raw, 20, &h) == L80211_EINVAL);
    assert(luna80211_from_ethernet(ethernet, sizeof(ethernet), ap, 4096,
                                   raw, sizeof(raw), &n) == L80211_EINVAL);
}

int main(void) {
    test_beacon();
    test_data();
    return 0;
}
