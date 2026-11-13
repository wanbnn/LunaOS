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
    int dir = luna_opendir("/tmp");
    if (dir < 3) return 93;
    LunaGuestDirent entry = {0};
    int found = 0, result;
    while ((result = luna_readdir(dir, &entry)) == 1) {
        static const char expected[] = "guest-c-after.txt";
        unsigned i = 0;
        while (expected[i] && entry.name[i] == expected[i]) i++;
        if (!expected[i] && !entry.name[i]) {
            if (entry.mode != 0100755u || entry.mtime != 1735689600u)
                return 94;
            found = 1;
        }
    }
    if (result < 0 || !found || close(dir)) return 95;
    static const char ok[] = "LUNA FS3 METADATA REBOOT OK\n";
    return write(1, ok, sizeof(ok)-1u) == (long)(sizeof(ok)-1u) ? 0 : 92;
}
