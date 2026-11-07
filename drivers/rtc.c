#include "rtc.h"
#include "driver.h"
static int bcd(uint8_t value) {
    return (value & 15) > 9 || (value >> 4) > 9 ? -1 : (value >> 4) * 10 + (value & 15);
}
bool rtc_snapshot_decode(const RtcSnapshot *s, DateTime *out) {
    if (!s || !out)
        return false;
    bool binary = s->status & 4, hours24 = s->status & 2, pm = s->hour & 128;
    int y = binary ? s->year : bcd(s->year), century = binary ? s->century : bcd(s->century);
    int hour = binary ? s->hour & 127 : bcd(s->hour & 127);
    if (y < 0 || y > 99 || hour < 0)
        return false;
    if (!hours24) {
        if (hour < 1 || hour > 12)
            return false;
        hour = hour % 12 + (pm ? 12 : 0);
    }
    if (century != 19 && century != 20 && century != 21)
        century = y >= 70 ? 19 : 20;
    DateTime d = {century * 100 + y,
                  binary ? s->month : bcd(s->month),
                  binary ? s->day : bcd(s->day),
                  hour,
                  binary ? s->minute : bcd(s->minute),
                  binary ? s->second : bcd(s->second)};
    if (!datetime_valid(&d))
        return false;
    *out = d;
    return true;
}
static uint8_t reg(uint8_t address) {
    outb(0x70, address);
    return inb(0x71);
}
static void snapshot(RtcSnapshot *s) {
    *s = (RtcSnapshot){reg(0), reg(2), reg(4), reg(7), reg(8), reg(9), reg(0x32), reg(0xb)};
}
bool rtc_read_utc(DateTime *out) {
    if (!out)
        return false;
    uint32_t flags = drv_irq_save();
    bool ok = false;
    for (unsigned tries = 0; tries < 5; tries++) {
        unsigned budget = 10000;
        while ((reg(0xa) & 128) && --budget) {
        }
        if (!budget)
            break;
        RtcSnapshot a, b;
        snapshot(&a);
        if (reg(0xa) & 128)
            continue;
        snapshot(&b);
        if (!(reg(0xa) & 128) && !memcmp(&a, &b, sizeof(a))) {
            ok = rtc_snapshot_decode(&a, out);
            break;
        }
    }
    outb(0x70, 0);
    drv_irq_restore(flags);
    return ok;
}
