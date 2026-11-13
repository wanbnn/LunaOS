#include "posix.h"

/* Use native i386 int $0x80, not Linux's syscall table.
 * No errno translation yet: negative return value means failure. */
static long call3(unsigned sys, unsigned a, unsigned b, unsigned c) {
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(sys), "b"(a), "c"(b), "d"(c) : "memory", "cc");
    return ret;
}
long write(int fd, const void *buffer, size_t n) {
    return call3(6, (unsigned)fd, (unsigned)(uintptr_t)buffer, (unsigned)n);
}
long read(int fd, void *buffer, size_t n) {
    return call3(8, (unsigned)fd, (unsigned)(uintptr_t)buffer, (unsigned)n);
}
int open(const char *path, int flags, ...) {
    return (int)call3(7, (unsigned)(uintptr_t)path, (unsigned)flags, 0);
}
int close(int fd) { return (int)call3(9, (unsigned)fd, 0, 0); }
long lseek(int fd, long offset, int whence) {
    return call3(10, (unsigned)fd, (unsigned)offset, (unsigned)whence);
}
int luna_stat(const char *path, LunaGuestStat *info) {
    return (int)call3(11, (unsigned)(uintptr_t)path, (unsigned)(uintptr_t)info, 0);
}
int chdir(const char *path) { return (int)call3(12, (unsigned)(uintptr_t)path, 0, 0); }
char *getcwd(char *buffer, size_t capacity) {
    long rc = call3(13, (unsigned)(uintptr_t)buffer, (unsigned)capacity, 0);
    return rc == -1 ? (char *)0 : buffer;
}
int mkdir(const char *path, unsigned mode) {
    (void)mode; /* LunaFS2 does not implement POSIX permissions. */
    return (int)call3(14, (unsigned)(uintptr_t)path, 0, 0);
}
int unlink(const char *path) { return (int)call3(15, (unsigned)(uintptr_t)path, 0, 0); }
int rename(const char *from, const char *to) {
    return (int)call3(16, (unsigned)(uintptr_t)from, (unsigned)(uintptr_t)to, 0);
}
int getpid(void) { return (int)call3(17, 0, 0, 0); }
int pipe(int fds[2]) { return (int)call3(18, (unsigned)(uintptr_t)fds, 0, 0); }
int fsync(int fd) { return (int)call3(19, (unsigned)fd, 0, 0); }
int luna_spawn(const char *path, const char *const argv[], const char *const envp[]) {
    return (int)call3(20, (unsigned)(uintptr_t)path,
                      (unsigned)(uintptr_t)argv, (unsigned)(uintptr_t)envp);
}
int luna_waitpid(int pid, int *status, int options) {
    return (int)call3(21, (unsigned)pid, (unsigned)(uintptr_t)status, (unsigned)options);
}
int luna_execve(const char *path, const char *const argv[], const char *const envp[]) {
    return (int)call3(22, (unsigned)(uintptr_t)path,
                      (unsigned)(uintptr_t)argv, (unsigned)(uintptr_t)envp);
}
int chmod(const char *path, unsigned mode) {
    return (int)call3(23, (unsigned)(uintptr_t)path, (unsigned)mode, 0);
}
int luna_utime(const char *path, uint32_t seconds) {
    return (int)call3(24, (unsigned)(uintptr_t)path, (unsigned)seconds, 0);
}
int luna_tcp_connect(const char *host, unsigned port, int verified_tls) {
    return (int)call3(25, (unsigned)(uintptr_t)host, port,
                      (unsigned)(verified_tls != 0));
}
int luna_opendir(const char *path) {
    return (int)call3(26, (unsigned)(uintptr_t)path, 0, 0);
}
int luna_readdir(int fd, LunaGuestDirent *entry) {
    return (int)call3(27, (unsigned)fd, (unsigned)(uintptr_t)entry, 0);
}
__attribute__((noreturn)) void _exit(int code) {
    (void)call3(3, (unsigned)code, 0, 0);
    for (;;) { __asm__ volatile("pause"); }
}
