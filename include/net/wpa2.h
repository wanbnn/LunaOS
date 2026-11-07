#ifndef LUNA_WPA2_H
#define LUNA_WPA2_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* RSN WPA2-PSK/CCMP only. EAPOL input/output excludes Ethernet header.
 * Caller must have completed open-system authentication + association and
 * verified the AP RSN IE. Callbacks are synchronous, never retain pointers.
 * The backend owns CCMP PN/replay enforcement and must clear keys on reset.
 * No TKIP, enterprise, WPA3, PMF, pairwise/group rekey or software CCMP. */
#define WPA2_EAPOL_MAX 1024
#define WPA2_RSN_MAX 96
typedef struct {
    uint8_t pairwise[16], group[16], group_index, group_rsc[6];
} Wpa2Keys;
typedef struct {
    void *context;
    int (*random)(void *, uint8_t *, size_t); /* Cryptographic RNG or fail. */
    int (*send)(void *, const uint8_t *, size_t);
    int (*install)(void *, const Wpa2Keys *); /* Atomic, no PN resets on retry. */
    void (*clear)(void *);
} Wpa2Ops;
typedef enum { WPA2_OFF, WPA2_WAIT_M1, WPA2_WAIT_M3, WPA2_AUTHORIZED, WPA2_FAILED } Wpa2Phase;
typedef struct {
    Wpa2Ops ops;
    Wpa2Phase phase;
    uint8_t pmk[32], ptk[48], station[6], bssid[6], snonce[32], anonce[32];
    uint8_t replay[8], m3_replay[8], m3_digest[20];
    uint8_t ap_rsn[WPA2_RSN_MAX], station_rsn[WPA2_RSN_MAX];
    unsigned ap_rsn_len, station_rsn_len;
    uint32_t deadline;
    int error;
} Wpa2Supplicant;
int wpa2_pmk(const char *passphrase, const uint8_t *ssid, size_t ssid_len, uint8_t pmk[32]);
int wpa2_ptk(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6],
             const uint8_t anonce[32], const uint8_t snonce[32], uint8_t ptk[48]);
int wpa2_key_mic(const uint8_t kck[16], const uint8_t *eapol, size_t len, uint8_t mic[16]);
int wpa2_key_unwrap(const uint8_t kek[16], const uint8_t *wrapped, size_t len,
                    uint8_t *plain, size_t capacity, size_t *written);
/* Exact AP RSN IE is retained to prevent RSN downgrade in message 3.
 * The station IE sent in message 2 selects only PSK + CCMP. */
int wpa2_start(Wpa2Supplicant *, const Wpa2Ops *, const uint8_t station[6],
                const uint8_t bssid[6], const uint8_t *ssid, size_t ssid_len,
                const char *passphrase, const uint8_t *ap_rsn, size_t ap_rsn_len,
                uint32_t now_ms);
int wpa2_receive(Wpa2Supplicant *, const uint8_t source[6], const uint8_t *, size_t,
                  uint32_t now_ms);
void wpa2_poll(Wpa2Supplicant *, uint32_t now_ms);
void wpa2_stop(Wpa2Supplicant *);
#endif
