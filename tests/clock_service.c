#include "../drivers/rtc.h"
#include "../include/net/sntp.h"
#include "clock.h"
#include "luna.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
volatile uint32_t ticks, frame_clock;
bool net_ready;
static char saved_data[8192];
static File saved = {.data = saved_data};
static bool exists, active;
static int result = -11;
static uint8_t packet[48];
static uint64_t hardware = 1794052800;
File *fs_find(const char *s) {
    (void)s;
    return exists ? &saved : NULL;
}
int fs_write(const char *p, const char *s, bool a) {
    (void)p;
    (void)a;
    strcpy(saved.data, s);
    saved.size = strlen(s);
    exists = true;
    return 0;
}
bool fs_read_bytes(const File *file, uint32_t offset, void *out, size_t bytes) {
    if (offset > file->size || bytes > file->size - offset)
        return false;
    memcpy(out, file->data + offset, bytes);
    return true;
}
bool fs_save(void) {
    return true;
}
bool rtc_read_utc(DateTime *d) {
    return datetime_from_epoch(hardware, d);
}
bool random_bytes(void *p, size_t n) {
    memset(p, 7, n);
    return true;
}
void number(char *s, uint32_t n) {
    sprintf(s, "%u", n);
}
void net_time_cancel(void) {
    active = false;
    result = -11;
}
int net_time_begin(const char *h, uint16_t p, const uint8_t *req) {
    (void)h;
    (void)p;
    active = true;
    memcpy(packet, req, 48);
    return 0;
}
int net_time_result(uint8_t *p, unsigned c, unsigned *n, uint32_t *r) {
    (void)c;
    if (result)
        return result;
    memcpy(p, packet, 48);
    *n = 48;
    *r = 100;
    active = false;
    return 0;
}
static void put32(unsigned char *p, uint32_t v) {
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}
int main(void) {
    clock_init();
    assert(clock_utc_ms() == hardware * 1000);
    DateTime d;
    assert(clock_zone(-180));
    assert(datetime_parse("2032-02-29", "12:34:56", &d));
    assert(clock_set(&d));
    assert(!clock_auto());
    uint64_t set = clock_utc_ms();
    frame_clock += 1500;
    assert(clock_utc_ms() == set + 1500);
    hardware += 5;
    clock_init();
    assert(clock_utc_ms() == set + 5000);
    assert(clock_offset() == -180 && !clock_auto());
    frame_clock = UINT32_MAX - 10;
    clock_init();
    uint64_t before = clock_utc_ms();
    frame_clock = 19;
    assert(clock_utc_ms() == before + 30);
    net_ready = true;
    clock_sync();
    clock_poll();
    assert(active);
    uint8_t nonce[8];
    memcpy(nonce, packet + 40, 8);
    memset(packet, 0, 48);
    packet[0] = 0x24;
    packet[1] = 2;
    memcpy(packet + 24, nonce, 8);
    uint64_t target = 2208988800u;
    put32(packet + 32, (uint32_t)(target + 2208988800u));
    put32(packet + 40, (uint32_t)(target + 2208988800u));
    result = 0;
    clock_poll();
    assert(clock_utc_ms() == target * 1000 + 50);
    assert(strstr(clock_status(), "Synchronized"));
    clock_sync();
    clock_poll();
    assert(active);
    assert(clock_set(&d));
    assert(!active);
    before = clock_utc_ms();
    frame_clock += 1000;
    clock_poll();
    assert(clock_utc_ms() == before + 1000);
    assert(!clock_endpoint("bad name", 123));
    assert(!clock_endpoint("time.cloudflare.com", 65536));
    puts("clock: offline persistence, UTC/local distinction, monotonic wrap, NTP integration and "
         "manual cancellation passed");
}
