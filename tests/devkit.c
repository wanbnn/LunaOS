#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/devkit.h"
static int hardware_read(void *ctx, char *out, size_t cap) {
    const char *value = (const char *)ctx;
    if (strlen(value) >= cap) return -28;
    strcpy(out, value);
    return 0;
}
static int hardware_write(void *ctx, const char *value) {
    char *dest = ctx;
    if (strlen(value) > 15) return -22;
    strcpy(dest, value);
    return 0;
}
int main(void) {
    char b[512];
    assert(ldev_valid_name("demo_01"));
    assert(!ldev_valid_name("bad/path"));
    assert(!ldev_valid_name(""));
    assert(ldev_load("LDEV1\nname=state\ntype=register\nvalue=ready\n") == 0);
    assert(ldev_read("state", b, sizeof(b)) == 0 && !strcmp(b, "ready"));
    assert(ldev_write("state", "updated") == 0);
    assert(ldev_read("state", b, sizeof(b)) == 0 && !strcmp(b, "updated"));
    assert(ldev_load("LDEV1\nname=state\ntype=register\n") != 0);
    assert(ldev_load("LDEV1\nname=upper\ntype=upper\n") == 0);
    assert(ldev_write("upper", "Hello Luna") == 0);
    assert(ldev_read("upper", b, sizeof(b)) == 0 && !strcmp(b, "HELLO LUNA"));
    assert(ldev_load("LDEV1\nname=count\ntype=counter\n") == 0);
    assert(ldev_write("count", "inc") == 0);
    assert(ldev_write("count", "inc") == 0);
    assert(ldev_read("count", b, sizeof(b)) == 0 && !strcmp(b, "2"));
    assert(ldev_write("count", "dec") == 0);
    assert(ldev_write("count", "reset") == 0);
    assert(ldev_read("count", b, sizeof(b)) == 0 && !strcmp(b, "0"));
    assert(ldev_write("count", "oops") == -22);
    assert(ldev_load("LDEV1\nname=bad\ntype=counter\nvalue=100\n") == -22);
    assert(ldev_load("LDEV1\nname=bad\ntype=bad\n") == -22);
    assert(ldev_load("LDEV1\nname=bad/name\ntype=upper\n") == -22);
    assert(ldev_load("LDEV1\nname=x\ntype=upper\nname=y\n") == -22);
    assert(ldev_load("LDEV1\nname=x\ntype=upper\njunk=yes\n") == -22);
    assert(ldev_read("upper", b, 2) == -28);
    ldev_list(b, sizeof(b));
    assert(strstr(b, "state  register"));
    assert(strstr(b, "count  counter"));
    assert(ldev_unload("upper") == 0 && ldev_read("upper", b, sizeof(b)) == -2);
    assert(ldev_unload("upper") == -2);
    char device_state[16] = "physical";
    assert(!ldev_native_register("real-callback", hardware_read,
                                 hardware_write, device_state));
    assert(!ldev_read("real-callback", b, sizeof(b)) && !strcmp(b, "physical"));
    assert(!ldev_write("real-callback", "updated"));
    assert(!ldev_read("real-callback", b, sizeof(b)) && !strcmp(b, "updated"));
    assert(ldev_unload("real-callback") == -95);
    assert(ldev_native_register("real-callback", hardware_read, 0, 0) == -22);
    ldev_list(b, sizeof(b));
    assert(strstr(b, "real-callback  hardware"));
    puts("PASS: LunaSDK virtual driver lifecycle, native C endpoints and malformed descriptors");
    return 0;
}
