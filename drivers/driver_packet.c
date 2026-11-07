#include "driver.h"
DrvPacket *drv_packet_alloc(unsigned payload, unsigned headroom) {
    if (payload > DRV_PACKET_CAP || headroom > DRV_PACKET_CAP - payload)
        return 0;
    DrvPacket *p = drv_alloc(sizeof(*p));
    if (!p)
        return 0;
    unsigned bytes = payload + headroom;
    if (drv_dma_alloc(bytes ? bytes : 1, 16, UINT32_MAX, 0, &p->allocation)) {
        drv_free(p);
        return 0;
    }
    p->storage = p->allocation.cpu;
    p->capacity = bytes;
    p->offset = headroom;
    p->refs = 1;
    return p;
}
int drv_packet_ref(DrvPacket *p) {
    if (!p)
        return DRV_EINVAL;
    uint32_t flags = drv_irq_save();
    if (!p->refs || p->refs == UINT32_MAX || p->queued) {
        drv_irq_restore(flags);
        return DRV_EBUSY;
    }
    p->refs++;
    drv_irq_restore(flags);
    return 0;
}
int drv_packet_release(DrvPacket *p) {
    if (!p)
        return DRV_EINVAL;
    uint32_t flags = drv_irq_save();
    if (!p->refs || p->queued) {
        drv_irq_restore(flags);
        return DRV_EBUSY;
    }
    if (--p->refs) {
        drv_irq_restore(flags);
        return 0;
    }
    int r = drv_dma_free(&p->allocation);
    if (!r)
        r = drv_free(p);
    drv_irq_restore(flags);
    return r;
}
/* Mutation requires a uniquely owned packet outside any queue. */
static bool writable(DrvPacket *p) {
    return p && p->refs == 1 && !p->queued;
}
void *drv_packet_put(DrvPacket *p, unsigned n) {
    if (!writable(p) || n > p->capacity - p->offset - p->length)
        return 0;
    void *result = p->storage + p->offset + p->length;
    p->length += n;
    return result;
}
void *drv_packet_push(DrvPacket *p, unsigned n) {
    if (!writable(p) || n > p->offset)
        return 0;
    p->offset -= n;
    p->length += n;
    return p->storage + p->offset;
}
void *drv_packet_pull(DrvPacket *p, unsigned n) {
    if (!writable(p) || n > p->length)
        return 0;
    p->offset += n;
    p->length -= n;
    return p->storage + p->offset;
}
int drv_packet_enqueue(DrvPacketQueue *q, DrvPacket *p) {
    if (!q || !p)
        return DRV_EINVAL;
    uint32_t flags = drv_irq_save();
    if (p->refs != 1 || p->queued || p->next) {
        drv_irq_restore(flags);
        return DRV_EBUSY;
    }
    if (q->count == DRV_PACKET_QUEUE_MAX) {
        drv_irq_restore(flags);
        return DRV_ENOSPC;
    }
    p->queued = true;
    if (q->tail)
        q->tail->next = p;
    else
        q->head = p;
    q->tail = p;
    q->count++;
    drv_irq_restore(flags);
    return 0;
}
DrvPacket *drv_packet_dequeue(DrvPacketQueue *q) {
    if (!q)
        return 0;
    uint32_t flags = drv_irq_save();
    DrvPacket *p = q->head;
    if (p) {
        q->head = p->next;
        if (!q->head)
            q->tail = 0;
        q->count--;
        p->next = 0;
        p->queued = false;
    }
    drv_irq_restore(flags);
    return p;
}
void drv_packet_purge(DrvPacketQueue *q) {
    DrvPacket *p;
    while ((p = drv_packet_dequeue(q)))
        drv_packet_release(p);
}
