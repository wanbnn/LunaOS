#include "posix.h"

/* Freestanding C guest test — argv/env + RX/RW pages and LunaFS2 writes.
 * This is a real C executable, not a mocked or pre-rendered shell command. */
volatile int luna_initialized_data = 77;  /* ensure RW PT_LOAD */
static char read_buffer[64];              /* BSS */
static const char marker[] = "LUNA GUEST C POSIX OK\n";
static const char payload[] = "C SDK FILE WRITE OK\n";
int main(int argc, char **argv, char **envp) {
    if (argc != 3 || !argv || !argv[1] || argv[1][0] != 'A' ||
        !envp || !envp[0] || envp[0][0] != 'H')
        return 40;
    if (luna_initialized_data != 77)
        return 41;
    luna_initialized_data = 78;
    if (write(1, marker, sizeof(marker) - 1u) != (long)(sizeof(marker) - 1u))
        return 42;
    int fd = open("/home/luna/welcome.txt", LUNA_O_RDONLY);
    if (fd < 0)
        return 43;
    long n = read(fd, read_buffer, sizeof(read_buffer));
    if (n < 10 || read_buffer[0] != 'W' || close(fd))
        return 44;
    fd = open("/tmp/guest-c-before.txt", LUNA_O_WRONLY | LUNA_O_CREAT | LUNA_O_TRUNC);
    if (fd < 0)
        return 45;
    if (write(fd, payload, sizeof(payload) - 1u) != (long)(sizeof(payload) - 1u) ||
        close(fd))
        return 46;
    if (rename("/tmp/guest-c-before.txt", "/tmp/guest-c-after.txt"))
        return 47;
    LunaGuestStat st;
    if (luna_stat("/tmp/guest-c-after.txt", &st) || st.size != sizeof(payload) - 1u)
        return 48;
    char cwd[128];
    if (!getcwd(cwd, sizeof(cwd)) || cwd[0] != '/')
        return 49;
    if (getpid() <= 0)
        return 50;
    return 0;
}
