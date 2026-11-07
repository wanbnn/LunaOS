#include "../drivers/rtc.h"
#include "../include/net/sntp.h"
#include "../include/net/time_dns.h"
#include "datetime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void put32(unsigned char *p, uint32_t x) {
    p[0] = x >> 24;
    p[1] = x >> 16;
    p[2] = x >> 8;
    p[3] = x;
}
int main(void) {
    DateTime d, out;
    uint64_t t;
    assert(datetime_parse("2000-02-29", "23:59:59", &d));
    assert(!datetime_parse("2100-02-29", "00:00:00", &d));
    for (uint64_t s = 0; s <= DATETIME_LAST_SECOND; s += 86399) {
        assert(datetime_from_epoch(s, &d));
        assert(datetime_to_epoch(&d, &t) && s == t);
    }
    assert(datetime_from_epoch(DATETIME_LAST_SECOND, &d) && d.year == 2199 && d.month == 12 &&
           d.day == 31);
    assert(!datetime_from_epoch(DATETIME_LAST_SECOND + 1, &d));
    assert(datetime_parse("1970-01-01", "00:00:00", &d) && datetime_weekday(&d) == 3);
    int z;
    assert(datetime_offset_parse("-03:00", &z) && z == -180);
    assert(!datetime_offset_parse("+14:01", &z));
    assert(!datetime_offset_parse("-12:01", &z));
    RtcSnapshot rtc = {0x59, 0x58, 0x92, 0x29, 0x02, 0x24, 0x20, 0};
    assert(rtc_snapshot_decode(&rtc, &d) && d.hour == 12 && d.year == 2024 && d.second == 59);
    rtc.hour = 0x12;
    assert(rtc_snapshot_decode(&rtc, &d) && d.hour == 0);
    rtc.month = 0x1a;
    assert(!rtc_snapshot_decode(&rtc, &d));
    uint8_t token[8] = {1, 2, 3, 4, 5, 6, 7, 8}, p[512];
    sntp_request(token, p);
    assert(p[0] == 0x23 && !memcmp(p + 40, token, 8));
    memset(p, 0, 48);
    p[0] = 0x24;
    p[1] = 2;
    memcpy(p + 24, token, 8);
    assert(datetime_parse("2040-01-01", "00:00:00", &out) && datetime_to_epoch(&out, &t));
    put32(p + 32, (uint32_t)(t + 2208988800u));
    put32(p + 40, (uint32_t)(t + 2208988800u));
    uint64_t ms;
    assert(!sntp_response(p, 48, token, 100, &ms) && ms == t * 1000 + 50);
    p[24] ^= 1;
    assert(sntp_response(p, 48, token, 100, &ms));
    p[24] ^= 1;
    p[1] = 0;
    assert(sntp_response(p, 48, token, 100, &ms) == -11);
    p[1] = 2;
    p[0] |= 0xc0;
    assert(sntp_response(p, 48, token, 100, &ms));
    unsigned n;
    assert(!time_dns_question("time.cloudflare.com", 123, p, sizeof(p), &n));
    unsigned q = n;
    p[2] = 0x81;
    p[3] = 0x80;
    p[7] = 1;
    const uint8_t answer[] = {0xc0, 12, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 162, 159, 200, 1};
    memcpy(p + q, answer, sizeof(answer));
    n += sizeof(answer);
    uint8_t ip[4];
    assert(!time_dns_answer("time.cloudflare.com", 123, p, n, ip) && ip[0] == 162);
    assert(time_dns_answer("evil.example", 123, p, n, ip));
    assert(time_dns_answer("time.cloudflare.com", 124, p, n, ip));
    p[q] = 0xc0;
    p[q + 1] = q;
    assert(time_dns_answer("time.cloudflare.com", 123, p, n, ip));
    assert(!time_hostname_valid("-bad.example"));
    assert(!time_hostname_valid("a..b"));
    puts("calendar: Gregorian dates, UTC offsets, RTC modes, NTP era/validation, DNS validation "
         "passed");
    return 0;
}
