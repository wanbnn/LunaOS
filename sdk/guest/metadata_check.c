#include "posix.h"
/* Run after reboot: metadata must come from LunaFS v3 snapshot on disk, not RAM. */
int main(void) {
    LunaGuestStat st = {0};
    if (luna_stat("/tmp/guest-c-after.txt", &st))
        return 90;
    if (st.size != sizeof("C SDK FILE WRITE OK\n") - 1u ||
        st.mode != 0100755u || st.mtime != 1735689600u ||
        !st.ctime)
        return 91;
    static const char ok[] = "LUNA FS3 METADATA REBOOT OK\n";
    return write(1, ok, sizeof(ok)-1u) == (long)(sizeof(ok)-1u) ? 0 : 92;
}
