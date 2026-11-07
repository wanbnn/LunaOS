#include "wifi.h"
/* No fake APs. The native kernel driver must provide all operations and must
 * have initialized its radio before registering. No driver is registered yet. */
#define WIFI_EBUSY (-16)
#define WIFI_EINVAL (-22)
#define WIFI_ENOTSUP (-95)
static const WifiDriverOps *backend;
static void *backend_context;
static int valid_ap(const WifiAp *ap) {
    if (!ap || !ap->ssid_len || ap->ssid_len > WIFI_MAX_SSID ||
        ap->ssid[ap->ssid_len]) return WIFI_EINVAL;
    for (unsigned i = 0; i < ap->ssid_len; i++)
        if (!ap->ssid[i]) return WIFI_EINVAL;
    if (ap->security != WIFI_OPEN && ap->security != WIFI_WPA2_PSK &&
        ap->security != WIFI_OTHER) return WIFI_EINVAL;
    return 0;
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
    return backend->connect(backend_context, ap,
                            ap->security == WIFI_OPEN ? "" : password);
}
void wifi_poll(void) {
    if (backend && backend->poll) backend->poll(backend_context);
}
