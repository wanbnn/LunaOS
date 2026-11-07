#ifndef LUNA_WINDOW_H
#define LUNA_WINDOW_H
#include "task.h"
typedef struct {
    int app, x, y, w, h, restore_x, restore_y, restore_w, restore_h;
    uint32_t pid;
    bool open, minimized, maximized;
} Window;
extern Window windows[APP_COUNT];
const char *app_name(int);
void window_close(int);
void window_minimize(int);
void window_maximize(int);
void window_focus(int);
void app_draw(int, int, int, int, int);
void app_click(int, int, int);
void app_key(int, int);
void notes_key(int);
void notes_draw(int,int,int,int);
void notes_click(int,int);
#endif
