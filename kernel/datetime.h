#ifndef LUNA_DATETIME_H
#define LUNA_DATETIME_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define DATETIME_LAST_SECOND UINT64_C(7258118399) /* 2199-12-31 23:59:59 */
typedef struct {
    int year, month, day, hour, minute, second;
} DateTime;
bool datetime_leap(int year);
int datetime_month_days(int year, int month);
bool datetime_valid(const DateTime *);
bool datetime_to_epoch(const DateTime *, uint64_t *);
bool datetime_from_epoch(uint64_t, DateTime *);
int datetime_weekday(const DateTime *); /* Monday=0 .. Sunday=6 */
bool datetime_parse(const char *date, const char *time, DateTime *);
void datetime_format(const DateTime *, char out[20]); /* ISO local/UTC, caller chooses */
bool datetime_offset_parse(const char *, int *minutes);
void datetime_offset_format(int minutes, char out[7]);
#endif
