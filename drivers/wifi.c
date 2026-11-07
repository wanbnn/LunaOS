#include "wifi.h"
/* No fake APs. The native kernel driver must provide all operations and must
 * have initialized its radio before registering. Passive scanner backends
 * do not provide association. */
#define WIFI_EBUSY (-16)
#define WIFI_EINVAL (-22)
#define WIFI_ENOTSUP (-95)
static const WifiDriverOps *backend;
static void *backend_context;
unsigned wifi_channel_frequency(uint8_t channel) {
    if (channel >= 1 && channel <= 13) return 2407 + 5 * channel;
    if (channel == 14) return 2484;
    if ((channel >= 36 && channel <= 64 && !(channel % 4)) ||
        (channel >= 100 && channel <= 144 && !(channel % 4)) ||
        (channel >= 149 && channel <= 177 && channel % 4 == 1))
        return 5000 + 5 * channel;
    return 0;
}
WifiBand wifi_channel_band(uint8_t channel) {
    unsigned mhz = wifi_channel_frequency(channel);
    return !mhz ? WIFI_BAND_UNKNOWN : mhz < 3000 ? WIFI_BAND_2GHZ : WIFI_BAND_5GHZ;
}
static int valid_ap(const WifiAp *ap) {
    if (!ap || !ap->ssid_len || ap->ssid_len > WIFI_MAX_SSID ||
        ap->ssid[ap->ssid_len]) return WIFI_EINVAL;
    for (unsigned i = 0; i < ap->ssid_len; i++)
        if (!ap->ssid[i]) return WIFI_EINVAL;
    if (ap->security != WIFI_OPEN && ap->security != WIFI_WPA2_PSK &&
        ap->security != WIFI_OTHER) return WIFI_EINVAL;
    if (!wifi_channel_frequency(ap->channel) || (ap->bssid[0] & 1))
        return WIFI_EINVAL;
    bool nonzero = false;
    for (unsigned i = 0; i < 6; i++) if (ap->bssid[i]) nonzero = true;
    return nonzero ? 0 : WIFI_EINVAL;
}
int wifi_register_driver(const WifiDriverOps *ops, void *context) {
    if (!ops || !ops->scan_start || !ops->scan_results || !ops->connect ||
        !ops->state || !ops->poll) return WIFI_EINVAL;
    if (backend) return WIFI_EBUSY;
    WifiState s = {0};
    int rc = ops->state(context, &s);
    if (rc) return rc;
    if (!s.radio_ready) return WIFI_ENOTSUP;
    backend_context = context;
    backend = ops;
    return 0;
}
void wifi_unregister_driver(const WifiDriverOps *ops) {
    if (backend == ops) { backend = 0; backend_context = 0; }
}
int wifi_state(WifiState *out) {
    if (!out) return WIFI_EINVAL;
    *out = (WifiState){0};
    if (!backend) return WIFI_ENOTSUP;
    int rc = backend->state(backend_context, out);
    if (rc || !out->radio_ready) {
        *out = (WifiState){0};
        return rc ? rc : WIFI_ENOTSUP;
    }
    return 0;
}
int wifi_scan_start(void) {
    WifiState s;
    int rc = wifi_state(&s);
    if (rc) return rc;
    return backend->scan_start(backend_context);
}
int wifi_scan_results(WifiAp *out, unsigned cap, unsigned *count) {
    if (!count || (cap && !out)) return WIFI_EINVAL;
    *count = 0;
    WifiState s;
    int rc = wifi_state(&s);
    if (rc) return rc;
    rc = backend->scan_results(backend_context, out, cap, count);
    if (rc) { *count = 0; return rc; }
    if (*count > cap) { *count = 0; return WIFI_EINVAL; }
    for (unsigned i = 0; i < *count; i++) {
        if (valid_ap(out + i)) { *count = 0; return WIFI_EINVAL; }
    }
    return 0;
}
int wifi_connect_ap(const WifiAp *ap, const char *password) {
    int rc = valid_ap(ap);
    if (rc) return rc;
    if (ap->security == WIFI_OTHER) return WIFI_ENOTSUP;
    if (ap->security == WIFI_WPA2_PSK) {
        if (!password) return WIFI_EINVAL;
        size_t n = 0;
        while (password[n] && n <= WIFI_MAX_PASSWORD) n++;
        if (n < 8 || n > WIFI_MAX_PASSWORD) return WIFI_EINVAL;
    }
    WifiState s;
    rc = wifi_state(&s);
    if (rc) return rc;
    if (!s.can_connect) return WIFI_ENOTSUP;
    if (s.link == WIFI_SCANNING || s.link == WIFI_ASSOCIATING) return WIFI_EBUSY;
    return backend->connect(backend_context, ap,
                            ap->security == WIFI_OPEN ? "" : password);
}
void wifi_poll(void) {
    if (backend && backend->poll) backend->poll(backend_context);
}
