#include <unistd.h>
#include "pthread_impl.h"
static struct pthread initial_thread;
extern char **__environ;
extern int main(int, char **, char **);
void *__luna_thread_pointer(void) { return &initial_thread; }
_Noreturn void __luna_start(int argc, char **argv, char **envp) {
    static size_t aux[] = {0,0};
    __environ = envp;
    __progname_full = argv[0];
    __progname = argv[0];
    for (char *p = argv[0]; *p; p++) if (*p == '/') __progname = p + 1;
    libc.auxv = aux;
    libc.page_size = 4096;
    initial_thread.self = &initial_thread;
    initial_thread.prev = initial_thread.next = &initial_thread;
    initial_thread.tid = getpid();
    initial_thread.locale = &libc.global_locale;
    exit(main(argc, argv, envp));
}
