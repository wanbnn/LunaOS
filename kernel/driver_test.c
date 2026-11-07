#include "driver.h"
/* Executed inside LunaOS, including negative paths and real QEMU PCI DMA/INTx. */
typedef struct {
    DrvWork work;
    DrvCompletion done;
    unsigned count;
    bool outside_irq;
} WorkTest;
static void work_once(void *arg) {
    WorkTest *t = arg;
    t->count++;
    t->outside_irq = !drv_in_irq();
    drv_complete(&t->done);
}
static void work_repeat(void *arg) {
    WorkTest *t = arg;
    t->count++;
    if (t->count < 3)
        drv_work_queue(&t->work);
}
static void log_test(char *out, size_t cap, const char *name, bool success) {
    append(out, success ? "PASS: " : "FAIL: ", cap);
    append(out, name, cap);
    append(out, "\n", cap);
}
typedef struct {
    DrvMmio mmio;
    DrvCompletion done;
    DrvWork work;
    unsigned shared_calls, irq_calls;
    bool irq_context, deferred_context, wait_rejected;
} EduTest;
static void edu_deferred(void *arg) {
    EduTest *e = arg;
    e->deferred_context = !drv_in_irq() && drv_irqs_enabled();
    drv_complete(&e->done);
}
static bool edu_unowned(void *arg) {
    EduTest *e = arg;
    e->shared_calls++;
    return false;
}
static bool edu_irq(void *arg) {
    EduTest *e = arg;
    uint32_t status = 0;
    if (drv_mmio_read(&e->mmio, 0x24, 4, &status) || !status)
        return false;
    e->irq_context = drv_in_irq() && !drv_irqs_enabled();
    e->wait_rejected = drv_wait(&e->done, 0) == DRV_EDEADLK;
    drv_mmio_write(&e->mmio, 0x64, 4, status);
    e->irq_calls++;
    drv_work_queue(&e->work);
    return true;
}
static int test_edu(char *out, size_t cap) {
    DrvPci device = {0}, duplicate = {0};
    DrvIrq first = {0}, second = {0};
    DrvDma source = {0}, dest = {0};
    EduTest e = {0};
    int result = drv_pci_find(0x1234, 0x11e8, 0, &device);
    if (result == DRV_ENOENT) {
        append(out, "SKIP: EDU PCI fixture absent (use -device edu)\n", cap);
        return 0;
    }
    if (result)
        return result;
    drv_work_init(&e.work, edu_deferred, &e);
#define EDU_CHECK(expr, name)                                                                      \
    do {                                                                                           \
        bool ok = (expr);                                                                          \
        log_test(out, cap, name, ok);                                                              \
        if (!ok) {                                                                                 \
            result = DRV_EIO;                                                                      \
            goto cleanup;                                                                          \
        }                                                                                          \
    } while (0)
    EDU_CHECK(!drv_pci_claim(&device), "PCI claim");
    duplicate = device;
    duplicate.cookie = 0;
    EDU_CHECK(drv_pci_claim(&duplicate) == DRV_EBUSY && drv_pci_msi_enable(&device) == DRV_ENOTSUP,
              "exclusive device ownership / explicit MSI unsupported");
    EDU_CHECK(!drv_pci_map(&device, 0, &e.mmio) && !drv_pci_enable(&device, false, true),
              "PCI BAR probe / uncached supervisor MMIO");
    EDU_CHECK(drv_pci_release(&device) == DRV_EBUSY, "mapped device release refused");
    uint32_t value = 0;
    EDU_CHECK(!drv_pci_config_read(&device, 0, 2, &value) && value == 0x1234 &&
                  drv_pci_config_read(&device, 255, 2, &value) == DRV_EINVAL,
              "PCI configuration widths and bounds");
    EDU_CHECK(!drv_mmio_read(&e.mmio, 0, 4, &value) && (value & 255) == 0xed,
              "EDU register identification");
    EDU_CHECK(!drv_mmio_write(&e.mmio, 4, 4, 0xa5a51234) && !drv_mmio_read(&e.mmio, 4, 4, &value) &&
                  value == ~0xa5a51234u &&
                  drv_mmio_read(&e.mmio, e.mmio.size, 4, &value) == DRV_EINVAL &&
                  drv_mmio_read(&e.mmio, 1, 4, &value) == DRV_EINVAL,
              "MMIO liveness / bounds / alignment");
    EDU_CHECK(!drv_irq_request(&device, edu_unowned, &e, &first) &&
                  !drv_irq_request(&device, edu_irq, &e, &second),
              "shared PCI INTx registration");
    EDU_CHECK(!drv_mmio_write(&e.mmio, 0x60, 4, 1) && !drv_wait(&e.done, 1000) && e.irq_calls &&
                  e.shared_calls && e.irq_context && e.deferred_context && e.wait_rejected,
              "hardware interrupt / shared handlers / deferred work / IRQ sleep rejection");
    EDU_CHECK(!drv_dma_alloc(1024, 4096, 0x0fffffffu, 4096, &source) &&
                  !drv_dma_alloc(1024, 4096, 0x0fffffffu, 4096, &dest),
              "device DMA mask / separate buffers");
    for (unsigned i = 0; i < 1024; i++)
        ((uint8_t *)source.cpu)[i] = (i * 37u + 19u) & 255;
    EDU_CHECK(!drv_pci_master(&device, true), "explicit PCI bus mastering enable");
    EDU_CHECK(drv_mmio_unmap(&e.mmio) == DRV_EBUSY, "active DMA/IRQ mapping cannot be released");
    drv_memory_barrier();
    EDU_CHECK(!drv_mmio_write(&e.mmio, 0x80, 4, source.bus) &&
                  !drv_mmio_write(&e.mmio, 0x88, 4, 0x40000) &&
                  !drv_mmio_write(&e.mmio, 0x90, 4, 1024) && !drv_mmio_write(&e.mmio, 0x98, 4, 5) &&
                  !drv_wait(&e.done, 2000),
              "RAM to device DMA completed by hardware IRQ");
    EDU_CHECK(!drv_mmio_write(&e.mmio, 0x80, 4, 0x40000) &&
                  !drv_mmio_write(&e.mmio, 0x88, 4, dest.bus) &&
                  !drv_mmio_write(&e.mmio, 0x90, 4, 1024) && !drv_mmio_write(&e.mmio, 0x98, 4, 7) &&
                  !drv_wait(&e.done, 2000),
              "device to RAM DMA completed by hardware IRQ");
    drv_memory_barrier();
    EDU_CHECK(!memcmp(source.cpu, dest.cpu, 1024) && e.irq_calls >= 3,
              "DMA round trip binary payload");
    result = 0;
cleanup:
    if (device.cookie)
        drv_pci_master(&device, false);
    if (e.mmio.cookie)
        drv_mmio_write(&e.mmio, 0x64, 4, UINT32_MAX);
    if (second.cookie)
        drv_irq_free(&second);
    if (first.cookie)
        drv_irq_free(&first);
    drv_work_cancel(&e.work);
    if (dest.cookie)
        drv_dma_free(&dest);
    if (source.cookie)
        drv_dma_free(&source);
    if (e.mmio.cookie)
        drv_mmio_unmap(&e.mmio);
    if (device.cookie) {
        int r = drv_pci_release(&device);
        log_test(out, cap, "PCI IRQ/MMIO/DMA teardown", !r);
        if (r)
            result = r;
    }
    return result;
#undef EDU_CHECK
}
static void never_run(void *arg) {
    (void)arg;
}
static bool test_limits(void) {
    DrvDma pages[512] = {0};
    DrvWork work[65] = {0};
    DrvTimer timers[33] = {0};
    DrvPacketQueue queue = {0};
    DrvPacket *packets[65] = {0};
    size_t before = drv_dma_available();
    unsigned count = before / 4096;
    bool ok = count <= 512;
    for (unsigned i = 0; i < count && ok; i++)
        ok = !drv_dma_alloc(4096, 4096, UINT32_MAX, 0, &pages[i]);
    DrvDma extra = {0};
    if (ok)
        ok = drv_dma_alloc(1, 16, UINT32_MAX, 0, &extra) == DRV_ENOMEM;
    for (unsigned i = 0; i < 512; i++)
        if (pages[i].cookie)
            drv_dma_free(&pages[i]);
    if (extra.cookie)
        drv_dma_free(&extra);
    for (unsigned i = 0; i < 65; i++)
        drv_work_init(&work[i], never_run, 0);
    for (unsigned i = 0; i < 64 && ok; i++)
        ok = !drv_work_queue(&work[i]);
    if (ok)
        ok = drv_work_queue(&work[64]) == DRV_ENOSPC;
    for (unsigned i = 0; i < 65; i++)
        drv_work_cancel(&work[i]);
    for (unsigned i = 0; i < 33; i++)
        drv_timer_init(&timers[i], never_run, 0);
    for (unsigned i = 0; i < 32 && ok; i++)
        ok = !drv_timer_start(&timers[i], 60000);
    if (ok)
        ok = drv_timer_start(&timers[32], 60000) == DRV_ENOSPC;
    for (unsigned i = 0; i < 33; i++)
        drv_timer_cancel(&timers[i]);
    for (unsigned i = 0; i < 65 && ok; i++) {
        packets[i] = drv_packet_alloc(64, 16);
        ok = packets[i] != 0;
        if (ok && i < 64)
            ok = !drv_packet_enqueue(&queue, packets[i]);
    }
    if (ok)
        ok = drv_packet_enqueue(&queue, packets[64]) == DRV_ENOSPC;
    if (ok)
        for (unsigned i = 0; i < 64; i++)
            if (drv_packet_dequeue(&queue) != packets[i]) {
                ok = false;
                break;
            }
    /* Packets still queued belong to queue; dequeue before releasing each handle. */
    while (drv_packet_dequeue(&queue)) {
    }
    for (unsigned i = 0; i < 65; i++)
        if (packets[i])
            drv_packet_release(packets[i]);
    return ok && drv_dma_available() == before;
}
int driver_services_test(char *out, size_t cap) {
    out[0] = 0;
    size_t available = drv_dma_available();
    DrvDma a = {0}, b = {0}, stale = {0};
    DrvWork work = {0};
    DrvTimer timer = {0};
    WorkTest t = {0};
    DrvPacketQueue queue = {0};
    DrvPacket *packet = 0;
    DrvFirmware fw = {0};
    int result = DRV_EIO;
#define CHECK(expr, name)                                                                          \
    do {                                                                                           \
        bool ok = (expr);                                                                          \
        log_test(out, cap, name, ok);                                                              \
        if (!ok)                                                                                   \
            goto cleanup;                                                                          \
    } while (0)
    CHECK(available >= 1024 * 1024, "E820-backed driver memory available");
    CHECK(drv_dma_alloc(8, 3, UINT32_MAX, 0, &a) == DRV_EINVAL &&
              drv_dma_alloc(4096, 4096, 0xfffff, 0, &a) == DRV_ENOMEM,
          "invalid alignment / unreachable DMA mask rejected");
    CHECK(!drv_dma_alloc(8192, 65536, UINT32_MAX, 65536, &a) && !(a.bus & 65535) &&
              !drv_dma_alloc(8192, 4096, UINT32_MAX, 0, &b) && a.bus != b.bus,
          "DMA alignment / boundary / nonoverlap");
    CHECK(!memcmp(a.cpu, b.cpu, 8192), "new DMA buffers zero initialized");
    stale = a;
    CHECK(!drv_dma_free(&a) && drv_dma_free(&stale) == DRV_EINVAL, "double DMA free rejected");
    CHECK(!drv_dma_alloc(8192, 65536, UINT32_MAX, 65536, &a) && drv_dma_free(&stale) == DRV_EINVAL,
          "stale DMA allocation cookie rejected after reuse");
    drv_dma_free(&a);
    drv_dma_free(&b);
    CHECK(test_limits(),
          "DMA exhaustion / work and timer limits / packet queue saturation and FIFO");
    DrvLock lock = {0};
    uint32_t flags, again;
    CHECK(!drv_lock(&lock, &flags), "IRQ-save lock acquired");
    bool lock_ok = !drv_irqs_enabled() && drv_lock(&lock, &again) == DRV_EBUSY;
    DrvCompletion empty = {0};
    lock_ok &= drv_wait(&empty, 1) == DRV_EDEADLK;
    drv_unlock(&lock, flags);
    CHECK(lock_ok && drv_irqs_enabled(),
          "nested lock rejected / interrupt state restored / no sleeping with IRQs off");
    drv_work_init(&work, work_once, &t);
    CHECK(!drv_work_queue(&work) && drv_work_queue(&work) == DRV_EALREADY &&
              !drv_work_cancel(&work),
          "duplicate work / cancellation");
    driver_services_poll(2);
    CHECK(!t.count, "cancelled work never executed");
    drv_work_init(&t.work, work_repeat, &t);
    drv_work_queue(&t.work);
    driver_services_poll(1);
    CHECK(t.count == 1 && t.work.queued, "worker budget and self requeue");
    driver_services_poll(8);
    CHECK(t.count == 3 && !t.work.queued, "FIFO work drained exactly once per submission");
    t.count = 0;
    drv_timer_init(&timer, work_once, &t);
    uint32_t start = drv_now_ms();
    CHECK(!drv_timer_start(&timer, 7) && !drv_wait(&t.done, 1000) && drv_now_ms() - start >= 7 &&
              t.count == 1 && t.outside_irq,
          "timer deadline / completion / callback process context");
    CHECK(!drv_timer_start(&timer, 2) && !drv_timer_cancel(&timer) && !drv_sleep_ms(4) &&
              t.count == 1 && drv_wait(&empty, 3) == DRV_ETIMEDOUT,
          "timer cancellation / completion timeout");
    CHECK(!drv_udelay(50) && drv_udelay(1001) == DRV_EINVAL,
          "bounded calibrated microsecond delay");
    packet = drv_packet_alloc(256, 64);
    CHECK(packet, "packet allocated with DMA-capable payload");
    CHECK(drv_packet_put(packet, 20) && packet->length == 20 && drv_packet_push(packet, 14) &&
              packet->length == 34 && !drv_packet_push(packet, 51) && drv_packet_pull(packet, 14) &&
              !drv_packet_put(packet, 1000),
          "packet headroom / tailroom / push / pull bounds");
    CHECK(!drv_packet_ref(packet) && !drv_packet_put(packet, 1) && !drv_packet_release(packet),
          "shared packet mutation rejected / references");
    CHECK(!drv_packet_enqueue(&queue, packet) && drv_packet_enqueue(&queue, packet) == DRV_EBUSY &&
              drv_packet_release(packet) == DRV_EBUSY,
          "queue ownership / duplicate enqueue / queued free rejected");
    CHECK(drv_packet_dequeue(&queue) == packet && queue.count == 0,
          "packet FIFO ownership transfer");
    drv_packet_release(packet);
    packet = 0;
    CHECK(drv_firmware_request("../bad.bin", &fw) == DRV_EINVAL,
          "firmware path traversal rejected");
    int r = drv_firmware_request("luna-test/pattern.bin", &fw);
    if (r == DRV_ENOENT)
        append(out, "SKIP: optional binary firmware test fixture absent\n", cap);
    else {
        CHECK(!r && fw.size == 70000, "binary firmware larger than LunaFS limit loaded");
        bool valid = true;
        for (unsigned i = 0; i < fw.size; i++)
            if (fw.data[i] != (uint8_t)(i * 37u)) {
                valid = false;
                break;
            }
        CHECK(valid, "binary firmware preserves NUL bytes / checksum verified");
        drv_firmware_release(&fw);
    }
    CHECK(!test_edu(out, cap), "PCI fixture tests");
    CHECK(drv_dma_available() == available, "all driver allocations reclaimed");
    result = 0;
cleanup:
    drv_work_cancel(&work);
    drv_work_cancel(&t.work);
    drv_timer_cancel(&timer);
    if (packet && packet->queued)
        packet = 0;
    drv_packet_purge(&queue);
    if (packet && !packet->queued)
        drv_packet_release(packet);
    if (fw.allocation.cookie)
        drv_firmware_release(&fw);
    if (a.cookie)
        drv_dma_free(&a);
    if (b.cookie)
        drv_dma_free(&b);
    append(out, result ? "DRIVER SERVICES FAIL\n" : "DRIVER SERVICES PASS\n", cap);
    return result;
#undef CHECK
}
