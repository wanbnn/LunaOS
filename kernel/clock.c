#include "clock.h"
#include "../drivers/rtc.h"
#include "../include/net/sntp.h"
#include "../include/net/time_dns.h"
#include "luna.h"
#include "net_time.h"
#include "web.h"
#include "task.h"
static uint64_t wall;
static int64_t correction;
static uint32_t last_tick, deadline, backoff = 16000;
static int zone;
static bool automatic = true, pending, requested;
static unsigned port = 123;
static char server[128] = "time.cloudflare.com";
static char state[96] = "RTC / offline";
static uint8_t nonce[8];
static void advance(void) {
    uint32_t now = frame_clock;
    wall += (uint32_t)(now - last_tick);
    last_tick = now;
}
uint64_t clock_utc_ms(void) {
    advance();
    return wall;
}
int clock_offset(void) {
    return zone;
}
bool clock_auto(void) {
    return automatic;
}
const char *clock_server(void) {
    return server;
}
unsigned clock_port(void) {
    return port;
}
const char *clock_status(void) {
    return state;
}
bool clock_local(DateTime *d) {
    int64_t sec = (int64_t)(clock_utc_ms() / 1000) + (int64_t)zone * 60;
    return sec >= 0 && datetime_from_epoch((uint64_t)sec, d);
}
static void decimal64(char *out, int64_t n) {
    char tmp[24];
    unsigned i = 0, j = 0;
    uint64_t v = n < 0 ? (uint64_t)(-n) : (uint64_t)n;
    if (n < 0)
        out[j++] = '-';
    do {
        tmp[i++] = '0' + v % 10;
        v /= 10;
    } while (v);
    while (i)
        out[j++] = tmp[--i];
    out[j] = 0;
}
static bool parse64(const char *s, int64_t *out) {
    bool neg = *s == '-';
    if (neg)
        s++;
    if (!*s)
        return false;
    uint64_t n = 0;
    while (*s) {
        if (*s < '0' || *s > '9' || n > UINT64_C(100000000000))
            return false;
        n = n * 10 + *s++ - '0';
    }
    if (n > UINT64_C(100000000000))
        return false;
    *out = neg ? -(int64_t)n : (int64_t)n;
    return true;
}
bool clock_write(void) {
    char c[32], v[32], out[512];
    decimal64(c, correction);
    decimal64(v, (int64_t)(clock_utc_ms() / 1000));
    char z[24], p[24];
    decimal64(z, zone);
    number(p, port);
    strcpy(out, "offset=");
    strcat(out, z);
    strcat(out, automatic ? "\nauto=1\nserver=" : "\nauto=0\nserver=");
    strcat(out, server);
    strcat(out, "\nport=");
    strcat(out, p);
    strcat(out, "\ncorrection=");
    strcat(out, c);
    strcat(out, "\nfallback=");
    strcat(out, v);
    strcat(out, "\n");
    return fs_write("/etc/luna-clock", out, false) == 0;
}
static bool persist(void) {
    bool ok = clock_write() && fs_save();
    if (!ok)
        strcpy(state, "Time applied; disk save failed");
    return ok;
}
static void persist_clock_service(void) {
    persist();
}
static void persist_in_background(void) {
    static uint32_t pid;
    if (!pid)
        pid = task_spawn_service("Clock persistence", persist_clock_service);
    if (!pid || !task_post(pid, (AppEvent){EVENT_SERVICE, 0, 0}))
        strcpy(state, "Time synchronized; disk save could not be queued");
}
void clock_init(void) {
    DateTime d;
    uint64_t rtc = 0, fallback = 0;
    bool valid = rtc_read_utc(&d) && datetime_to_epoch(&d, &rtc);
    File *f = fs_find("/etc/luna-clock");
    if (f) {
        char buf[512];
        unsigned n = f->size;
        if (n >= sizeof(buf))
            n = sizeof(buf) - 1;
        if (!fs_read_bytes(f, 0, buf, n))
            n = 0;
        buf[n] = 0;
        char *p = buf;
        while (*p) {
            char *key = p;
            while (*p && *p != '\n' && *p != '=')
                p++;
            if (*p != '=') {
                if (*p)
                    p++;
                continue;
            }
            *p++ = 0;
            char *value = p;
            while (*p && *p != '\n')
                p++;
            if (*p)
                *p++ = 0;
            int64_t n;
            if (!strcmp(key, "server")) {
                if (time_hostname_valid(value))
                    strcpy(server, value);
                continue;
            }
            if (!parse64(value, &n))
                continue;
            if (!strcmp(key, "offset") && n >= -720 && n <= 840)
                zone = (int)n;
            if (!strcmp(key, "auto") && (n == 0 || n == 1))
                automatic = n;
            if (!strcmp(key, "port") && n > 0 && n <= 65535)
                port = n;
            if (!strcmp(key, "correction"))
                correction = n;
            if (!strcmp(key, "fallback") && n >= 0 && (uint64_t)n <= DATETIME_LAST_SECOND)
                fallback = n;
        }
    }
    int64_t initial = (int64_t)rtc + correction;
    wall = (valid && initial >= 0 && (uint64_t)initial <= DATETIME_LAST_SECOND ? (uint64_t)initial
                                                                               : fallback) *
           1000;
    last_tick = frame_clock;
    deadline = frame_clock;
    requested = automatic;
    strcpy(state,
           valid ? "RTC / saved clock adjustment" : "RTC unavailable; set time or synchronize");
}
static void cancel(void) {
    net_time_cancel();
    pending = false;
    requested = false;
}
static void anchor(uint64_t ms) {
    DateTime d;
    uint64_t rtc;
    wall = ms;
    last_tick = frame_clock;
    if (rtc_read_utc(&d) && datetime_to_epoch(&d, &rtc))
        correction = (int64_t)(ms / 1000) - (int64_t)rtc;
}
bool clock_set(const DateTime *d) {
    uint64_t local;
    if (!datetime_to_epoch(d, &local))
        return false;
    int64_t utc = (int64_t)local - (int64_t)zone * 60;
    if (utc < 0 || (uint64_t)utc > DATETIME_LAST_SECOND)
        return false;
    cancel();
    automatic = false;
    anchor((uint64_t)utc * 1000);
    strcpy(state, "Manual time / automatic sync disabled");
    persist();
    return true;
}
bool clock_zone(int minutes) {
    if (minutes < -720 || minutes > 840)
        return false;
    zone = minutes;
    persist();
    return true;
}
bool clock_automatic(bool on) {
    automatic = on;
    cancel();
    requested = on;
    deadline = frame_clock;
    strcpy(state, on ? "Automatic sync enabled" : "Automatic sync disabled");
    persist();
    return true;
}
bool clock_endpoint(const char *host, unsigned p) {
    if (!time_hostname_valid(host) || !p || p > 65535)
        return false;
    cancel();
    strcpy(server, host);
    port = p;
    requested = automatic;
    deadline = frame_clock;
    persist();
    return true;
}
void clock_sync(void) {
    cancel();
    requested = true;
    deadline = frame_clock;
    strcpy(state, net_ready ? "Synchronization queued" : "Waiting for network");
}
void clock_poll(void) {
    advance();
    if (pending) {
        uint8_t reply[512];
        unsigned n;
        uint32_t rtt;
        int rc = net_time_result(reply, sizeof(reply), &n, &rtt);
        if (rc == -11)
            return;
        pending = false;
        uint64_t ms;
        if (!rc)
            rc = sntp_response(reply, n, nonce, rtt, &ms);
        if (!rc) {
            anchor(ms);
            strcpy(state, "Synchronized via NTP");
            backoff = 16000;
            deadline = frame_clock + 21600000;
            requested = false;
            persist_in_background();
        } else {
            strcpy(state, "NTP unavailable / invalid reply; retry scheduled");
            deadline = frame_clock + backoff;
            if (backoff < 1800000)
                backoff *= 2;
            requested = true;
        }
    }
    if (!automatic && !requested)
        return;
    if ((int32_t)(frame_clock - deadline) < 0)
        return;
    if (!net_ready) {
        strcpy(state, "Waiting for network / offline clock active");
        return;
    }
    if (!random_bytes(nonce, sizeof(nonce))) {
        deadline = frame_clock + 16000;
        return;
    }
    uint8_t packet[48];
    sntp_request(nonce, packet);
    int rc = net_time_begin(server, port, packet);
    if (!rc) {
        pending = true;
        strcpy(state, "Synchronizing via NTP...");
    } else
        deadline = frame_clock + 1000;
}
void rtc_string(char *s) {
    DateTime d;
    char b[20];
    if (!clock_local(&d)) {
        strcpy(s, "---- -- --  --:--");
        return;
    }
    datetime_format(&d, b);
    memcpy(s, b, 10);
    s[10] = ' ';
    s[11] = ' ';
    memcpy(s + 12, b + 11, 5);
    s[17] = 0;
}
