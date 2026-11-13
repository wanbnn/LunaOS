#ifndef LUNA_GUEST_POSIX_H
#define LUNA_GUEST_POSIX_H
#include <stddef.h>
#include <stdint.h>
/* Minimal LunaOS freestanding C bridge. Not a complete POSIX libc.
 * Syscall numbers and errno policy are LunaOS-specific and experimental.
 * Pointers are 32-bit guest segment-relative offsets, not Linux addresses. */
#define LUNA_O_RDONLY 0
#define LUNA_O_WRONLY 1
#define LUNA_O_RDWR 2
#define LUNA_O_CREAT 64
#define LUNA_O_TRUNC 512
#define LUNA_O_APPEND 1024
#define LUNA_SEEK_SET 0
#define LUNA_SEEK_CUR 1
#define LUNA_SEEK_END 2
typedef struct {
    uint32_t size, mode, reserved, mtime, ctime;
} LunaGuestStat;
long write(int fd, const void *buffer, size_t n);
long read(int fd, void *buffer, size_t n);
int open(const char *path, int flags, ...);
int close(int fd);
long lseek(int fd, long offset, int whence);
char *getcwd(char *buffer, size_t capacity);
int chdir(const char *path);
int mkdir(const char *path, unsigned mode);
int unlink(const char *path);
int rename(const char *from, const char *to);
int getpid(void);
int pipe(int fds[2]); /* bounded 4096 B; returns -1 instead of blocking */
int fsync(int fd); /* persists a LunaFS2 snapshot */
int luna_spawn(const char *path, const char *const argv[], const char *const envp[]);
int luna_waitpid(int pid, int *status, int options); /* options 1 = WNOHANG */
int luna_execve(const char *path, const char *const argv[], const char *const envp[]); /* does not return on success */
int chmod(const char *path, unsigned mode);
int luna_utime(const char *path, uint32_t unix_seconds);
int luna_tcp_connect(const char *host, unsigned port, int verified_tls);
/* Returns a guest fd; use read/write/close. Singleton TCP backend for now. */
int luna_stat(const char *path, LunaGuestStat *info);
__attribute__((noreturn)) void _exit(int code);
#endif
