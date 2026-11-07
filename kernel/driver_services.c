#include "driver.h"
#include "task.h"
#include "memory.h"
#define PAGE 4096u
#define MAX_WORK 64
#define MAX_TIMERS 32
static bool polling;
static unsigned work_count;
static DrvWork *work_head, *work_tail;
static DrvTimer *timers[MAX_TIMERS];
static uint32_t cycles_per_us;
static unsigned interrupt_depth;
uint32_t drv_irq_save(void) {
    uint32_t flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags)::"memory");
    return flags;
}
void drv_irq_restore(uint32_t flags) {
    if (flags & 0x200)
        __asm__ volatile("sti" ::: "memory");
    else
        __asm__ volatile("cli" ::: "memory");
}
bool drv_irqs_enabled(void) {
    uint32_t flags;
    __asm__ volatile("pushfl; popl %0" : "=r"(flags));
    return (flags & 0x200) != 0;
}
bool drv_in_irq(void) {
    return interrupt_depth != 0;
}
/* Only the interrupt dispatcher may change the IRQ context depth. */
void driver_irq_enter(void) {
    interrupt_depth++;
}
void driver_irq_leave(void) {
    interrupt_depth--;
}
void drv_memory_barrier(void) {
    __sync_synchronize();
}
int drv_lock(DrvLock *lock, uint32_t *flags) {
    if (!lock || !flags)
        return DRV_EINVAL;
    *flags = drv_irq_save();
    if (lock->held) {
        drv_irq_restore(*flags);
        return DRV_EBUSY;
    }
    lock->held = true;
    __asm__ volatile("" ::: "memory");
    return 0;
}
void drv_unlock(DrvLock *lock, uint32_t flags) {
    __asm__ volatile("" ::: "memory");
    lock->held = false;
    drv_irq_restore(flags);
}
uint32_t drv_now_ms(void) {
    return frame_clock;
}
static uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
int drv_udelay(unsigned us) {
    if (us > 1000)
        return DRV_EINVAL;
    if (!cycles_per_us)
        return DRV_ENOTSUP;
    uint64_t end = rdtsc() + (uint64_t)us * cycles_per_us;
    while (rdtsc() < end)
        __asm__ volatile("pause");
    return 0;
}
void driver_services_init(void) {
    uint32_t a = 1, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    if ((d & (1u << 4)) && drv_irqs_enabled()) {
        uint32_t start = frame_clock;
        while (frame_clock == start)
            __asm__ volatile("hlt");
        start = frame_clock;
        uint64_t before = rdtsc();
        while (frame_clock - start < 10)
            __asm__ volatile("hlt");
        uint64_t rate = (rdtsc() - before) / ((frame_clock - start) * 1000u);
        if (rate && rate <= UINT32_MAX)
            cycles_per_us = rate;
    }
    MemoryStats stats; memory_stats(&stats);
    serial_write(stats.ready ? "Driver services: shared physical RAM / DMA masks / workers / timers\n"
                             : "Driver services: physical memory manager unavailable\n");
}
int drv_dma_alloc(size_t bytes, size_t align, uint32_t mask, size_t boundary, DrvDma *out) {
    if (!out) return DRV_EINVAL;
    if (out->cpu || out->cookie) return DRV_EBUSY;
    if (drv_in_irq() && bytes > PAGE) return DRV_ENOTSUP;
    PmmAllocation allocation={0};
    int rc=memory_allocate(bytes,align,mask,boundary,false,true,&allocation);
    if (rc) return rc;
    *out=(DrvDma){(void *)(uintptr_t)allocation.address,allocation.address,bytes,allocation.cookie};
    return 0;
}
int drv_dma_free(DrvDma *mem) {
    if (!mem || mem->bus!=(uintptr_t)mem->cpu) return DRV_EINVAL;
    PmmAllocation allocation={mem->bus,mem->size,mem->cookie};
    int rc=memory_release(&allocation);
    if (!rc) memset(mem,0,sizeof(*mem));
    return rc;
}
void *drv_alloc(size_t bytes) {
    DrvDma mem={0};
    return drv_dma_alloc(bytes,16,UINT32_MAX,0,&mem) ? 0 : mem.cpu;
}
int drv_free(void *ptr) {
    if (!ptr) return 0;
    PmmAllocation allocation;
    int rc=memory_lookup((uintptr_t)ptr,&allocation);
    return rc ? rc : memory_release(&allocation);
}
size_t drv_dma_available(void) {
    MemoryStats stats; memory_stats(&stats);
    return (size_t)stats.free_bytes;
}
void drv_work_init(DrvWork *work, DrvCallback fn, void *arg) {
    *work = (DrvWork){.fn = fn, .arg = arg};
}
int drv_work_queue(DrvWork *work) {
    if (!work || !work->fn)
        return DRV_EINVAL;
    uint32_t flags = drv_irq_save();
    if (work->queued) {
        drv_irq_restore(flags);
        return DRV_EALREADY;
    }
    if (work_count == MAX_WORK) {
        drv_irq_restore(flags);
        return DRV_ENOSPC;
    }
    work->queued = true;
    work->next = 0;
    if (work_tail)
        work_tail->next = work;
    else
        work_head = work;
    work_tail = work;
    work_count++;
    drv_irq_restore(flags);
    return 0;
}
int drv_work_cancel(DrvWork *work) {
    if (!work)
        return DRV_EINVAL;
    uint32_t flags = drv_irq_save();
    if (work->running) {
        drv_irq_restore(flags);
        return DRV_EBUSY;
    }
    DrvWork *prev = 0, *it = work_head;
    while (it && it != work) {
        prev = it;
        it = it->next;
    }
    if (it) {
        if (prev)
            prev->next = it->next;
        else
            work_head = it->next;
        if (work_tail == it)
            work_tail = prev;
        it->queued = false;
        it->next = 0;
        work_count--;
    }
    drv_irq_restore(flags);
    return 0;
}
void drv_timer_init(DrvTimer *timer, DrvCallback fn, void *arg) {
    *timer = (DrvTimer){0};
    drv_work_init(&timer->work, fn, arg);
}
int drv_timer_start(DrvTimer *timer, uint32_t delay) {
    if (!timer || !timer->work.fn || !delay || delay > INT32_MAX)
        return DRV_EINVAL;
    uint32_t flags = drv_irq_save();
    if (timer->work.queued) {
        drv_irq_restore(flags);
        return DRV_EBUSY;
    }
    int slot = -1;
    for (unsigned i = 0; i < MAX_TIMERS; i++) {
        if (timers[i] == timer) {
            slot = i;
            break;
        }
        if (!timers[i] && slot < 0)
            slot = i;
    }
    if (slot < 0) {
        drv_irq_restore(flags);
        return DRV_ENOSPC;
    }
    timer->deadline = frame_clock + delay;
    timer->armed = true;
    timers[slot] = timer;
    drv_irq_restore(flags);
    return 0;
}
int drv_timer_cancel(DrvTimer *timer) {
    if (!timer)
        return DRV_EINVAL;
    uint32_t flags = drv_irq_save();
    int result = drv_work_cancel(&timer->work);
    if (!result) {
        for (unsigned i = 0; i < MAX_TIMERS; i++)
            if (timers[i] == timer)
                timers[i] = 0;
        timer->armed = false;
    }
    drv_irq_restore(flags);
    return result;
}
void driver_services_poll(unsigned budget) {
    if (!drv_irqs_enabled() || drv_in_irq() || polling)
        return;
    polling = true;
    for (unsigned i = 0; i < MAX_TIMERS; i++) {
        uint32_t flags = drv_irq_save();
        DrvTimer *t = timers[i];
        if (t && (int32_t)(frame_clock - t->deadline) >= 0 && !drv_work_queue(&t->work)) {
            timers[i] = 0;
            t->armed = false;
        }
        drv_irq_restore(flags);
    }
    if (budget > MAX_WORK)
        budget = MAX_WORK;
    while (budget--) {
        uint32_t flags = drv_irq_save();
        DrvWork *w = work_head;
        if (!w) {
            drv_irq_restore(flags);
            break;
        }
        work_head = w->next;
        if (!work_head)
            work_tail = 0;
        work_count--;
        w->next = 0;
        w->queued = false;
        w->running = true;
        drv_irq_restore(flags);
        w->fn(w->arg);
        flags = drv_irq_save();
        w->running = false;
        drv_irq_restore(flags);
    }
    polling = false;
}
void drv_complete(DrvCompletion *completion) {
    if (!completion)
        return;
    uint32_t flags = drv_irq_save();
    if (completion->count != UINT32_MAX)
        completion->count++;
    drv_irq_restore(flags);
}
int drv_wait(DrvCompletion *completion, uint32_t timeout) {
    if (!completion || timeout > INT32_MAX)
        return DRV_EINVAL;
    if (!drv_irqs_enabled() || drv_in_irq() || polling)
        return DRV_EDEADLK;
    uint32_t start = frame_clock;
    for (;;) {
        uint32_t flags = drv_irq_save();
        if (completion->count) {
            completion->count--;
            drv_irq_restore(flags);
            return 0;
        }
        drv_irq_restore(flags);
        if (frame_clock - start >= timeout)
            return DRV_ETIMEDOUT;
        driver_services_poll(8);
        task_yield();
        __asm__ volatile("hlt");
    }
}
int drv_sleep_ms(uint32_t ms) {
    DrvCompletion unused = {0};
    int r = drv_wait(&unused, ms);
    return r == DRV_ETIMEDOUT ? 0 : r;
}
void driver_services_status(char *out, size_t cap) {
    char n[20];
    out[0] = 0;
    append(out, "Native driver services v1 / x86 UP\nShared DMA-addressable free RAM: ", cap);
    number(n, drv_dma_available());
    append(out, n, cap);
    append(out, " bytes\nPCI INTx / uncached MMIO / packet queues / binary firmware\n", cap);
    append(out, "Workers: bounded cooperative callbacks; not kernel threads.\n", cap);
    append(out, "MSI/APIC, IOMMU, USB/HCI and mac80211 compatibility: unavailable.\n", cap);
}
