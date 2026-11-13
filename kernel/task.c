#include "update.h"
#include "task.h"
#include "elf32.h"
#include "driver.h"
#include "heap.h"
#include "memory.h"
#include "boot.h"
#define KSTACK_BYTES (128 * 1024)
#define SHELL_STACK_BYTES (1024 * 1024)
#define STACK_MAGIC 0x51ac7a5bu
Task tasks[TASK_MAX];
int current_task;
static bool initialized, idling;
static uint32_t next_pid = 1, stamp, sample_stamp, sample_tick;
static uint32_t directories[TASK_MAX][1024] __attribute__((aligned(4096)));
static uint32_t user_tables[TASK_MAX][1024] __attribute__((aligned(4096)));
static uint8_t user_code[4096] __attribute__((aligned(4096)));
/* One private executable code page per ring-3 program (no shared RW alias). */
static uint8_t exec_code[TASK_MAX][LUNA_ELF_CODE_BYTES] __attribute__((aligned(4096)));
static uint8_t user_stacks[TASK_MAX][USER_STACK_BYTES] __attribute__((aligned(4096)));
static uint8_t kernel_stacks[TASK_MAX][KSTACK_BYTES] __attribute__((aligned(4096), section(".bss.stacks")));
static uint8_t shell_stack[SHELL_STACK_BYTES] __attribute__((aligned(4096)));
#define MMIO_BASE 0x3c000000u
#define MMIO_PAGES 4096u
static uint32_t mmio_tables[4][1024] __attribute__((aligned(4096)));
static uint16_t mmio_lengths[MMIO_PAGES];
static uint32_t framebuffer_tables[4][1024] __attribute__((aligned(4096)));
static uint64_t framebuffer_start, framebuffer_end;
static uint64_t gdt[10];
static struct {
    uint32_t previous, esp0, ss0, esp1, ss1, esp2, ss2, cr3, eip, eflags, eax, ecx, edx, ebx, esp,
        ebp, esi, edi, es, cs, ss, ds, fs, gs, ldt;
    uint16_t trap, iomap;
} __attribute__((packed)) tss;
extern const uint8_t user_program_start[], user_program_end[];
static uint32_t clock_units(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (lo >> 10) | (hi << 22);
}
static void serial_num(unsigned n) {
    char s[16];
    number(s, n);
    serial_write(s);
}
static uint64_t segment(uint32_t base, uint32_t limit, uint8_t access, uint8_t flags) {
    return (limit & 65535) | ((uint64_t)(base & 0xffffff) << 16) | ((uint64_t)access << 40) |
           ((uint64_t)((limit >> 16) & 15) << 48) | ((uint64_t)(flags & 15) << 52) |
           ((uint64_t)(base >> 24) << 56);
}
Task *task_current(void) {
    return &tasks[current_task];
}
uint32_t task_memory_owner(void) {
    if (!initialized) return 1; /* Boot allocations belong to the future kernel PID. */
    Task *t=task_current();
    return t->memory_owner ? t->memory_owner : t->pid;
}
uint32_t task_memory_enter(uint32_t pid) {
    uint32_t previous=task_current()->memory_owner;
    task_current()->memory_owner=pid ? pid : 1; return previous;
}
void task_memory_leave(uint32_t previous) { task_current()->memory_owner=previous; }
static uint32_t private_reservation(const Task *t) {
    return t->ring ? t->stack_bytes+USER_STACK_BYTES+sizeof(directories[0])+sizeof(user_tables[0]) : 0;
}
void task_memory_stats(const Task *t,TaskMemoryStats *out) {
    if (!out) return;
    *out=(TaskMemoryStats){0};
    if (!t || t->state==TASK_UNUSED || t->state==TASK_EXITED) return;
    HeapStats own; heap_stats(t->pid,&own);
    out->heap_committed=own.committed_bytes; out->heap_live=own.live_bytes;
    out->heap_allocations=own.allocations;
    if (t->ring) {
        out->stack_reserved=t->stack_bytes+USER_STACK_BYTES;
        out->page_tables=sizeof(directories[0])+sizeof(user_tables[0]);
        out->ram_bytes=private_reservation(t)+own.committed_bytes;
    } else {
        extern uint8_t __bss_low_end[],__bss_high_start[],__bss_end[];
        uint64_t linked=(((uint64_t)(uintptr_t)__bss_low_end+4095)&~UINT64_C(4095))-0x100000u+
            ((((uint64_t)(uintptr_t)__bss_end+4095)&~UINT64_C(4095))-(uintptr_t)__bss_high_start);
        uint64_t assigned=0;
        for (unsigned i=1; i<TASK_MAX; i++) if (tasks[i].state!=TASK_UNUSED && tasks[i].state!=TASK_EXITED) {
            HeapStats h; heap_stats(tasks[i].pid,&h);
            assigned+=private_reservation(tasks+i)+h.committed_bytes;
        }
        MemoryStats memory; memory_stats(&memory);
        uint64_t metadata=((uint64_t)memory.metadata_bytes+4095)&~UINT64_C(4095);
        uint64_t total=linked+memory.allocated_bytes+metadata+0x300000u; /* Bootstrap stack reservation. */
        if (boot_is_uefi()) total+=LUNA_UEFI_INFO->ram_bytes; /* Kernel-owned RAM disk. */
        out->ram_bytes=total>assigned ? total-assigned : 0;
    }
}
Task *task_by_pid(uint32_t pid) {
    for (int i = 0; i < TASK_MAX; i++)
        if (tasks[i].state != TASK_UNUSED && tasks[i].pid == pid)
            return &tasks[i];
    return 0;
}
Task *task_for_app(int app) {
    for (int i = 1; i < TASK_MAX; i++)
        if (tasks[i].state != TASK_UNUSED && tasks[i].state != TASK_EXITED && tasks[i].app == app)
            return &tasks[i];
    return 0;
}
const char *task_state_name(TaskState s) {
    const char *n[] = {"unused", "ready", "running", "waiting", "sleeping", "exited"};
    return s <= TASK_EXITED ? n[s] : "unknown";
}
void task_init(void) {
    memset(&tss, 0, sizeof(tss));
    tss.ss0 = 16;
    tss.esp0 = 0x3f00000;
    tss.ss1 = 0x21;
    tss.ss2 = 0x32;
    tss.iomap = sizeof(tss);
    gdt[0] = 0;
    for (int ring = 0; ring < 4; ring++) {
        /* Paging treats CPL 0, 1 and 2 as supervisor. Bound every nonzero
         * data/code selector as well; otherwise ring 1 could load a flat DPL-3
         * data selector and reach supervisor mappings. */
        uint32_t base = ring ? USER_CODE : 0, limit = ring ? 0x13fff : 0xfffff;
        gdt[1 + ring * 2] = segment(base, limit, 0x9a | (ring << 5), ring ? 4 : 0xc);
        gdt[2 + ring * 2] = segment(base, limit, 0x92 | (ring << 5), ring ? 4 : 0xc);
    }
    gdt[9] = segment((uint32_t)&tss, sizeof(tss) - 1, 0x89, 0);
    struct {
        uint16_t size;
        uint32_t address;
    } __attribute__((packed)) gdtr = {sizeof(gdt) - 1, (uint32_t)gdt};
    __asm__ volatile("cli; lgdt %0" ::"m"(gdtr));
    uint16_t tr = 0x48;
    __asm__ volatile("ltr %0" ::"r"(tr));
    for (unsigned i = 0; i < 1024; i++)
        directories[0][i] = (i << 22) | 0x83;
    for (unsigned j = 0; j < 4; j++)
        directories[0][(MMIO_BASE >> 22) + j] = (uint32_t)mmio_tables[j] | 3;
    uint32_t cr4;
    __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
    cr4 |= 16;
    __asm__ volatile("mov %0,%%cr4" ::"r"(cr4) : "memory");
    __asm__ volatile("mov %0,%%cr3" ::"r"(directories[0]) : "memory");
    uint32_t cr0;
    __asm__ volatile("mov %%cr0,%0" : "=r"(cr0));
    cr0 |= 0x80010000u;
    __asm__ volatile("mov %0,%%cr0" ::"r"(cr0) : "memory");
    size_t bytes = user_program_end - user_program_start;
    if (bytes > sizeof(user_code))
        kernel_fault();
    memcpy(user_code, user_program_start, bytes);
    tasks[0].pid = next_pid++;
    strcpy(tasks[0].name, "Luna kernel / compositor");
    tasks[0].state = TASK_RUNNING;
    tasks[0].app = -1;
    tasks[0].system = true;
    tasks[0].capabilities = CAP_SYSTEM;
    tasks[0].directory = directories[0];
    stamp = sample_stamp = clock_units();
    sample_tick = ticks;
    initialized = true;
    __asm__ volatile("sti");
    serial_write("Tasks: paging / TSS / CPL 0,1,2,3 / int80 / private stacks\n");
}
static uint32_t spawn(int app, const char *name, unsigned ring, unsigned probe,
                      void (*service)(void)) {
    if (!initialized || ring < 1 || ring > 3 || app >= APP_COUNT)
        return 0;
    int slot;
    for (slot = 1; slot < TASK_MAX; slot++)
        if (tasks[slot].state == TASK_UNUSED || tasks[slot].state == TASK_EXITED)
            break;
    if (slot == TASK_MAX)
        return 0;
    Task *t = &tasks[slot];
    memset(t, 0, sizeof(*t));
    t->pid = next_pid++;
    append(t->name, name, sizeof(t->name));
    t->ring = ring;
    t->observed_ring = 255;
    t->app = app;
    t->probe = probe;
    t->system = ring < 3 || service != NULL;
    t->service = service;
    t->capabilities = app >= 0 || service ? CAP_UI : 0;
    if (app == APP_TASKS || app == APP_SHELL)
        t->capabilities |= CAP_TASK_CONTROL;
    t->kernel_stack = app == APP_SHELL ? shell_stack : kernel_stacks[slot];
    t->stack_bytes = app == APP_SHELL ? SHELL_STACK_BYTES : KSTACK_BYTES;
    memset(t->kernel_stack, 0xcc, t->stack_bytes);
    *(uint32_t *)t->kernel_stack = STACK_MAGIC;
    memset(user_stacks[slot], 0, USER_STACK_BYTES);
    memcpy(directories[slot], directories[0], 4096);
    memset(user_tables[slot], 0, 4096);
    directories[slot][USER_CODE >> 22] = (uint32_t)user_tables[slot] | 7;
    user_tables[slot][0] = (uint32_t)user_code | 5;
    for (unsigned i = 0; i < USER_STACK_BYTES / 4096; i++)
        user_tables[slot][16 + i] = ((uint32_t)user_stacks[slot] + i * 4096) | 7;
    t->directory = directories[slot];
    t->frame = (TrapFrame *)(t->kernel_stack + t->stack_bytes - sizeof(TrapFrame));
    memset(t->frame, 0, sizeof(TrapFrame));
    uint32_t cs = ((1 + ring * 2) << 3) | ring, ds = ((2 + ring * 2) << 3) | ring;
    t->frame->cs = cs;
    t->frame->ss = t->frame->ds = t->frame->es = t->frame->fs = t->frame->gs = ds;
    t->frame->eip = 0;
    t->frame->eflags = 0x202;
    t->frame->useresp = USER_STACK + USER_STACK_BYTES - USER_CODE - 16;
    t->frame->eax = probe;
    t->state = TASK_READY;
    if (!service)
        gui_damage_all();
    return t->pid;
}
uint32_t task_spawn(int app, const char *name, unsigned ring, unsigned probe) {
    return spawn(app, name, ring, probe, NULL);
}
/* MVP ELF execution: only one RX PT_LOAD fitting in a page is accepted.
 * The file is checked before a task slot is claimed. No arbitrary user
 * syscall is exposed to this program, other than write-to-console and exit. */
uint32_t task_spawn_elf(const char *path) {
    File *f = fs_find(path);
    if (!f || f->dir || f->size < 84 || f->size > LUNA_ELF_CODE_BYTES)
        return 0;
    uint8_t *bytes = heap_allocate(f->size);
    if (!bytes)
        return 0;
    bool read_ok = fs_read_bytes(f, 0, bytes, f->size);
    LunaElfImage *image = heap_allocate(sizeof(LunaElfImage));
    if (!image) {
        heap_release(bytes);
        return 0;
    }
    bool accepted = read_ok && luna_elf32_load(bytes, f->size, image);
    heap_release(bytes);
    if (!accepted) {
        heap_release(image);
        return 0;
    }
    uint32_t pid = spawn(-1, "ELF32 native user process", 3, 0, NULL);
    if (!pid) {
        heap_release(image);
        return 0;
    }
    Task *t = task_by_pid(pid);
    unsigned slot = (unsigned)(t - tasks);
    memcpy(exec_code[slot], image->code, LUNA_ELF_CODE_BYTES);
    /* Ring-3 code segment has base USER_CODE; EIP is segment relative. */
    user_tables[slot][0] = (uint32_t)exec_code[slot] | 5;
    t->frame->eip = image->entry;
    t->exec_image = true;
    heap_release(image);
    return pid;
}
uint32_t task_spawn_service(const char *name, void (*service)(void)) {
    return spawn(-1, name, 3, 0, service);
}
bool task_post(uint32_t pid, AppEvent e) {
    Task *t = task_by_pid(pid);
    if (!t || t->state == TASK_EXITED)
        return false;
    uint32_t next = (t->write_event + 1) % TASK_EVENTS;
    if (next == t->read_event)
        return false;
    t->events[t->write_event] = e;
    t->write_event = next;
    if (t->state == TASK_WAITING)
        t->state = TASK_READY;
    else if (t->state != TASK_READY && t->state != TASK_RUNNING) {
        serial_write("POST lost: task not waiting\n");
        return true;
    }
    return true;
}
static void finish(Task *t, unsigned code) {
    t->exit_code = code;
    t->state = TASK_EXITED;
    net_cancel_owner(t->pid);
    if (t->app >= 0) {
        uint32_t previous=task_memory_enter(t->pid);
        app_close(t->app);
        task_memory_leave(previous);
        gui_task_closed(t->app);
    }
    if (heap_release_owner(t->pid)) {
        serial_write("Process heap release failed\n");
        kernel_fault();
    }
    gui_damage_all();
}
bool task_kill(uint32_t pid) {
    if (update_busy() && update_owner() == pid) return false;
    Task *t = task_by_pid(pid);
    if (!t || t->system || t->storage_depth || t->state == TASK_EXITED || t->state == TASK_UNUSED)
        return false;
    finish(t, 0);
    return true;
}
void task_yield(void) {
    if (initialized)
        __asm__ volatile("int $0x81" ::: "memory");
    else
        __asm__ volatile("hlt");
}
void task_yield_ready(void) {
    for (unsigned i = 0; i < TASK_MAX; i++)
        if (tasks[i].state == TASK_READY) {
            task_yield();
            return;
        }
}
void task_sleep_ms(uint32_t duration) {
    if (!initialized) {
        uint32_t start = frame_clock;
        while (frame_clock - start < duration)
            __asm__ volatile("hlt");
        return;
    }
    if (!duration)
        return;
    Task *t = task_current();
    t->wake_tick = frame_clock + duration;
    t->state = TASK_SLEEPING;
    task_yield();
}
void task_sleep(uint32_t duration) {
    task_sleep_ms(duration > INT32_MAX / 10u ? INT32_MAX : duration * 10);
}
void task_idle(void) {
    if (!initialized) {
        __asm__ volatile("hlt");
        return;
    }
    perf_pause();
    idling = true;
    uint32_t now = clock_units();
    tasks[current_task].cpu_units += now - stamp;
    __asm__ volatile("hlt");
    stamp = clock_units();
    idling = false;
    perf_resume();
}
static TrapFrame *schedule(TrapFrame *f) {
    Task *old = task_current();
    perf_pause();
    uint32_t now = clock_units();
    if (!idling)
        old->cpu_units += now - stamp;
    stamp = now;
    old->frame = f;
    if (old->state == TASK_RUNNING)
        old->state = TASK_READY;
    if (current_task && *(uint32_t *)old->kernel_stack != STACK_MAGIC) {
        serial_write("Kernel stack overflow\n");
        kernel_fault();
    }
    int next = current_task;
    for (int n = 1; n <= TASK_MAX; n++) {
        int candidate = (current_task + n) % TASK_MAX;
        if (tasks[candidate].state == TASK_READY) {
            next = candidate;
            break;
        }
    }
    if (tasks[next].state != TASK_READY) {
        serial_write("No runnable kernel task\n");
        kernel_fault();
    }
    current_task = next;
    Task *t = task_current();
    t->state = TASK_RUNNING;
    t->switches++;
    if (next) {
        tss.esp0 = (uint32_t)(t->kernel_stack + t->stack_bytes);
        tss.esp1 = tss.esp2 = tss.esp0;
    } else
        tss.esp0 = 0x3f00000;
    if (t->directory != old->directory)
        __asm__ volatile("mov %0,%%cr3" ::"r"(t->directory) : "memory");
    perf_resume();
    return t->frame;
}
static void sample(void) {
    uint32_t now = clock_units(), interval = now - sample_stamp;
    if (!interval)
        return;
    if (!idling) {
        tasks[current_task].cpu_units += now - stamp;
        stamp = now;
    }
    for (int i = 0; i < TASK_MAX; i++) {
        uint32_t units = tasks[i].cpu_units;
        tasks[i].cpu_units = 0;
        tasks[i].cpu_permille = units / (interval / 1000 + 1);
        if (tasks[i].cpu_permille > 1000)
            tasks[i].cpu_permille = 1000;
    }
    sample_stamp = now;
    sample_tick = ticks;
}
static TrapFrame *syscall(TrapFrame *f) {
    Task *t = task_current();
    unsigned call = f->eax;
    __asm__ volatile("sti");
    switch (call) {
    case SYS_HELLO:
        t->observed_ring = f->cs & 3;
        if (t->observed_ring != t->ring || f->edx != t->ring) {
            finish(t, 255);
            break;
        }
        f->eax = t->pid;
        break;
    case SYS_WAIT_EVENT:
        if (!(t->capabilities & CAP_UI)) {
            f->eax = (uint32_t)-1;
            break;
        }
        while (t->read_event == t->write_event) {
            t->state = TASK_WAITING;
            task_yield();
        }
        t->delivered = t->events[t->read_event];
        t->read_event = (t->read_event + 1) % TASK_EVENTS;
        t->event_pending = true;
        f->eax = t->delivered.type;
        break;
    case SYS_APP_EVENT:
        if (!(t->capabilities & CAP_UI) || !t->event_pending) {
            f->eax = (uint32_t)-1;
            break;
        }
        t->event_pending = false;
        if (t->service) {
            if (t->delivered.type == EVENT_SERVICE)
                t->service();
        } else {
            app_event(t->app, t->delivered);
        }
        f->eax = 0;
        break;
    case SYS_EXIT:
        if (t->probe == 3) {
            serial_write("RINGTEST PASS ring=");
            serial_num(t->ring);
            serial_write(" syscall denied\n");
        }
        finish(t, f->ebx);
        break;
    case SYS_EXEC_WRITE: {
        /* stdout/stderr only, bounded access to the guest's code/stack.
         * No implicit access to supervisor memory or other processes.
         * EIP/ECX here are segment-relative user offsets. */
        uint32_t pos = f->ecx, length = f->edx;
        if (!t->exec_image || t->ring != 3 ||
            (f->ebx != 1 && f->ebx != 2) || length > 256u ||
            pos >= 0x14000u || length > 0x14000u - pos ||
            !((pos < LUNA_ELF_CODE_BYTES && length <= LUNA_ELF_CODE_BYTES - pos) ||
              (pos >= 0x10000u && length <= 0x14000u - pos))) {
            f->eax = (uint32_t)-1;
            break;
        }
        char buffer[257];
        memcpy(buffer, (const void *)(uintptr_t)(USER_CODE + pos), length);
        buffer[length] = 0;
        /* Reject embedded NUL until stream-safe terminal output exists. */
        if (strlen(buffer) != length) {
            f->eax = (uint32_t)-1;
            break;
        }
        shell_print(buffer);
        f->eax = length;
        break;
    }
    case SYS_SYSTEM:
        f->eax = (uint32_t)-1;
        break;
    case SYS_PROBE_FAIL:
        serial_write("RINGTEST FAIL ring=");
        serial_num(t->ring);
        serial_write(" protection bypass\n");
        finish(t, 255);
        break;
    default:
        f->eax = (uint32_t)-1;
        break;
    }
    __asm__ volatile("cli");
    return t->state == TASK_EXITED ? schedule(f) : f;
}
TrapFrame *trap_dispatch(TrapFrame *f) {
    if (f->vector == 32) {
        timer_interrupt();
        if (!initialized)
            return f;
        for (int i = 0; i < TASK_MAX; i++)
            if (tasks[i].state == TASK_SLEEPING && (int32_t)(frame_clock - tasks[i].wake_tick) >= 0)
                tasks[i].state = TASK_READY;
        if (!idling)
            task_current()->run_ticks++;
        if (ticks - sample_tick >= 50)
            sample();
        if ((f->cs & 3) && frame_clock % 20 == 0)
            return schedule(f);
        return f;
    }
    if (f->vector >= 33 && f->vector < 48) {
        driver_irq_dispatch(f->vector - 32);
        return f;
    }
    if (f->vector == 129 && initialized)
        return schedule(f);
    if (f->vector == 128 && initialized && (f->cs & 3))
        return syscall(f);
    if (initialized && (f->cs & 3) && current_task) {
        Task *t = task_current();
        bool pass = (t->probe == 1 && f->vector == 13) || (t->probe == 2 && f->vector == 14) ||
                    (t->probe == 4 && (f->vector == 13 || f->vector == 14));
        serial_write(pass ? "RINGTEST PASS ring=" : "Task fault ring=");
        serial_num(t->ring);
        serial_write(t->probe == 1 ? " I/O blocked" : " memory blocked");
        serial_write(" pid=");
        serial_num(t->pid);
        serial_write(" vector=");
        serial_num(f->vector);
        serial_write("\n");
        finish(t, pass ? 0 : f->vector);
        return schedule(f);
    }
    serial_write("Kernel trap vector=");
    serial_num(f->vector);
    serial_write(" eip=");
    char b[16];
    hex(b, f->eip);
    serial_write(b);
    kernel_fault();
    return f;
}
void task_list(char *out, size_t cap) {
    out[0] = 0;
    append(out, "PID  RING  STATE       RAM KiB  HEAP KiB  STACK KiB  SWITCHES  NAME\n", cap);
    for (int i = 0; i < TASK_MAX; i++) {
        Task *t = &tasks[i];
        if (t->state == TASK_UNUSED || t->state == TASK_EXITED)
            continue;
        char b[16];
        number(b, t->pid);
        append(out, b, cap);
        append(out, "    ", cap);
        number(b, t->ring);
        append(out, b, cap);
        append(out, "    ", cap);
        append(out, task_state_name(t->state), cap);
        append(out, "    ", cap);
        TaskMemoryStats memory; task_memory_stats(t,&memory);
        number(b,(uint32_t)((memory.ram_bytes+1023)/1024)); append(out,b,cap); append(out,"    ",cap);
        number(b,(uint32_t)((memory.heap_committed+1023)/1024)); append(out,b,cap); append(out,"    ",cap);
        number(b,memory.stack_reserved/1024);
        append(out,b,cap);
        append(out, "    ", cap);
        number(b, t->switches);
        append(out, b, cap);
        append(out, "    ", cap);
        append(out, t->name, cap);
        append(out, "\n", cap);
    }
}
void task_ring_tests(char *out, size_t cap) {
    out[0] = 0;
    for (unsigned ring = 1; ring <= 3; ring++)
        for (unsigned test = 1; test <= 4; test++) {
            char name[32] = "ring", b[16];
            number(b, ring);
            strcat(name, b);
            strcat(name, " protection probe");
            if (!task_spawn(-1, name, ring, test)) {
                append(out, "ringtest: no free task slots\n", cap);
                return;
            }
        }
    append(out, "ringtest: 12 hardware protection probes started (rings 1, 2, 3).\n", cap);
}

/* Dedicated virtual arena shared by all CR3s, never mapped user-accessible. */
/* Keep the framebuffer at its identity address. Replacing those entries
 * avoids creating a second virtual alias with a different memory type. */
bool task_framebuffer_wc(uint32_t physical, uint32_t bytes) {
    uint64_t end = ((uint64_t)physical + bytes + 4095u) & ~UINT64_C(4095);
    uint32_t start = physical & ~4095u;
    if (!bytes || framebuffer_end || start < 0x4000000u || end > UINT64_C(0x100000000))
        return false;
    unsigned first = start >> 22, last = (end - 1) >> 22;
    if (last - first >= 4 || (start < USER_CODE + 0x400000u && end > MMIO_BASE))
        return false;
    unsigned ranges = *(const volatile uint32_t *)0x5000;
    const volatile PmmRange *map = (const volatile PmmRange *)0x5004;
    for (unsigned i = 0; i < ranges && i < 128; i++) {
        uint64_t base = map[i].base;
        if (map[i].type == 1 && base < end &&
            (base >= start || map[i].length > start - base))
            return false;
    }
    for (unsigned i = 0; i < MMIO_PAGES; i++) {
        uint32_t page = mmio_tables[i / 1024][i % 1024];
        if (page && (page & ~4095u) >= start && (page & ~4095u) < end)
            return false;
    }

    uint32_t eax = 1, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    const uint32_t required = (1u << 5) | (1u << 16) | (1u << 25); /* MSR, PAT, SFENCE */
    if ((edx & required) != required)
        return false;
    const uint32_t pat_msr = 0x277;
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(pat_msr));
    uint64_t pat = ((uint64_t)high << 32) | low;
    unsigned index;
    for (index = 0; index < 8; index++)
        if (((pat >> (index * 8)) & 255u) == 1)
            break;
    if (index == 8) {
        /* The kernel uses PAT entries 0–3 only. Keep their types intact. */
        index = 4;
        high = (high & ~255u) | 1u;
    }
    uint32_t attributes = ((index & 3u) << 3) | ((index & 4u) << 5);
    uint32_t flags = drv_irq_save();
    __asm__ volatile("wbinvd" ::: "memory");
    if (index == 4 && ((pat >> 32) & 255u) != 1) {
        uint32_t cr0, uncached;
        __asm__ volatile("mov %%cr0,%0" : "=r"(cr0));
        uncached = (cr0 | (1u << 30)) & ~(1u << 29);
        __asm__ volatile("mov %0,%%cr0; wbinvd" :: "r"(uncached) : "memory");
        __asm__ volatile("wrmsr" :: "a"(low), "d"(high), "c"(pat_msr) : "memory");
        __asm__ volatile("wbinvd; mov %0,%%cr0" :: "r"(cr0) : "memory");
    }
    for (unsigned region = first; region <= last; region++) {
        uint32_t *table = framebuffer_tables[region - first];
        for (unsigned i = 0; i < 1024; i++) {
            uint32_t page = (region << 22) | (i << 12);
            table[i] = page | 3u | ((page >= start && page < end) ? attributes : 0);
        }
        for (unsigned i = 0; i < TASK_MAX; i++)
            if (!i || tasks[i].directory)
                directories[i][region] = (uint32_t)table | 3u;
    }
    __asm__ volatile("mov %0,%%cr3" :: "r"(task_current()->directory) : "memory");
    framebuffer_start = start;
    framebuffer_end = end;
    drv_irq_restore(flags);
    return true;
}

static int physical_map(uint32_t physical, uint32_t bytes, void **out, uint32_t attributes) {
    if (!initialized || !out || !bytes || (uint64_t)physical + bytes > 0x100000000ull)
        return DRV_EINVAL;
    if (framebuffer_end && (uint64_t)physical < framebuffer_end &&
        (uint64_t)physical + bytes > framebuffer_start)
        return DRV_EBUSY;
    uint32_t offset = physical & 4095;
    uint64_t span = (uint64_t)bytes + offset;
    if (span > MMIO_PAGES * 4096u)
        return DRV_ENOSPC;
    unsigned count = (span + 4095) / 4096;
    uint32_t flags = drv_irq_save();
    for (unsigned i = 0; i + count <= MMIO_PAGES; i++) {
        unsigned n = 0;
        while (n < count && !mmio_lengths[i + n])
            n++;
        if (n != count) {
            i += n;
            continue;
        }
        mmio_lengths[i] = count;
        for (unsigned j = 0; j < count; j++) {
            if (j)
                mmio_lengths[i + j] = 0xffff;
            mmio_tables[(i + j) / 1024][(i + j) % 1024] =
                ((physical & ~4095u) + j * 4096u) | attributes; /* supervisor-only physical mapping */
            __asm__ volatile("invlpg (%0)" ::"r"(MMIO_BASE + (i + j) * 4096u) : "memory");
        }
        *out = (void *)(MMIO_BASE + i * 4096u + offset);
        drv_irq_restore(flags);
        return 0;
    }
    drv_irq_restore(flags);
    return DRV_ENOSPC;
}
int task_mmio_map(uint32_t physical,uint32_t bytes,void **out) {
    return physical_map(physical,bytes,out,0x1b);
}
int task_physical_map_cached(uint32_t physical,uint32_t bytes,void **out) {
    return physical_map(physical,bytes,out,3);
}
int task_mmio_unmap(void *address, uint32_t bytes) {
    uintptr_t p = (uintptr_t)address;
    if (p < MMIO_BASE || p >= MMIO_BASE + MMIO_PAGES * 4096u || !bytes)
        return DRV_EINVAL;
    unsigned i = (p - MMIO_BASE) / 4096;
    unsigned count = ((uint64_t)bytes + (p & 4095) + 4095) / 4096;
    uint32_t flags = drv_irq_save();
    if (!count || count > MMIO_PAGES - i || mmio_lengths[i] != count) {
        drv_irq_restore(flags);
        return DRV_EINVAL;
    }
    for (unsigned j = 0; j < count; j++) {
        mmio_tables[(i + j) / 1024][(i + j) % 1024] = 0;
        mmio_lengths[i + j] = 0;
        __asm__ volatile("invlpg (%0)" ::"r"(MMIO_BASE + (i + j) * 4096u) : "memory");
    }
    drv_irq_restore(flags);
    return 0;
}
