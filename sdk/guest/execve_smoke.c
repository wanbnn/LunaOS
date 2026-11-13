#include "posix.h"

/* Demonstrates POSIX-like in-place execve semantics:
 * - failed replacement leaves the original program running
 * - successful replacement never returns
 * - PID and ordinary open file descriptors survive.
 * This is still LunaOS's restricted statically linked ELF32 ABI. */
static char expected_pid[16];
static void encode_pid(int pid) {
    char reversed[16];
    unsigned n = 0;
    while (pid > 0 && n < sizeof(reversed)-1) {
        reversed[n++] = (char)('0' + pid % 10);
        pid /= 10;
    }
    unsigned i = 0;
    while (n)
        expected_pid[i++] = reversed[--n];
    expected_pid[i] = 0;
}
static int decode_pid(const char *text) {
    int n = 0;
    for (; *text; text++) {
        if (*text < '0' || *text > '9')
            return -1;
        n = n * 10 + *text - '0';
    }
    return n;
}
int main(int argc, char **argv, char **envp) {
    if (argc == 3 && argv[1] && argv[1][0] == 'R') {
        if (decode_pid(argv[2]) != getpid())
            return 81;
        static const char text[] = "EXECVE SAME PID FD OK\n";
        /* The original caller opened fd=3 and did not close it. */
        if (write(3, text, sizeof(text)-1u) != (long)(sizeof(text)-1u))
            return 82;
        if (fsync(3) || close(3))
            return 83;
        if (write(1, text, sizeof(text)-1u) != (long)(sizeof(text)-1u))
            return 84;
        return 0;
    }
    const char *args[] = {"/tmp/execve.elf", "Replacement", expected_pid, 0};
    if (luna_execve("/tmp/this-image-does-not-exist", args, (const char *const *)envp) != -1)
        return 85;
    int fd = open("/tmp/execve-fd.txt", LUNA_O_WRONLY | LUNA_O_CREAT | LUNA_O_TRUNC);
    if (fd != 3)
        return 86;
    encode_pid(getpid());
    (void)luna_execve("/tmp/execve.elf", args, (const char *const *)envp);
    return 87; /* must never execute if successful */
}
