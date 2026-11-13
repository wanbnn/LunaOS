#include "posix.h"

/* Native guest-to-host TCP echo. Endpoint must be the QEMU 10.0.2.2 host.
 * This proves real ring-3 TCP I/O; it is not a BSD sockets implementation. */
static unsigned parse_port(const char *s) {
    unsigned n=0;
    for (;*s;s++) {
        if (*s<'0' || *s>'9') return 0;
        n=n*10u + (unsigned)(*s-'0');
        if (n>65535u) return 0;
    }
    return n;
}
int main(int argc, char **argv, char **envp) {
    (void)envp;
    if (argc != 2) return 101;
    unsigned port = parse_port(argv[1]);
    if (!port) return 102;
    int fd = luna_tcp_connect("10.0.2.2", port, 0);
    if (fd < 3) return 103;
    /* Single global stream must reject a second guest connection. */
    if (luna_tcp_connect("10.0.2.2", port, 0) >= 0)
        return 104;
    static const char request[] = "PING LUNA TCP\n";
    if (write(fd, request, sizeof(request)-1u) != (long)(sizeof(request)-1u))
        return 105;
    char response[64] = {0};
    long n = read(fd, response, sizeof(response));
    if (close(fd) || n != (long)(sizeof("PONG LUNA TCP\n")-1u))
        return 106;
    static const char expected[] = "PONG LUNA TCP\n";
    for (unsigned i=0;i<sizeof(expected)-1u;i++)
        if (response[i]!=expected[i])
            return 107;
    static const char marker[] = "LUNA NATIVE TCP GUEST OK\n";
    return write(1, marker, sizeof(marker)-1u)==(long)(sizeof(marker)-1u) ? 0 : 108;
}
