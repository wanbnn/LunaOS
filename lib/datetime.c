#include "datetime.h"
bool datetime_leap(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}
int datetime_month_days(int year, int month) {
    static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month < 1 || month > 12 ? 0 : days[month - 1] + (month == 2 && datetime_leap(year));
}
bool datetime_valid(const DateTime *d) {
    return d && d->year >= 1970 && d->year <= 2199 && d->month >= 1 && d->month <= 12 &&
           d->day >= 1 && d->day <= datetime_month_days(d->year, d->month) && d->hour >= 0 &&
           d->hour < 24 && d->minute >= 0 && d->minute < 60 && d->second >= 0 && d->second < 60;
}
bool datetime_to_epoch(const DateTime *d, uint64_t *out) {
    if (!out || !datetime_valid(d))
        return false;
    uint64_t days = 0;
    for (int y = 1970; y < d->year; y++)
        days += 365 + datetime_leap(y);
    for (int m = 1; m < d->month; m++)
        days += datetime_month_days(d->year, m);
    *out = (days + d->day - 1) * 86400 + d->hour * 3600 + d->minute * 60 + d->second;
    return true;
}
bool datetime_from_epoch(uint64_t epoch, DateTime *out) {
    if (!out || epoch > DATETIME_LAST_SECOND)
        return false;
    uint32_t days = epoch / 86400;
    DateTime d = {.year = 1970,
                  .month = 1,
                  .hour = epoch / 3600 % 24,
                  .minute = epoch / 60 % 60,
                  .second = epoch % 60};
    while (days >= 365u + datetime_leap(d.year))
        days -= 365 + datetime_leap(d.year++);
    while (days >= (unsigned)datetime_month_days(d.year, d.month))
        days -= datetime_month_days(d.year, d.month++);
    d.day = days + 1;
    *out = d;
    return true;
}
int datetime_weekday(const DateTime *d) {
    uint64_t epoch;
    return datetime_to_epoch(d, &epoch) ? (int)((epoch / 86400 + 3) % 7) : -1;
}
static int digits(const char *p, unsigned n) {
    int value = 0;
    for (unsigned i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9')
            return -1;
        value = value * 10 + p[i] - '0';
    }
    return value;
}
bool datetime_parse(const char *date, const char *time, DateTime *out) {
    if (!date || !time || !out)
        return false;
    unsigned a = 0, b = 0;
    while (a < 11 && date[a])
        a++;
    while (b < 9 && time[b])
        b++;
    if (a != 10 || b != 8 || date[4] != '-' || date[7] != '-' || time[2] != ':' || time[5] != ':')
        return false;
    DateTime d = {digits(date, 4), digits(date + 5, 2), digits(date + 8, 2),
                  digits(time, 2), digits(time + 3, 2), digits(time + 6, 2)};
    if (!datetime_valid(&d))
        return false;
    *out = d;
    return true;
}
static void two(char *p, int n) {
    p[0] = '0' + n / 10;
    p[1] = '0' + n % 10;
}
void datetime_format(const DateTime *d, char out[20]) {
    if (!out)
        return;
    if (!datetime_valid(d)) {
        out[0] = 0;
        return;
    }
    two(out, d->year / 100);
    two(out + 2, d->year % 100);
    out[4] = '-';
    two(out + 5, d->month);
    out[7] = '-';
    two(out + 8, d->day);
    out[10] = ' ';
    two(out + 11, d->hour);
    out[13] = ':';
    two(out + 14, d->minute);
    out[16] = ':';
    two(out + 17, d->second);
    out[19] = 0;
}
bool datetime_offset_parse(const char *s, int *minutes) {
    if (!s || !minutes || (s[0] != '+' && s[0] != '-'))
        return false;
    unsigned n = 0;
    while (n < 7 && s[n])
        n++;
    if (n != 6 || s[3] != ':')
        return false;
    int h = digits(s + 1, 2), m = digits(s + 4, 2);
    if (h < 0 || m < 0 || m >= 60)
        return false;
    int offset = (h * 60 + m) * (s[0] == '-' ? -1 : 1);
    if (offset < -720 || offset > 840)
        return false;
    *minutes = offset;
    return true;
}
void datetime_offset_format(int minutes, char out[7]) {
    out[0] = minutes < 0 ? '-' : '+';
    if (minutes < 0)
        minutes = -minutes;
    two(out + 1, minutes / 60);
    out[3] = ':';
    two(out + 4, minutes % 60);
    out[6] = 0;
}
