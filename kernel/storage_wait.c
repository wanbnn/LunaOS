#include "storage.h"
#include "task.h"

uint32_t storage_sleeps;

/* Most devices complete before the short polling budget is exhausted.
 * A slower command keeps its buffers reserved while the caller sleeps. */
void storage_poll_wait(unsigned *polls) {
    if (*polls < 32 || !drv_irqs_enabled() || drv_in_irq()) {
        if (*polls < 32)
            (*polls)++;
        __asm__ volatile("pause");
        return;
    }
    if (current_task) {
        Task *task = task_current();
        task->storage_depth++;
        storage_sleeps++;
        task_sleep_ms(1);
        task->storage_depth--;
    } else {
        /* Boot and setup also use this backend before an app can own the I/O. */
        task_idle();
    }
}
