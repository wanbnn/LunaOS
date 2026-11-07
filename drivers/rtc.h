#ifndef LUNA_RTC_H
#define LUNA_RTC_H
#include "datetime.h"
typedef struct {
    uint8_t second, minute, hour, day, month, year, century, status;
} RtcSnapshot;
bool rtc_snapshot_decode(const RtcSnapshot *, DateTime *);
bool rtc_read_utc(DateTime *); /* Stable, bounded CMOS read; no writes to RTC data. */
#endif
