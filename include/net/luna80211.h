#ifndef LUNA_NET_80211_H
#define LUNA_NET_80211_H

/* Native, freestanding IEEE 802.11 frame primitives for LunaOS.
 * This is NOT the Linux mac80211/cfg80211 API or a working radio driver.
 * Buffers are caller-owned; RX frames must have FCS removed by the driver.
 */
#include <stddef.h>
#include <stdint.h>

enum {
    L80211_OK = 0,
    L80211_ENOENT = -2,
    L80211_EINVAL = -22,
    L80211_ENOSPC = -28,
    L80211_ENOTSUP = -95
};

typedef struct {
    uint8_t type, subtype;
    uint8_t to_ds, from_ds, protected_frame, qos;
    size_t header_len;
    uint8_t destination[6], source[6], bssid[6];
} Luna80211Header;

typedef struct {
    uint8_t bssid[6];
    uint8_t ssid[32];
    uint8_t ssid_len;
    uint8_t channel; /* 0 when the frame does not advertise a DS channel. */
    uint16_t capability;
} Luna80211Bss;

/* Accepts management and data frames. Rejects control/unknown formats. */
int luna80211_header(const uint8_t *frame, size_t length, Luna80211Header *out);
/* Accepts beacon and probe-response frames; does not apply regulatory policy. */
int luna80211_bss(const uint8_t *frame, size_t length, Luna80211Bss *out);
/* Decapsulation expects a *decrypted* data MPDU with RFC1042 LLC/SNAP. */
int luna80211_to_ethernet(const uint8_t *frame, size_t length,
                         uint8_t *ethernet, size_t capacity, size_t *written);
/* Constructs an unprotected STA->AP data MPDU; caller must own association,
 * encryption, sequence management, fragmentation and actual transmission. */
int luna80211_from_ethernet(const uint8_t *ethernet, size_t length,
                           const uint8_t bssid[6], uint16_t sequence,
                           uint8_t *frame, size_t capacity, size_t *written);
#endif
