#include "devkit.h"
#include "luna.h"

/* Bounded, in-memory virtual device registry. No heap, PCI, MMIO or IRQ.
 * These devices are software endpoints, NOT dynamically loaded kernel code. */
typedef struct {
    char name[25], data[128];
    unsigned count;
    unsigned char kind;
    LunaNativeRead read;
    LunaNativeWrite write;
    void *context;
} VirtualDevice;
static VirtualDevice devices[LDEV_MAX];

int ldev_valid_name(const char *name) {
    size_t n = name ? strlen(name) : 0;
    if (!n || n > 24)
        return 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}
static int find_device(const char *name) {
    if (!name)
        return -1;
    for (int i = 0; i < LDEV_MAX; i++)
        if (devices[i].kind && !strcmp(devices[i].name, name))
            return i;
    return -1;
}
/* Reject unknown fields, duplicates, invalid names, oversized data and
 * malformed lines so a user-edited .ldrv cannot become an unsafe module. */
int ldev_load(const char *descriptor) {
    if (!descriptor || strlen(descriptor) > 512 || strncmp(descriptor, "LDEV1\n", 6))
        return -22;
    char name[25] = "", type[16] = "", value[128] = "";
    int saw_name = 0, saw_type = 0, saw_value = 0;
    const char *p = descriptor + 6;
    while (*p) {
        char key[16], val[128];
        size_t k = 0, n = 0;
        while (*p && *p != '=' && *p != '\n') {
            if (k >= sizeof(key) - 1) return -22;
            key[k++] = *p++;
        }
        if (*p++ != '=') return -22;
        key[k] = 0;
        while (*p && *p != '\n') {
            if (n >= sizeof(val) - 1) return -22;
            val[n++] = *p++;
        }
        val[n] = 0;
        if (*p == '\n') p++;
        if (!strcmp(key, "name")) {
            if (saw_name++ || !ldev_valid_name(val)) return -22;
            strcpy(name, val);
        } else if (!strcmp(key, "type")) {
            if (saw_type++ || (strcmp(val, "register") && strcmp(val, "upper") &&
                               strcmp(val, "counter"))) return -22;
            strcpy(type, val);
        } else if (!strcmp(key, "value")) {
            if (saw_value++) return -22;
            strcpy(value, val);
        } else return -22;
    }
    if (!saw_name || !saw_type || find_device(name) >= 0) return -22;
    int free_slot = -1;
    for (int i = 0; i < LDEV_MAX; i++)
        if (!devices[i].kind) { free_slot = i; break; }
    if (free_slot < 0) return -28;
    VirtualDevice *d = &devices[free_slot];
    memset(d, 0, sizeof(*d));
    strcpy(d->name, name);
    d->kind = !strcmp(type, "register") ? 1 : !strcmp(type, "upper") ? 2 : 3;
    if (d->kind == 3 && saw_value && strcmp(value, "0")) {
        memset(d, 0, sizeof(*d));
        return -22;
    }
    if (d->kind != 3)
        strcpy(d->data, value);
    return 0;
}
int ldev_native_register(const char *name, LunaNativeRead read,
                         LunaNativeWrite write, void *ctx) {
    if (!ldev_valid_name(name) || !read || find_device(name) >= 0)
        return -22;
    for (int i = 0; i < LDEV_MAX; i++)
        if (!devices[i].kind) {
            memset(&devices[i], 0, sizeof(devices[i]));
            strcpy(devices[i].name, name);
            devices[i].read = read;
            devices[i].write = write;
            devices[i].context = ctx;
            devices[i].kind = 4;
            return 0;
        }
    return -28;
}
int ldev_unload(const char *name) {
    int i = find_device(name);
    if (i < 0) return -2;
    if (devices[i].kind == 4) return -95; /* cannot detach a running PCI driver */
    memset(&devices[i], 0, sizeof(devices[i]));
    return 0;
}
static void decimal(char *out, unsigned n) {
    char backwards[16];
    unsigned i = 0, j = 0;
    do { backwards[i++] = (char)('0' + n % 10); n /= 10; } while (n);
    while (i) out[j++] = backwards[--i];
    out[j] = 0;
}
int ldev_read(const char *name, char *out, size_t cap) {
    int i = find_device(name);
    if (i < 0) return -2;
    if (!out || !cap) return -22;
    if (devices[i].kind == 4)
        return devices[i].read(devices[i].context, out, cap);
    char num[16];
    const char *s = devices[i].data;
    if (devices[i].kind == 3) {
        decimal(num, devices[i].count);
        s = num;
    }
    if (strlen(s) >= cap) return -28;
    strcpy(out, s);
    return 0;
}
int ldev_write(const char *name, const char *value) {
    int i = find_device(name);
    if (i < 0) return -2;
    if (!value || strlen(value) >= sizeof(devices[i].data)) return -22;
    VirtualDevice *d = &devices[i];
    if (d->kind == 4)
        return d->write ? d->write(d->context, value) : -95;
    if (d->kind == 3) {
        if (!strcmp(value, "inc")) {
            if (d->count == UINT32_MAX) return -28;
            d->count++;
        } else if (!strcmp(value, "dec")) {
            if (!d->count) return -22;
            d->count--;
        } else if (!strcmp(value, "reset")) d->count = 0;
        else return -22;
        return 0;
    }
    strcpy(d->data, value);
    if (d->kind == 2)
        for (char *p = d->data; *p; p++)
            if (*p >= 'a' && *p <= 'z') *p -= 'a' - 'A';
    return 0;
}
static void add(char *out, size_t cap, const char *s) {
    size_t n = strlen(out), remaining = cap > n ? cap - n : 0;
    if (!remaining) return;
    while (*s && remaining > 1) { out[n++] = *s++; remaining--; }
    out[n] = 0;
}
void ldev_list(char *out, size_t cap) {
    if (!out || !cap) return;
    out[0] = 0;
    for (int i = 0; i < LDEV_MAX; i++) if (devices[i].kind) {
        add(out, cap, devices[i].name);
        add(out, cap, devices[i].kind == 1 ? "  register\n" :
                       devices[i].kind == 2 ? "  upper\n" :
                       devices[i].kind == 3 ? "  counter\n" : "  hardware\n");
    }
}
