#ifndef LUNA_CLOCK_H
#define LUNA_CLOCK_H
#include "datetime.h"
void clock_init(void);
void clock_poll(void);
uint64_t clock_utc_ms(void);
bool clock_local(DateTime *);
int clock_offset(void);
bool clock_auto(void);
const char *clock_server(void);
unsigned clock_port(void);
const char *clock_status(void);
bool clock_write(void);
bool clock_set(const DateTime *);
bool clock_zone(int minutes);
bool clock_automatic(bool enabled);
bool clock_endpoint(const char *, unsigned);
void clock_sync(void);
#endif
