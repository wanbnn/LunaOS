#ifndef LUNA_WIFI_CCMP_H
#define LUNA_WIFI_CCMP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "wpa2.h"
/* Software WPA2 CCMP-128 (802.11i, non-QoS, single-MPDU only).
 * No TKIP, fragmentation, A-MSDU, aggregate MPDUs or rekeying. Not thread safe.
 * The caller MUST validate BSSID, transmitter and association state.
 * On MIC/replay failure, the decrypted output is NEVER delivered.
 */
typedef struct {
    uint8_t pairwise[16], group[16], group_index;
    uint64_t tx_pn, rx_pairwise_pn[17], rx_group_pn[4][17];
    bool active;
} WifiCcmp;
int wifi_ccmp_install(WifiCcmp *, const Wpa2Keys *);
void wifi_ccmp_clear(WifiCcmp *);
/* Operates on caller-owned MPDU including the 24-byte 802.11 header.
 * encrypt: plaintext data MPDU -> Protected+8-byte CCMP header+MIC.
 * decrypt: Protected CCMP MPDU -> plaintext data MPDU. */
int wifi_ccmp_encrypt(WifiCcmp *, uint8_t *, size_t *, size_t capacity);
int wifi_ccmp_decrypt(WifiCcmp *, uint8_t *, size_t *);
#endif
