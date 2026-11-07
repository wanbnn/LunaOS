#ifndef LUNA_CALENDAR_H
#define LUNA_CALENDAR_H
#include <stdbool.h>
void calendar_draw(void);
bool calendar_click(int, int);
void calendar_refresh(void);
void time_settings_load(void);
void time_settings_draw(int, int, int);
void time_settings_click(int, int);
void time_settings_key(int);
#endif
