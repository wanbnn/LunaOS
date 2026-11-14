#ifndef LUNA_TASK_H
#define LUNA_TASK_H
#include "perf.h"
#include "elf32.h"
#include "pmm.h"
#define TASK_MAX 20
#define TASK_EVENTS 64
#define APP_COUNT 7
#define APP_HOME 0
#define APP_SHELL 1
#define APP_FILES 2
#define APP_BROWSER 3
#define APP_SETTINGS 4
#define APP_TASKS 5
#define APP_NOTES 6
#define EVENT_KEY 1
#define EVENT_CLICK 2
#define EVENT_NAVIGATE 3
#define EVENT_INIT 4
#define EVENT_DRAG 5
#define EVENT_FILE_PREVIEW 6
#define EVENT_SERVICE 7
#define EVENT_SCROLL 8
#define EVENT_BROWSER_TIMER 9
#define CAP_UI 1u
#define CAP_TASK_CONTROL 2u
#define CAP_SYSTEM 4u
#define USER_CODE 0x40000000u
#define USER_STACK 0x40010000u
#define USER_STACK_BYTES 16384u
#define USER_ELF_STACK_BYTES (128u * 1024u)
#define USER_ELF_STACK (USER_CODE + LUNA_ELF_STACK_OFFSET)
#define SYS_HELLO 0
#define SYS_WAIT_EVENT 1
#define SYS_APP_EVENT 2
#define SYS_EXIT 3
#define SYS_SYSTEM 4
#define SYS_PROBE_FAIL 5
#define SYS_EXEC_WRITE 6 /* ELF32 console/file/pipe write; LunaOS-specific limits. */
#define SYS_EXEC_OPEN 7  /* LunaOS flags; includes exclusive-create lockfiles. */
#define SYS_EXEC_READ 8
#define SYS_EXEC_CLOSE 9
#define SYS_EXEC_LSEEK 10
#define SYS_EXEC_STAT 11
#define SYS_EXEC_CHDIR 12
#define SYS_EXEC_GETCWD 13
#define SYS_EXEC_MKDIR 14
#define SYS_EXEC_UNLINK 15
#define SYS_EXEC_RENAME 16
#define SYS_EXEC_GETPID 17
#define SYS_EXEC_PIPE 18 /* Inherited 4096-byte pipe, blocking with a bounded wait. */
#define SYS_EXEC_FSYNC 19 /* Commits LunaFS2 snapshot for durable writes. */
#define SYS_EXEC_SPAWN 20 /* Native child process, argv/envp pointers. */
#define SYS_EXEC_WAITPID 21 /* Child exit status, not full waitpid(2) yet. */
#define SYS_EXEC_EXECVE 22 /* Replace current process image, preserve PID and open FDs. */
#define SYS_EXEC_CHMOD 23 /* Persist 0777 permission bits, no enforcement yet. */
#define SYS_EXEC_UTIME 24 /* Persist file mtime (epoch seconds). */
#define SYS_EXEC_TCP_CONNECT 25 /* Exclusive outbound IPv4 TCP/TLS socket. */
#define SYS_EXEC_OPENDIR 26 /* Descriptor-backed directory iterator. */
#define SYS_EXEC_READDIR 27 /* One LunaGuestDirent record per call. */
#define SYS_EXEC_BRK 28 /* Bounded process heap, Linux-like program-break result. */
#define SYS_EXEC_CLOCK 29 /* clock id, 16-byte {seconds64,nsec32,reserved32}. */
#define SYS_EXEC_RANDOM 30 /* Fill a validated guest buffer from kernel entropy. */
#define SYS_EXEC_FSTAT 31 /* Native 20-byte stat record for an open file. */
#define SYS_EXEC_SIGACTION 32
#define SYS_EXEC_SIGMASK 33
#define SYS_EXEC_KILL 34
#define SYS_EXEC_SIGRETURN 35
#define SYS_EXEC_FORK 36
#define SYS_EXEC_DUP2 37
#define SYS_EXEC_FDFLAGS 38
#define SYS_EXEC_SESSION 39
#define SYS_EXEC_SIGSUSPEND 40
#define SYS_EXEC_FCHMOD 41
#define SYS_EXEC_FUTIME 42
#define SYS_EXEC_POLL 43
#define EXEC_FD_MAX 8
#define EXEC_FD_STORAGE (EXEC_FD_MAX + 3)
/* Saved by the common interrupt entry; useresp/ss exist only on privilege transitions. */
typedef struct {
    uint32_t gs, fs, es, ds, edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t vector, error, eip, cs, eflags, useresp, ss;
} TrapFrame;
typedef enum {
    TASK_UNUSED,
    TASK_READY,
    TASK_RUNNING,
    TASK_WAITING,
    TASK_SLEEPING,
    TASK_EXITED
} TaskState;
typedef struct {
    uint32_t type;
    int a, b;
} AppEvent;
typedef struct { uint32_t handler, flags, restorer, mask[2]; } ExecSignalAction;
typedef struct {
    uint32_t pid;
    char name[32];
    uint32_t ring, observed_ring, capabilities;
    int app;
    TaskState state;
    TrapFrame *frame;
    uint32_t *directory;
    uint8_t *kernel_stack;
    uint32_t stack_bytes;
    uint32_t memory_owner; /* Per-task service attribution, preserved across yields. */
    uint32_t probe, wake_tick, cpu_permille, cpu_units, switches, run_ticks, exit_code;
    AppEvent events[TASK_EVENTS];
    uint32_t read_event, write_event;
    AppEvent delivered;
    bool event_pending, system, exec_image;
    uint8_t exec_permissions[LUNA_ELF_PAGES];
    PmmAllocation exec_allocation[LUNA_ELF_MAX_SEGMENTS];
    uint32_t exec_segments, exec_physical_bytes;
    uint32_t exec_heap_base, exec_heap_break, exec_heap_pages;
    uint32_t parent_pid, session_id, process_group, termination_signal;
    ExecSignalAction signal_actions[31];
    uint64_t signal_mask, signal_saved_mask;
    uint32_t signal_pending;
    TrapFrame signal_frame;
    bool in_signal, suspend_restore, exec_reaped;
    uint64_t suspend_old_mask;
    char exec_cwd[128];
    char exec_path[128];
    bool std_closed[3];
    uintptr_t exec_source; /* Immutable payload identity, zero for writable files. */
    struct { char path[128]; uint32_t offset, flags; uint8_t kind, pipe_id; bool open; } exec_fds[EXEC_FD_STORAGE];
    void (*service)(void);
    PerfArea perf_area;
    unsigned storage_depth;
} Task;
typedef struct {
    uint64_t ram_bytes, heap_committed, heap_live;
    uint32_t stack_reserved, page_tables, heap_allocations;
} TaskMemoryStats;
uint32_t task_memory_owner(void);
uint32_t task_memory_enter(uint32_t pid);
void task_memory_leave(uint32_t previous);
void task_memory_stats(const Task *,TaskMemoryStats *);
extern Task tasks[TASK_MAX];
extern int current_task;
void task_init(void);
uint32_t task_spawn(int, const char *, unsigned, unsigned);
uint32_t task_spawn_elf(const char *path);
bool task_wait_elf(uint32_t pid, uint32_t timeout_ms, unsigned *exit_code);
uint32_t task_spawn_elf_args(const char *path, int argc, const char *const argv[],
                             int envc, const char *const envp[]);
uint32_t task_spawn_service(const char *, void (*service)(void));
Task *task_by_pid(uint32_t);
Task *task_for_app(int);
Task *task_current(void);
bool task_post(uint32_t, AppEvent);
bool task_kill(uint32_t);
void task_yield(void);
void task_sleep(uint32_t);
void task_idle(void);
void task_yield_ready(void);
void task_sleep_ms(uint32_t);
bool task_framebuffer_wc(uint32_t physical, uint32_t bytes);
void task_list(char *, size_t);
void task_ring_tests(char *, size_t);
const char *task_state_name(TaskState);
TrapFrame *trap_dispatch(TrapFrame *);
void task_kernel_fault(void);
void app_event(int, AppEvent);
void app_reset(int);
void app_close(int);
void gui_task_closed(int);
void gui_serial(int);
void gui_pointer(int, int, int);
void gui_scroll(int, int, int);
void gui_focus_home(void);
void net_cancel_owner(uint32_t);
#endif
