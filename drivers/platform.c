#include "luna.h"
#include "task.h"
volatile uint32_t ticks, frame_clock;
uint32_t memory_mb;
static uint32_t beep_end;
void serial_init(void) {
    outb(0x3f9, 0);
    outb(0x3fb, 0x80);
    outb(0x3f8, 1);
    outb(0x3f9, 0);
    outb(0x3fb, 3);
    outb(0x3fa, 0xc7);
    outb(0x3fc, 0x0b);
}
void serial_put(char c) {
    for (int i = 0; i < 100000; i++)
        if (inb(0x3fd) & 32) {
            outb(0x3f8, c);
            return;
        }
}
void serial_write(const char *s) {
    while (*s) {
        if (*s == '\n')
            serial_put('\r');
        serial_put(*s++);
    }
}
int serial_get(void) {
    return (inb(0x3fd) & 1) ? inb(0x3f8) : -1;
}
struct Gate {
    uint16_t low, selector;
    uint8_t zero, flags;
    uint16_t high;
} __attribute__((packed));
static struct Gate idt[256];
extern void *isr_table[];
extern void isr_128(void), isr_129(void);
void timer_interrupt(void) {
    frame_clock++;
    if (frame_clock % 10 == 0)
        ticks++;
    outb(0x20, 0x20);
}
void kernel_fault(void) {
    serial_write("\nFATAL: CPU exception; kernel halted.\n");
    for (;;)
        __asm__ volatile("cli; hlt");
}
void platform_init(void) {
    for (int i = 0; i < 256; i++) {
        uint32_t a = (uint32_t)isr_table[0];
        idt[i] = (struct Gate){a & 65535, 8, 0, 0x8e, a >> 16};
    }
    for (int i = 0; i < 48; i++) {
        uint32_t a = (uint32_t)isr_table[i];
        idt[i] = (struct Gate){a & 65535, 8, 0, 0x8e, a >> 16};
    }
    uint32_t a = (uint32_t)isr_128;
    idt[128] = (struct Gate){a & 65535, 8, 0, 0xee, a >> 16};
    a = (uint32_t)isr_129;
    idt[129] = (struct Gate){a & 65535, 8, 0, 0x8e, a >> 16};
    struct {
        uint16_t limit;
        uint32_t base;
    } __attribute__((packed)) p = {sizeof(idt) - 1, (uint32_t)idt};
    __asm__ volatile("lidt %0" ::"m"(p));
    outb(0x20, 0x11);
    outb(0xa0, 0x11);
    outb(0x21, 0x20);
    outb(0xa1, 0x28);
    outb(0x21, 4);
    outb(0xa1, 2);
    outb(0x21, 1);
    outb(0xa1, 1);
    outb(0x21, 0xfe);
    outb(0xa1, 0xff);
    outb(0x43, 0x36);
    outb(0x40, 1193 & 255);
    outb(0x40, 1193 >> 8);
    uint32_t count = *(volatile uint32_t *)0x5000;
    uint64_t total = 0;
    for (uint32_t i = 0; i < count && i < 64; i++) {
        uint8_t *e = (uint8_t *)(0x5004 + i * 24);
        if (*(uint32_t *)(e + 16) == 1)
            total += *(uint64_t *)(e + 8);
    }
    memory_mb = (uint32_t)(total >> 20);
    outb(0x70, 0); /* IDT installed: re-enable NMIs after firmware transition. */
    __asm__ volatile("sti");
}
void sleep_ticks(uint32_t n) {
    task_sleep(n);
}
void speaker_beep(unsigned hz, unsigned duration) {
    if (hz < 20 || hz > 20000)
        return;
    unsigned div = 1193182 / hz;
    outb(0x43, 0xb6);
    outb(0x42, div);
    outb(0x42, div >> 8);
    outb(0x61, inb(0x61) | 3);
    beep_end = ticks + duration;
}
void speaker_poll(void) {
    if (beep_end && (int32_t)(ticks - beep_end) >= 0) {
        outb(0x61, inb(0x61) & ~3);
        beep_end = 0;
    }
}
void reboot(void) {
    for (int i = 0; i < 100000 && inb(0x64) & 2; i++)
        ;
    outb(0x64, 0xfe);
}
void poweroff(void) {
    outw(0x604, 0x2000);
    outw(0xb004, 0x2000);
    outw(0x4004, 0x3400);
}
static uint8_t cmos(int r) {
    outb(0x70, r);
    return inb(0x71);
}
static int bcd(int x) {
    return (x >> 4) * 10 + (x & 15);
}
void rtc_string(char *s) {
    for (int i = 0; i < 100000 && (cmos(0xa) & 0x80); i++)
        ;
    int h = cmos(4), m = cmos(2), d = cmos(7), mo = cmos(8), y = cmos(9);
    if (!(cmos(0xb) & 4)) {
        h = bcd(h & 0x7f);
        m = bcd(m);
        d = bcd(d);
        mo = bcd(mo);
        y = bcd(y);
    }
    char b[16];
    strcpy(s, "20");
    number(b, y);
    if (y < 10)
        strcat(s, "0");
    strcat(s, b);
    strcat(s, "-");
    if (mo < 10)
        strcat(s, "0");
    number(b, mo);
    strcat(s, b);
    strcat(s, "-");
    if (d < 10)
        strcat(s, "0");
    number(b, d);
    strcat(s, b);
    strcat(s, "  ");
    if (h < 10)
        strcat(s, "0");
    number(b, h);
    strcat(s, b);
    strcat(s, ":");
    if (m < 10)
        strcat(s, "0");
    number(b, m);
    strcat(s, b);
}
