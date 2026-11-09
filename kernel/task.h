#ifndef LUNA_TASK_H
#define LUNA_TASK_H
#include "perf.h"
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
#define CAP_UI 1u
#define CAP_TASK_CONTROL 2u
#define CAP_SYSTEM 4u
#define USER_CODE 0x40000000u
#define USER_STACK 0x40010000u
#define USER_STACK_BYTES 16384u
#define SYS_HELLO 0
#define SYS_WAIT_EVENT 1
#define SYS_APP_EVENT 2
#define SYS_EXIT 3
#define SYS_SYSTEM 4
#define SYS_PROBE_FAIL 5
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
    bool event_pending, system;
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
