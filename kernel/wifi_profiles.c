#include "luna.h"
#include "wifi_profiles.h"
#include "../include/net/wpa2.h"

#define PROFILE_PATH "/etc/luna-wifi-v1"
#define MAX_PROFILES 16
#define RETRY_MS 5000u
#define MANUAL_SCAN_MS 30000u
#define SAVE_RETRY_MS 30000u

/* Fixed-size disk records, newest successful network first. The PSK is
 * specific to this SSID and grants network access, so never log it. */
typedef struct {
    uint8_t security;
    char ssid[WIFI_MAX_SSID + 1];
    char psk[WIFI_PSK_HEX_LEN + 1];
} WifiProfile;
static WifiProfile profiles[MAX_PROFILES], pending;
static unsigned profile_count, next_profile;
static uint32_t next_scan, next_save;
static bool auto_scanning, dirty, save_failed;

static void wipe(void *data, size_t bytes) {
    volatile uint8_t *p = data;
    while (bytes--) *p++ = 0;
}
static bool profile_matches(const WifiProfile *profile, const WifiAp *ap) {
    return profile->security == ap->security && !strcmp(profile->ssid, ap->ssid);
}
static const WifiProfile *find_profile(const WifiAp *ap) {
    for (unsigned i = 0; i < profile_count; i++)
        if (profile_matches(&profiles[i], ap)) return &profiles[i];
    return 0;
}
bool wifi_profile_known(const WifiAp *ap) {
    return find_profile(ap) != 0;
}
void wifi_profiles_init(void) {
    next_scan = frame_clock;
    File *file = fs_find(PROFILE_PATH);
    if (!file || file->dir || file->size > sizeof(profiles) ||
        file->size % sizeof(WifiProfile) ||
        !fs_read_bytes(file, 0, profiles, file->size)) return;
    for (unsigned i = 0; i < file->size / sizeof(WifiProfile); i++) {
        WifiProfile *p = &profiles[i];
        if (!p->ssid[0] || p->ssid[WIFI_MAX_SSID] ||
            p->psk[WIFI_PSK_HEX_LEN]) continue;
        if (p->security == WIFI_OPEN) {
            if (p->psk[0]) continue;
        } else if (p->security == WIFI_WPA2_PSK) {
            uint8_t key[32];
            if (strlen(p->psk) != WIFI_PSK_HEX_LEN) continue;
            int rc = wpa2_pmk(p->psk, (const uint8_t *)p->ssid, strlen(p->ssid), key);
            wipe(key, sizeof(key));
            if (rc) continue;
        } else continue;
        profiles[profile_count++] = *p;
    }
    wipe(profiles + profile_count, (MAX_PROFILES - profile_count) * sizeof(WifiProfile));
}
bool wifi_profiles_write(void) {
    return !dirty || !fs_write_bytes(PROFILE_PATH, profiles,
                                    profile_count * sizeof(WifiProfile), false);
}
int wifi_user_scan(void) {
    /* Let an in-flight automatic scan finish for the UI. Keep its results
     * available while the user selects a network and enters the password. */
    int rc = auto_scanning ? 0 : wifi_scan_start();
    auto_scanning = false;
    next_scan = frame_clock + MANUAL_SCAN_MS;
    return rc;
}
int wifi_user_connect(const WifiAp *ap, const char *password) {
    if (!ap || !ap->ssid_len || ap->ssid_len > WIFI_MAX_SSID || ap->ssid[ap->ssid_len])
        return -22;
    WifiProfile profile = {.security = ap->security};
    memcpy(profile.ssid, ap->ssid, ap->ssid_len);
    if (ap->security == WIFI_WPA2_PSK) {
        const WifiProfile *saved = find_profile(ap);
        if (saved && (!password || !*password)) {
            profile = *saved;
        } else {
            uint8_t key[32];
            int rc = wpa2_pmk(password, (const uint8_t *)ap->ssid, ap->ssid_len, key);
            if (!rc) {
                static const char hex[] = "0123456789abcdef";
                for (unsigned i = 0; i < sizeof(key); i++) {
                    profile.psk[2*i] = hex[key[i] >> 4];
                    profile.psk[2*i+1] = hex[key[i] & 15];
                }
            }
            wipe(key, sizeof(key));
            if (rc) return rc;
        }
    }
    int rc = wifi_connect_ap(ap, profile.psk);
    if (!rc) {
        pending = profile;
        next_profile = 0;
        next_scan = frame_clock + RETRY_MS;
    }
    wipe(&profile, sizeof(profile));
    return rc;
}
static void remember_connection(uint32_t now) {
    unsigned i = 0;
    while (i < profile_count && (profiles[i].security != pending.security ||
           strcmp(profiles[i].ssid, pending.ssid))) i++;
    if (i || !profile_count || strcmp(profiles[0].psk, pending.psk)) {
        if (i == profile_count && profile_count < MAX_PROFILES) profile_count++;
        if (i == MAX_PROFILES) i--;
        memmove(profiles + 1, profiles, i * sizeof(WifiProfile));
        profiles[0] = pending;
        if (!dirty) next_save = now;
        dirty = true;
    }
    wipe(&pending, sizeof(pending));
}
static void join_known_network(void) {
    WifiAp aps[WIFI_MAX_APS];
    unsigned count;
    if (wifi_scan_results(aps, WIFI_MAX_APS, &count)) return;
    for (unsigned offset = 0; offset < profile_count; offset++) {
        unsigned index = (next_profile + offset) % profile_count;
        const WifiProfile *profile = &profiles[index];
        const WifiAp *best = 0;
        for (unsigned i = 0; i < count; i++)
            if (profile_matches(profile, &aps[i]) && (!best || aps[i].rssi_dbm > best->rssi_dbm))
                best = &aps[i];
        if (best && !wifi_connect_ap(best, profile->psk)) {
            pending = *profile;
            next_profile = (index + 1) % profile_count;
            return;
        }
    }
}
void wifi_profiles_poll(bool save_to_boot_disk) {
    uint32_t now = frame_clock;
    if (dirty && save_to_boot_disk && (int32_t)(now - next_save) >= 0) {
        if (wifi_profiles_write() && fs_save()) {
            serial_write("Wi-Fi: profiles saved\n");
            dirty = save_failed = false;
        } else {
            if (!save_failed) serial_write("Wi-Fi: could not save profiles to boot disk\n");
            save_failed = true;
            next_save = frame_clock + SAVE_RETRY_MS;
        }
        now = frame_clock;
    }
    if (!pending.ssid[0] && !profile_count) return;
    WifiState state;
    if (wifi_state(&state) || !state.can_connect) {
        auto_scanning = false;
        return;
    }
    if (state.link == WIFI_ONLINE) {
        if (pending.ssid[0]) remember_connection(now);
        next_profile = 0;
        next_scan = now + RETRY_MS;
        return;
    }
    if (state.link >= WIFI_ASSOCIATING || state.link == WIFI_SCANNING) return;
    wipe(&pending, sizeof(pending));
    if (auto_scanning) {
        auto_scanning = false;
        next_scan = now + RETRY_MS;
        join_known_network();
    } else if (profile_count && (int32_t)(now - next_scan) >= 0) {
        auto_scanning = !wifi_scan_start();
        next_scan = now + RETRY_MS;
    }
}
