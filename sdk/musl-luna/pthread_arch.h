/* Single-threaded LunaOS processes have a process-local libc thread record.
 * This is not TLS support or permission to start pthreads. */
void *__luna_thread_pointer(void);
static inline uintptr_t __get_tp(void) { return (uintptr_t)__luna_thread_pointer(); }
#define MC_PC gregs[REG_EIP]
