#include "acpi.h"
#include "../ui/calendar.h"
#include "boot.h"
#include "clock.h"
#include "driver.h"
#include "storage.h"
#include "devkit.h"
#include "memory.h"
#include "wifi_profiles.h"
#include "../drivers/iwl_pcie.h"
#include "../drivers/rtw8822be_pcie.h"
#include "install.h"
#include "luna.h"
#include "settings.h"
#include "task.h"

typedef void (*KernelConstructor)(void);
extern KernelConstructor __init_array_start[], __init_array_end[];
#ifdef LUNA_USE_LITEHTML
extern const char __eh_frame_start[];
void __register_frame(const void *frame);
#endif

void kmain(void) {
    serial_init();
    serial_write("\nLunaOS " VERSION " / native boot\n");
    if (boot_is_uefi())
        serial_write("Boot: UEFI x64 firmware / native i386 kernel bridge\n");
    platform_init();
    __asm__ volatile("clts; fninit");
    memory_init();
#ifdef LUNA_USE_LITEHTML
    __register_frame(__eh_frame_start);
#endif
    for (KernelConstructor *init = __init_array_start; init != __init_array_end; init++)
        (*init)();
    serial_write("CPU: protected mode / timer 1000 Hz / E820 memory map\n");
    disk_init();
    /* Claim PCI/DMA early so a SATA/NVMe-installed LunaFS can be opened
     * before loading user settings. BIOS/UEFI already supplied our kernel. */
    task_init();
    driver_services_init();
    storage_pci_init();
    acpi_storage_init();
    disk_resolve_native_boot();
    fs_init();
    settings_init();
    clock_init();
    input_init();
    net_init();
    audio_init();
    lunasdk_edu_init();
    rtw8822be_init();
    wifi_intel_init();
    wifi_profiles_init();
    gui_init();
    gui_draw();
    serial_write("READY\n");
    uint32_t last_clock = ticks, next_frame = frame_clock * prefs.frame_hz + 1000;
    uint32_t next_driver = 0, next_net = 0, next_audio = 0, next_cursor = 0;
    bool cursor_pending = false;
    for (;;) {
        if (input_pending()) {
            PerfArea previous = perf_enter(PERF_INPUT);
            input_poll();
            int c, x, y, buttons, wheel;
            while ((c = input_key()) >= 0)
                gui_key(c);
            while (input_pointer(&x, &y, &buttons, &wheel)) {
                gui_pointer(x, y, buttons);
                if (wheel)
                    gui_scroll(x, y, wheel);
                cursor_pending = true;
            }
            perf_leave(previous);
        }
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
        if (drv_work_pending() || (int32_t)(frame_clock - next_driver) >= 0) {
            PerfArea previous = perf_enter(PERF_DRIVERS);
            driver_services_poll(16);
            perf_leave(previous);
            next_driver = frame_clock + 5;
        }
        if ((int32_t)(frame_clock - next_net) >= 0) {
            net_poll();
            wifi_profiles_poll(!setup_active);
            clock_poll();
            next_net = frame_clock + 5;
        }
        if ((int32_t)(frame_clock - next_audio) >= 0) {
            PerfArea previous = perf_enter(PERF_DRIVERS);
            audio_poll();
            perf_leave(previous);
            next_audio = frame_clock + 20;
        }
        if (ticks - last_clock >= 100) {
            last_clock = ticks;
            calendar_refresh();
            acpi_battery_poll();
            settings_network_refresh();
            gui_refresh_status();
        }
        gui_motion_poll();
        browser_timer_poll();
        uint32_t now = frame_clock * prefs.frame_hz;
        if (ui_dirty && (int32_t)(now - next_frame) >= 0) {
            gui_draw();
            if (compositor_render_ms * prefs.frame_hz > 1000)
                compositor_missed++;
            cursor_pending = false;
            next_frame = frame_clock * prefs.frame_hz + 1000;
        } else if (cursor_pending && (int32_t)(frame_clock - next_cursor) >= 0) {
            /* Keep motion responsive between expensive desktop frames. */
            gfx_cursor_present();
            cursor_pending = false;
            next_cursor = frame_clock + 8;
        }
        task_yield_ready();
        task_idle();
    }
}
