/* An API contract test with an explicit fake backend. Not a radio test. */
#include <assert.h>
#include <string.h>
#include "../kernel/wifi.h"
static unsigned starts, connects;
static bool can_connect = true;
static WifiLink link = WIFI_IDLE;
static int scan(void *p) { assert(p == &starts); starts++; return 0; }
static int results(void *p, WifiAp *out, unsigned cap, unsigned *count) {
    assert(p == &starts && cap > 0);
    out[0] = (WifiAp){.ssid = "test-ap", .ssid_len = 7,
                      .bssid = {2,0,0,0,0,1}, .channel = 11, .rssi_dbm = -42, .security = WIFI_WPA2_PSK};
    *count = 1;
    return 0;
}
static int connect(void *p, const WifiAp *ap, const char *pw) {
    assert(p == &starts && !strcmp(ap->ssid, "test-ap") &&
           !strcmp(pw, "12345678"));
    connects++;
    return 0;
}
static int state(void *p, WifiState *out) {
    assert(p == &starts);
    *out = (WifiState){.radio_ready = true, .can_connect = can_connect, .link = link};
    return 0;
}
static void poll(void *p) { assert(p == &starts); }
int main(void) {
    WifiState s;
    WifiAp aps[WIFI_MAX_APS];
    unsigned n = 123;
    const WifiDriverOps ops = {scan, results, connect, state, poll};
    assert(wifi_channel_frequency(1)==2412 && wifi_channel_frequency(14)==2484);
    assert(wifi_channel_frequency(36)==5180 && wifi_channel_frequency(149)==5745);
    assert(!wifi_channel_frequency(0) && !wifi_channel_frequency(15) &&
           !wifi_channel_frequency(35) && !wifi_channel_frequency(196));
    assert(wifi_channel_band(14)==WIFI_BAND_2GHZ);
    assert(wifi_channel_band(177)==WIFI_BAND_5GHZ);
    assert(wifi_channel_band(0)==WIFI_BAND_UNKNOWN);
    assert(wifi_state(&s) == -95 && s.link == WIFI_DOWN);
    assert(wifi_scan_start() == -95);
    assert(wifi_scan_results(aps, WIFI_MAX_APS, &n) == -95 && n == 0);
    assert(wifi_register_driver(&ops, &starts) == 0);
    assert(wifi_scan_start() == 0 && starts == 1);
    assert(wifi_scan_results(aps, WIFI_MAX_APS, &n) == 0 && n == 1);
    assert(wifi_connect_ap(aps, "bad") == -22);
    assert(wifi_connect_ap(aps, "12345678") == 0 && connects == 1);
    WifiAp bad = aps[0];
    bad.channel = 15;
    assert(wifi_connect_ap(&bad, "12345678")==-22);
    bad = aps[0]; bad.bssid[0] = 1;
    assert(wifi_connect_ap(&bad, "12345678")==-22);
    bad = aps[0]; memset(bad.bssid, 0, 6);
    assert(wifi_connect_ap(&bad, "12345678")==-22);
    can_connect = false;
    assert(wifi_connect_ap(aps, "12345678")==-95 && connects==1);
    can_connect = true; link = WIFI_SCANNING;
    assert(wifi_connect_ap(aps, "12345678")==-16 && connects==1);
    link = WIFI_ASSOCIATING;
    assert(wifi_connect_ap(aps, "12345678")==-16 && connects==1);
    link = WIFI_IDLE;
    wifi_poll();
    wifi_unregister_driver(&ops);
    assert(wifi_scan_start() == -95);
    return 0;
}
