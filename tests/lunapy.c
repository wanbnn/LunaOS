#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/lunasdk_hardware.h"
int lunapy_eval_sdk(const char *, char *, size_t);
static unsigned sound;
int lunasdk_pci_count(unsigned *n) { *n = 1; return 0; }
int lunasdk_pci_get(unsigned i, LunaPciInfo *d) {
    if (i) return -2;
    *d = (LunaPciInfo){.bus=0,.slot=2,.function=0,.vendor=0x1234,.device=0x11e8};
    return 0;
}
int lunasdk_pci_read(unsigned b,unsigned s,unsigned f,unsigned o,
                      unsigned w,uint32_t *value) {
    if (b || s!=2 || f || o || w!=4) return -22;
    *value=0x11e81234;
    return 0;
}
int ldev_read(const char *name,char *out,size_t cap) {
    if (strcmp(name,"demo") || cap < 6) return -2;
    strcpy(out,"hello"); return 0;
}
int ldev_write(const char *name,const char *value) {
    return strcmp(name,"demo") || strcmp(value,"ready") ? -22 : 0;
}
void audio_beep(unsigned hz,unsigned ms) { if (hz == 660 && ms == 25) sound++; }
void append(char *out, const char *s, size_t cap) {
    size_t n = strlen(out), m = strlen(s);
    if (n + m >= cap) m = cap-n-1;
    memcpy(out+n,s,m); out[n+m]=0;
}
int main(void) {
    char b[1024];
    assert(!lunapy_eval_sdk(
        "# test\nprint('Hello')\nprint(luna.hardware.pci.count())\n"
        "print(luna.hardware.pci.get(0))\n"
        "print(luna.hardware.pci.read(0,2,0,0,4))\n"
        "print(luna.device.read('demo'))\n"
        "print(luna.device.write('demo','ready'))\n"
        "luna.audio.beep(660,25)\n", b,sizeof(b)));
    assert(strstr(b,"Hello\n1\nPCI 0:2.0 vendor=4660 device=4584 class=0\n"));
    assert(strstr(b,"hello\nTrue\n"));
    assert(sound==1);
    assert(lunapy_eval_sdk("for i in range(5):\n  print(i)\n",b,sizeof(b)) == -1);
    assert(strstr(b,"not full Python"));
    assert(lunapy_eval_sdk("print(luna.audio.beep(10,1))",b,sizeof(b)) == -1);
    puts("PASS: LunaPy preview PCI, native device API, audio and explicit unsupported syntax");
}
