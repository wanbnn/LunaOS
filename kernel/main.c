#include "../ui/calendar.h"
#include "boot.h"
#include "clock.h"
#include "driver.h"
#include "devkit.h"
#include "memory.h"
#include "wifi.h"
#include "../drivers/iwl_pcie.h"
#include "../drivers/rtw8822be_pcie.h"
#include "install.h"
#include "luna.h"
#include "settings.h"
#include "task.h"
void kmain(void) {
    serial_init();
    serial_write("\nLunaOS " VERSION " / native boot\n");
    if (boot_is_uefi())
        serial_write("Boot: UEFI x64 firmware / native i386 kernel bridge\n");
    platform_init();
    __asm__ volatile("clts; fninit");
    memory_init();
    serial_write("CPU: protected mode / timer 1000 Hz / E820 memory map\n");
    disk_init();
    fs_init();
    settings_init();
    clock_init();
    input_init();
    net_init();
    audio_init();
    task_init();
    driver_services_init();
    lunasdk_edu_init();
    rtw8822be_init();
    wifi_intel_init();
    gui_init();
    gui_draw();
    serial_write("READY\n");
    uint32_t last_clock = ticks, next_frame = frame_clock * prefs.frame_hz + 1000;
    uint32_t next_driver = 0, next_net = 0, next_audio = 0;
    bool cursor_pending = false;
    for (;;) {
                input_poll();
        int c;
        while ((c = input_key()) >= 0)
            gui_key(c);
        for (int i = 0; i < 1; i++) {
            int s = serial_get();
            if (s < 0)
                break;
            if (s == 13)
                s = 10;
            if (s == 9 || s == 10 || s == 8 || s == 127 || (s >= 32 && s < 127)) {
                gui_serial(s);
            }
        }
        setup_poll();
        clock_poll();
        /* Deferred driver work and the polled devices run on their own
         * deadlines. An idle kernel no longer polls every service a thousand
         * times per second just because the PIT ticks at 1 kHz. */
        if (frame_clock >= next_driver) {
            driver_services_poll(16);
            wifi_poll();
            next_driver = frame_clock + 5;
        }
        if (frame_clock >= next_net) {
            net_poll();
            next_net = frame_clock + 5;
        }
        if (frame_clock >= next_audio) {
            audio_poll();
            next_audio = frame_clock + 20;
        }
        if (mouse_changed) {
            int x, y, buttons;
            while (input_pointer(&x, &y, &buttons))
                gui_pointer(x, y, buttons);
            mouse_changed = false;
            cursor_pending = true;
        }
        if (ticks - last_clock >= 100) {
            last_clock = ticks;
            calendar_refresh();
            gui_invalidate_app(APP_TASKS);
            gui_damage(W - 136, 8, 130, 28); /* the clock in the top bar */
        }
        gui_motion_poll();
        if (ui_dirty || cursor_pending) {
            uint32_t now = frame_clock * prefs.frame_hz;
                        if ((int32_t)(now - next_frame) >= 0) {
                if (ui_dirty) {
                    gui_draw();
                    if (compositor_render_ms * prefs.frame_hz > 1000)
                        compositor_missed++;
                } else
                    gfx_cursor_present();
                cursor_pending = false;
                /* The next frame is scheduled one interval from now: a late
                 * frame is dropped instead of a backlog accumulating. */
                next_frame = frame_clock * prefs.frame_hz + 1000;
            }
        }
        task_yield();
        task_idle();
    }
}
