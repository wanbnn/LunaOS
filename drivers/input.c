#include "luna.h"
#include "settings.h"
int mouse_x = 512, mouse_y = 384, mouse_buttons;
bool mouse_changed;
static struct {
    int x, y, buttons;
} pointers[64];
static unsigned pointer_read, pointer_write;
static int shift, caps, extended, packet_n, alt, ctrl;
static int fraction_x, fraction_y;
static int keys[64];
static unsigned key_read, key_write;
static void queue_key(int key) {
    unsigned next = (key_write + 1) % 64;
    if (next != key_read) {
        keys[key_write] =
            key | (alt ? MOD_ALT : 0) | (ctrl ? MOD_CTRL : 0) | (shift ? MOD_SHIFT : 0);
        key_write = next;
    }
}
static bool absolute_pointer;
static void vmcall(uint32_t command, uint32_t argument, uint32_t result[4]) {
    uint32_t a = 0x564d5868, b = argument, c = command, d = 0x5658;
    __asm__ volatile("inl %%dx, %%eax" : "+a"(a), "+b"(b), "+c"(c), "+d"(d) :: "memory");
    result[0] = a; result[1] = b; result[2] = c; result[3] = d;
}
static void queue_pointer(void) {
    unsigned last = (pointer_write + 63) % 64;
    if (pointer_write != pointer_read && pointers[last].buttons == mouse_buttons) {
        pointers[last].x = mouse_x; pointers[last].y = mouse_y;
    } else {
        unsigned next = (pointer_write + 1) % 64;
        if (next == pointer_read) pointer_read = (pointer_read + 1) % 64;
        pointers[pointer_write].x = mouse_x; pointers[pointer_write].y = mouse_y;
        pointers[pointer_write].buttons = mouse_buttons; pointer_write = next;
    }
    mouse_changed = true;
}
static void absolute_poll(void) {
    if (!absolute_pointer) return;
    uint32_t r[4];
    for (int i = 0; i < 64; i++) {
        vmcall(40, 0, r);
        if ((r[0] >> 16) || (r[0] & 65535) < 4) break;
        vmcall(39, 4, r);
        if (r[0] & 0x10000) continue;
        mouse_x = (r[1] & 65535) * (W - 1) / 65535;
        mouse_y = (r[2] & 65535) * (H - 1) / 65535;
        mouse_buttons = ((r[0] & 0x20) ? 1 : 0) | ((r[0] & 0x10) ? 2 : 0) | ((r[0] & 8) ? 4 : 0);
        if (prefs.left_handed) mouse_buttons = (mouse_buttons & 4) | ((mouse_buttons & 1) << 1) | ((mouse_buttons & 2) >> 1);
        queue_pointer();
    }
}
static uint8_t packet[3];
static const char normal[128] = {
    [2] = '1',  [3] = '2',  [4] = '3',   [5] = '4',  [6] = '5',   [7] = '6',  [8] = '7',
    [9] = '8',  [10] = '9', [11] = '0',  [12] = '-', [13] = '=',  [14] = 8,   [15] = 9,
    [16] = 'q', [17] = 'w', [18] = 'e',  [19] = 'r', [20] = 't',  [21] = 'y', [22] = 'u',
    [23] = 'i', [24] = 'o', [25] = 'p',  [26] = '[', [27] = ']',  [28] = 10,  [30] = 'a',
    [31] = 's', [32] = 'd', [33] = 'f',  [34] = 'g', [35] = 'h',  [36] = 'j', [37] = 'k',
    [38] = 'l', [39] = ';', [40] = '\'', [41] = '`', [43] = '\\', [44] = 'z', [45] = 'x',
    [46] = 'c', [47] = 'v', [48] = 'b',  [49] = 'n', [50] = 'm',  [51] = ',', [52] = '.',
    [53] = '/', [57] = ' '};
static const char shifted[128] = {
    [2] = '!',  [3] = '@',  [4] = '#',  [5] = '$',  [6] = '%',  [7] = '^',  [8] = '&',
    [9] = '*',  [10] = '(', [11] = ')', [12] = '_', [13] = '+', [26] = '{', [27] = '}',
    [39] = ':', [40] = '"', [41] = '~', [43] = '|', [51] = '<', [52] = '>', [53] = '?'};
static bool wait_write(void) {
    for (int i = 0; i < 100000; i++)
        if (!(inb(0x64) & 2))
            return true;
    return false;
}
static int wait_read(void) {
    for (int i = 0; i < 100000; i++)
        if (inb(0x64) & 1)
            return inb(0x60);
    return -1;
}
static void mouse_command(int c) {
    if (wait_write()) {
        outb(0x64, 0xd4);
        if (wait_write()) {
            outb(0x60, c);
            wait_read();
        }
    }
}
void input_init(void) {
    while (inb(0x64) & 1)
        inb(0x60);
    if (wait_write())
        outb(0x64, 0xa8);
    if (wait_write())
        outb(0x64, 0x20);
    int config = wait_read();
    if (config >= 0) {
        if (wait_write())
            outb(0x64, 0x60);
        if (wait_write())
            outb(0x60, (config | 0x40) & ~0x23);
    }
    mouse_command(0xf6);
    mouse_command(0xe6);
    mouse_command(0xf4);
    input_apply_settings();
    uint32_t r[4];
    vmcall(10, ~0x564d5868u, r);
    if (r[1] == 0x564d5868 && r[0] != 0xffffffffu) {
        vmcall(41, 0x45414552, r);
        vmcall(40, 0, r);
        if (!(r[0] >> 16) && (r[0] & 65535)) {
            vmcall(39, 1, r);
            if (r[0] == 0x3442554a) {
                vmcall(41, 0x53424152, r); absolute_pointer = true;
                serial_write("VMMouse absolute pointer enabled\n");
            }
        }
    }
}
void input_poll(void) {
    /* The absolute pointer is polled on its own deadline: the VMMouse
     * backdoor is a port read and the kernel wakes up every millisecond. */
    static uint32_t next_absolute;
    if (frame_clock >= next_absolute) {
        absolute_poll();
        next_absolute = frame_clock + 5;
    }
    for (int n = 0; n < 256; n++) {
        uint8_t st = inb(0x64);
        if (!(st & 1))
            break;
        uint8_t c = inb(0x60);
        if (st & 0x20) {
            if (absolute_pointer) continue;
            if (!packet_n && !(c & 8))
                continue;
            packet[packet_n++] = c;
            if (packet_n == 3) {
                packet_n = 0;
                /* PS/2 deltas are signed NINE-bit quantities: the sign lives
                 * in packet[0], not bit 7 of the movement byte. */
                int dx = packet[1] - ((packet[0] & 0x10) ? 256 : 0);
                int dy = packet[2] - ((packet[0] & 0x20) ? 256 : 0);
                if (packet[0] & 0x40)
                    dx = (packet[0] & 0x10) ? -255 : 255;
                if (packet[0] & 0x80)
                    dy = (packet[0] & 0x20) ? -255 : 255;
                fraction_x += dx * prefs.mouse_speed;
                fraction_y += dy * prefs.mouse_speed;
                mouse_x += fraction_x / 100;
                mouse_y -= fraction_y / 100;
                fraction_x %= 100;
                fraction_y %= 100;
                if (mouse_x < 0)
                    mouse_x = 0;
                if (mouse_y < 0)
                    mouse_y = 0;
                if (mouse_x >= W)
                    mouse_x = W - 1;
                if (mouse_y >= H)
                    mouse_y = H - 1;
                mouse_buttons = packet[0] & 7;
                if (prefs.left_handed)
                    mouse_buttons = (mouse_buttons & 4) | ((mouse_buttons & 1) << 1) |
                                    ((mouse_buttons & 2) >> 1);
                unsigned last = (pointer_write + 63) % 64;
                if (pointer_write != pointer_read && pointers[last].buttons == mouse_buttons) {
                    pointers[last].x = mouse_x;
                    pointers[last].y = mouse_y;
                    mouse_changed = true;
                    continue;
                }
                unsigned next = (pointer_write + 1) % 64;
                if (next == pointer_read)
                    pointer_read = (pointer_read + 1) % 64;
                pointers[pointer_write].x = mouse_x;
                pointers[pointer_write].y = mouse_y;
                pointers[pointer_write].buttons = mouse_buttons;
                pointer_write = next;
                mouse_changed = true;
            }
            continue;
        }
        if (c == 0xe0) {
            extended = 1;
            continue;
        }
        if ((c & 127) == 56) {
            bool was = alt;
            alt = !(c & 128);
            extended = 0;
            if (was && !alt)
                queue_key(KEY_ALT_RELEASE);
            continue;
        }
        if ((c & 127) == 29) {
            ctrl = !(c & 128);
            extended = 0;
            continue;
        }
        if (extended && ((c & 127) == 91 || (c & 127) == 92)) {
            extended = 0;
            if (!(c & 128))
                queue_key(KEY_SUPER);
            continue;
        }
        if (c == 42 || c == 54) {
            shift = 1;
            continue;
        }
        if (c == 170 || c == 182) {
            shift = 0;
            continue;
        }
        if (c & 128) {
            extended = 0;
            continue;
        }
        if (c == 58) {
            caps = !caps;
            continue;
        }
        if (c == 1) {
            queue_key(27);
            continue;
        }
        if (c >= 59 && c <= 64) {
            queue_key(KEY_F1 + c - 59);
            continue;
        }
        if (extended) {
            extended = 0;
            if (c == 72)
                queue_key(KEY_UP);
            if (c == 80)
                queue_key(KEY_DOWN);
            if (c == 75)
                queue_key(KEY_LEFT);
            if (c == 77)
                queue_key(KEY_RIGHT);
            if (c == 71)
                queue_key(KEY_HOME);
            if (c == 79)
                queue_key(KEY_END);
            if (c == 83)
                queue_key(KEY_DELETE);
            continue;
        }
        int a = normal[c];
        if (prefs.keyboard_layout) {
            if (c == 39)
                a = shift ? 0xc7 : 0xe7;
            if (c == 26)
                a = shift ? '`' : 0xb4;
            if (c == 40)
                a = shift ? '^' : '~';
        }
        if (a >= 'a' && a <= 'z') {
            if (shift ^ caps)
                a -= 32;
        } else if (shift && shifted[c])
            a = shifted[c];
        if (a)
            queue_key(a);
    }
}
int input_key(void) {
    if (key_read == key_write)
        return -1;
    int key = keys[key_read];
    key_read = (key_read + 1) % 64;
    return key;
}

bool input_pointer(int *x, int *y, int *buttons) {
    if (pointer_read == pointer_write)
        return false;
    *x = pointers[pointer_read].x;
    *y = pointers[pointer_read].y;
    *buttons = pointers[pointer_read].buttons;
    pointer_read = (pointer_read + 1) % 64;
    return true;
}

void input_apply_settings(void) {
    static int delay = -1, rate = -1;
    if (delay == prefs.repeat_delay && rate == prefs.repeat_rate)
        return;
    delay = prefs.repeat_delay;
    rate = prefs.repeat_rate;
    /* PS/2 typematic: two delay bits and the hardware repeat divisor. ACK bytes
     * are ignored by the scancode decoder, preserving queued input packets. */
    if (wait_write())
        outb(0x60, 0xf3);
    if (wait_write()) {
        unsigned delay = (prefs.repeat_delay - 250) / 250;
        outb(0x60, (delay << 5) | (prefs.repeat_rate <= 15 ? 0x08 : 0));
    }
}
