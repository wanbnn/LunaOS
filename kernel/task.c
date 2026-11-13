#include "update.h"
#include "task.h"
#include "elf32.h"
#include "driver.h"
#include "heap.h"
#include "memory.h"
#include "boot.h"
#include "web.h" /* Existing verified BearSSL/TCP transport, still singleton. */
#define KSTACK_BYTES (128 * 1024)
#define SHELL_STACK_BYTES (1024 * 1024)
#define STACK_MAGIC 0x51ac7a5bu
Task tasks[TASK_MAX];
int current_task;
static bool initialized, idling;
static uint32_t next_pid = 1, stamp, sample_stamp, sample_tick;
static uint32_t directories[TASK_MAX][1024] __attribute__((aligned(4096)));
static uint32_t user_tables[TASK_MAX][1024] __attribute__((aligned(4096)));
/* Up to 8 MiB application image plus a third PDE for the guarded ELF stack. */
static uint32_t user_large_tables[TASK_MAX][1024] __attribute__((aligned(4096)));
static uint32_t user_stack_tables[TASK_MAX][1024] __attribute__((aligned(4096)));
/* A maximum of 2 MiB of demand-allocated 4 KiB pages per native process.
 * Only physical handles, never heap page content, are stored in kernel BSS. */
#define EXEC_HEAP_LIMIT (2u * 1024u * 1024u)
#define EXEC_HEAP_MAX_PAGES (EXEC_HEAP_LIMIT / LUNA_ELF_PAGE_BYTES)
static PmmAllocation exec_heap_pages[TASK_MAX][EXEC_HEAP_MAX_PAGES];
static uint8_t user_code[4096] __attribute__((aligned(4096)));
/* ELF image pages are allocated per process through the physical allocator. */
static uint8_t user_stacks[TASK_MAX][USER_STACK_BYTES] __attribute__((aligned(4096)));
static uint8_t kernel_stacks[TASK_MAX][KSTACK_BYTES] __attribute__((aligned(4096), section(".bss.stacks")));
static uint8_t shell_stack[SHELL_STACK_BYTES] __attribute__((aligned(4096)));
#define EXEC_PIPE_COUNT 16u
#define EXEC_PIPE_CAP 4096u
typedef struct {
    uint8_t buffer[EXEC_PIPE_CAP];
    uint32_t length;
    uint16_t readers, writers;
    bool used;
} ExecPipe;
static ExecPipe exec_pipes[EXEC_PIPE_COUNT];
/* The transport backend is global: at most one guest stream can own it. */
static uint32_t exec_net_owner;
static void exec_close_fd(Task *t, unsigned fd) {
    if (fd >= EXEC_FD_MAX || !t->exec_fds[fd].open)
        return;
    unsigned kind = t->exec_fds[fd].kind, id = t->exec_fds[fd].pipe_id;
    if (kind == 3u || kind == 4u) {
        if (exec_net_owner == t->pid) {
            if (kind == 4u) tls_close();
            else stream_close();
            exec_net_owner = 0;
        }
    } else if ((kind == 1u || kind == 2u) && id < EXEC_PIPE_COUNT) {
        ExecPipe *p = &exec_pipes[id];
        if (p->used) {
            if (kind == 1u && p->readers) p->readers--;
            if (kind == 2u && p->writers) p->writers--;
            if (!p->readers && !p->writers)
                memset(p, 0, sizeof(*p));
        }
    }
    memset(&t->exec_fds[fd], 0, sizeof(t->exec_fds[fd]));
}

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
    return t->ring ? t->stack_bytes+USER_STACK_BYTES+sizeof(directories[0])+sizeof(user_tables[0])+
           (t->exec_image ? sizeof(user_large_tables[0])+sizeof(user_stack_tables[0])+
                             t->exec_physical_bytes : 0u) : 0;
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
        uint32_t base = ring ? USER_CODE : 0;
        uint32_t limit = ring == 3 ?
            (LUNA_ELF_STACK_OFFSET + USER_STACK_BYTES - 1u) >> 12 :
            (ring ? 0x13fffu : 0xfffffu);
        uint8_t flags = ring == 3 || ring == 0 ? 0xc : 4;
        gdt[1 + ring * 2] = segment(base, limit, 0x9a | (ring << 5), flags);
        gdt[2 + ring * 2] = segment(base, limit, 0x92 | (ring << 5), flags);
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
                      void (*service)(void), bool defer_start) {
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
    memset(user_large_tables[slot], 0, 4096);
    memset(user_stack_tables[slot], 0, 4096);
    memset(exec_heap_pages[slot], 0, sizeof(exec_heap_pages[slot]));
    directories[slot][USER_CODE >> 22] = (uint32_t)user_tables[slot] | 7;
    if (ring == 3) {
        directories[slot][(USER_CODE >> 22) + 1] = (uint32_t)user_large_tables[slot] | 7;
        directories[slot][(USER_CODE >> 22) + 2] = (uint32_t)user_stack_tables[slot] | 7;
    }
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
    t->state = defer_start ? TASK_WAITING : TASK_READY;
    if (!service)
        gui_damage_all();
    return t->pid;
}
uint32_t task_spawn(int app, const char *name, unsigned ring, unsigned probe) {
    return spawn(app, name, ring, probe, NULL, false);
}
/* Restricted i386 user-mode loader; separate physical pages per process.
 * A GDT base at USER_CODE makes all guest pointers segment-relative. */
static bool prepare_exec_stack(uint8_t *stack, int argc, const char *const argv[],
                               int envc, const char *const envp[], uint32_t *esp_out) {
    if (argc < 1 || argc > 24 || envc < 0 || envc > 16)
        return false;
    unsigned sp = USER_STACK_BYTES;
    uint32_t args[24], env[16];
    for (int group = 0; group < 2; group++) {
        int count = group ? envc : argc;
        const char *const *strings = group ? envp : argv;
        for (int i = count - 1; i >= 0; i--) {
            if (!strings || !strings[i])
                return false;
            size_t len = strlen(strings[i]) + 1u;
            if (len > 256u || sp < len + 256u)
                return false;
            sp -= (unsigned)len;
            memcpy(stack + sp, strings[i], len);
            (group ? env : args)[i] = LUNA_ELF_STACK_OFFSET + sp;
        }
    }
    sp &= ~3u;
    unsigned words = 1u + (unsigned)argc + 1u + (unsigned)envc + 1u + 2u;
    if (sp < words * sizeof(uint32_t) + 16u)
        return false;
    sp = (sp - words * sizeof(uint32_t)) & ~15u;
    uint32_t *p = (uint32_t *)(void *)(stack + sp);
    *p++ = (uint32_t)argc;
    for (int i = 0; i < argc; i++)
        *p++ = args[i];
    *p++ = 0;
    for (int i = 0; i < envc; i++)
        *p++ = env[i];
    *p++ = 0;
    *p++ = 0; /* AT_NULL */
    *p++ = 0;
    *esp_out = LUNA_ELF_STACK_OFFSET + sp;
    return true;
}
uint32_t task_spawn_elf_args(const char *path, int argc, const char *const argv[],
                             int envc, const char *const envp[]) {
    File *f = fs_find(path);
    if (!f || f->dir || f->size < 84u || f->size > LUNA_ELF_MAX_FILE_BYTES)
        return 0;
    /* Only read the ELF and PHDR header; stream PT_LOAD bytes to the PMM.
     * Never stage a 16 MiB Git binary and its virtual image in the heap. */
    uint8_t header[LUNA_ELF_HEADER_BYTES];
    size_t head_bytes = f->size;
    if (head_bytes > sizeof(header))
        head_bytes = sizeof(header);
    if (!fs_read_bytes(f, 0, header, head_bytes))
        return 0;
    LunaElfImage *image = heap_allocate(sizeof(*image));
    if (!image)
        return 0;
    bool accepted = luna_elf32_inspect(header, head_bytes, f->size, image);
    if (!accepted) {
        heap_release(image);
        return 0;
    }
    PmmAllocation allocations[LUNA_ELF_MAX_SEGMENTS] = {{0}};
    uint32_t physical_bytes = 0;
    for (unsigned i = 0; i < image->segments; i++) {
        const LunaElfSegment *seg = &image->load[i];
        uint32_t bytes = seg->page_count * LUNA_ELF_PAGE_BYTES;
        if (memory_allocate(bytes, LUNA_ELF_PAGE_BYTES, UINT32_MAX, 0u,
                            true, true, &allocations[i]))
            goto fail_load;
        physical_bytes += bytes;
        if (seg->file_bytes &&
            !fs_read_bytes(f, seg->file_offset,
                           (uint8_t *)(uintptr_t)allocations[i].address +
                           (seg->address % LUNA_ELF_PAGE_BYTES),
                           seg->file_bytes))
            goto fail_load;
    }
    uint32_t parent_pid = task_current()->pid;
    char parent_cwd[PATH_CAP];
    Task *parent = task_current();
    strcpy(parent_cwd, parent->exec_image ? parent->exec_cwd : cwd);
    uint32_t pid = spawn(-1, "ELF32 native user process", 3, 0, NULL, true);
    if (!pid)
        goto fail_load;
    Task *t = task_by_pid(pid);
    unsigned slot = (unsigned)(t - tasks);
    uint32_t stack_esp = 0;
    if (!prepare_exec_stack(user_stacks[slot], argc, argv, envc, envp,
                            &stack_esp)) {
        t->state = TASK_EXITED;
        goto fail_load;
    }
    /* Remove the legacy stack alias (at 64 KiB). New guest stack lives
     * at an isolated third PDE above the 8 MiB program image. */
    for (unsigned i = 0; i < USER_STACK_BYTES / 4096u; i++) {
        user_tables[slot][16 + i] = 0;
        user_stack_tables[slot][16 + i] =
            ((uint32_t)user_stacks[slot] + i * 4096u) | 7u;
    }
    for (unsigned i = 0; i < image->segments; i++) {
        const LunaElfSegment *seg = &image->load[i];
        for (uint32_t j = 0; j < seg->page_count; j++) {
            uint32_t page = seg->first_page + j;
            uint32_t *page_table = page < 1024u ?
                user_tables[slot] : user_large_tables[slot];
            page_table[page & 1023u] =
                (allocations[i].address + j * LUNA_ELF_PAGE_BYTES) |
                ((seg->flags & LUNA_ELF_WRITE) ? 7u : 5u);
        }
        t->exec_allocation[i] = allocations[i];
        t->exec_segments++;
    }
    memcpy(t->exec_permissions, image->permissions, sizeof(t->exec_permissions));
    t->exec_physical_bytes = physical_bytes;
    uint32_t highest = 0;
    for (unsigned i = 0; i < image->segments; i++) {
        const LunaElfSegment *s = &image->load[i];
        uint32_t end = s->address + s->memory_bytes;
        if (highest < end) highest = end;
    }
    /* Leave one guard page between the final ELF segment and new heap. */
    uint32_t heap_start = ((highest + 4095u) & ~4095u);
    if (heap_start < LUNA_ELF_IMAGE_BYTES - 4096u)
        heap_start += 4096u;
    t->exec_heap_base = t->exec_heap_break = heap_start;
    t->frame->eip = image->entry;
    t->frame->useresp = stack_esp;
    t->parent_pid = parent_pid;
    strcpy(t->exec_cwd, parent_cwd);
    t->exec_image = true;
    /* POSIX-like descriptor inheritance for regular files and pipes.
     * Guest network descriptors are not inherited: the underlying driver
     * is still a single host-wide TCP transport, not a cloneable socket. */
    if (parent->exec_image) {
        uint32_t irq=drv_irq_save();
        for (unsigned i=0;i<EXEC_FD_MAX;i++) {
            if (!parent->exec_fds[i].open) continue;
            unsigned kind=parent->exec_fds[i].kind;
            if (kind==3u || kind==4u) continue;
            t->exec_fds[i]=parent->exec_fds[i];
            if ((kind==1u || kind==2u) &&
                t->exec_fds[i].pipe_id<EXEC_PIPE_COUNT) {
                ExecPipe *p=&exec_pipes[t->exec_fds[i].pipe_id];
                if (p->used) {
                    if (kind==1u) p->readers++;
                    else p->writers++;
                } else t->exec_fds[i].open=false;
            }
        }
        drv_irq_restore(irq);
    }
    heap_release(image);
    t->state=TASK_READY; /* child cannot run until mappings/argv/FDs exist */
    return pid;
fail_load:
    for (unsigned i = 0; i < LUNA_ELF_MAX_SEGMENTS; i++)
        if (allocations[i].cookie && memory_release(&allocations[i]))
            kernel_fault();
    heap_release(image);
    return 0;
}
uint32_t task_spawn_elf(const char *path) {
    const char *a[1] = {path};
    return task_spawn_elf_args(path, 1, a, 0, NULL);
}
/* POSIX-like execve subset: stage the complete new image and stack before
 * changing the calling process. Failed exec leaves the old image intact.
 * On success PID, parentage, cwd and open descriptors are preserved.
 * No interpreter/relocations, shebangs, close-on-exec or signals yet. */
static bool task_execve_elf(Task *t, TrapFrame *frame, const char *path,
                            int argc, const char *const argv[],
                            int envc, const char *const envp[]) {
    if (!t->exec_image || t->ring != 3)
        return false;
    uint8_t *new_stack = heap_allocate(USER_STACK_BYTES);
    if (!new_stack)
        return false;
    memset(new_stack, 0, USER_STACK_BYTES);
    uint32_t new_esp = 0;
    if (!prepare_exec_stack(new_stack, argc, argv, envc, envp, &new_esp)) {
        heap_release(new_stack);
        return false;
    }
    File *file = fs_find(path);
    if (!file || file->dir || file->size < 84u ||
        file->size > LUNA_ELF_MAX_FILE_BYTES) {
        heap_release(new_stack);
        return false;
    }
    uint8_t head[LUNA_ELF_HEADER_BYTES];
    size_t bytes = file->size < sizeof(head) ? file->size : sizeof(head);
    LunaElfImage *image = heap_allocate(sizeof(*image));
    if (!image) {
        heap_release(new_stack);
        return false;
    }
    if (!fs_read_bytes(file, 0, head, bytes) ||
        !luna_elf32_inspect(head, bytes, file->size, image)) {
        heap_release(image);
        heap_release(new_stack);
        return false;
    }
    PmmAllocation fresh[LUNA_ELF_MAX_SEGMENTS] = {{0}};
    uint32_t physical_bytes = 0;
    bool loaded = true;
    for (unsigned i = 0; i < image->segments; i++) {
        const LunaElfSegment *s = &image->load[i];
        uint32_t n = s->page_count * LUNA_ELF_PAGE_BYTES;
        if (memory_allocate(n, LUNA_ELF_PAGE_BYTES, UINT32_MAX, 0u,
                            true, true, &fresh[i])) {
            loaded = false;
            break;
        }
        physical_bytes += n;
        if (s->file_bytes &&
            !fs_read_bytes(file, s->file_offset,
                           (uint8_t *)(uintptr_t)fresh[i].address +
                           (s->address % LUNA_ELF_PAGE_BYTES), s->file_bytes)) {
            loaded = false;
            break;
        }
    }
    if (!loaded) {
        for (unsigned i = 0; i < LUNA_ELF_MAX_SEGMENTS; i++)
            if (fresh[i].cookie && memory_release(&fresh[i]))
                kernel_fault();
        heap_release(image);
        heap_release(new_stack);
        return false;
    }
    /* Commit point. The current trap frame stays on a supervisor stack.
     * Interrupts are disabled while the active CR3 mappings change. */
    uint32_t irq = drv_irq_save();
    unsigned slot = (unsigned)(t - tasks);
    PmmAllocation previous[LUNA_ELF_MAX_SEGMENTS];
    unsigned old_count = t->exec_segments;
    memcpy(previous, t->exec_allocation, sizeof(previous));
    memset(user_tables[slot], 0, sizeof(user_tables[slot]));
    memset(user_large_tables[slot], 0, sizeof(user_large_tables[slot]));
    for (unsigned i = 0; i < USER_STACK_BYTES / 4096u; i++)
        user_stack_tables[slot][16 + i] =
            ((uint32_t)user_stacks[slot] + i * 4096u) | 7u;
    memcpy(user_stacks[slot], new_stack, USER_STACK_BYTES);
    memset(t->exec_allocation, 0, sizeof(t->exec_allocation));
    for (unsigned i = 0; i < image->segments; i++) {
        const LunaElfSegment *s = &image->load[i];
        for (uint32_t j = 0; j < s->page_count; j++) {
            uint32_t page = s->first_page + j;
            uint32_t *table = page < 1024u ?
                user_tables[slot] : user_large_tables[slot];
            table[page & 1023u] =
                (fresh[i].address + j * LUNA_ELF_PAGE_BYTES) |
                ((s->flags & LUNA_ELF_WRITE) ? 7u : 5u);
        }
        t->exec_allocation[i] = fresh[i];
    }
    t->exec_segments = image->segments;
    t->exec_physical_bytes = physical_bytes;
    unsigned slot_for_heap = (unsigned)(t - tasks);
    for (unsigned i=0;i<t->exec_heap_pages;i++) {
        /* Already detached from the page table by the image swap. */
        if (exec_heap_pages[slot_for_heap][i].cookie &&
            memory_release(&exec_heap_pages[slot_for_heap][i]))
            kernel_fault();
        exec_heap_pages[slot_for_heap][i] = (PmmAllocation){0};
    }
    t->exec_heap_pages = 0;
    uint32_t highest = 0;
    for (unsigned i=0;i<image->segments;i++) {
        const LunaElfSegment *s=&image->load[i];
        uint32_t end=s->address+s->memory_bytes;
        if (end>highest) highest=end;
    }
    uint32_t heap_start=(highest+4095u)&~4095u;
    if (heap_start < LUNA_ELF_IMAGE_BYTES - 4096u)
        heap_start+=4096u;
    t->exec_heap_base=t->exec_heap_break=heap_start;
    memcpy(t->exec_permissions, image->permissions, sizeof(t->exec_permissions));
    /* The return from int 0x80 enters _start of the replacement program,
     * never the old instruction after execve. */
    frame->eip = image->entry;
    frame->useresp = new_esp;
    frame->eax = frame->ebx = frame->ecx = frame->edx = 0;
    frame->esi = frame->edi = frame->ebp = 0;
    frame->eflags = 0x202u;
    __asm__ volatile("mov %0,%%cr3" :: "r"(t->directory) : "memory");
    for (unsigned i = 0; i < old_count; i++)
        if (previous[i].cookie && memory_release(&previous[i]))
            kernel_fault();
    drv_irq_restore(irq);
    heap_release(image);
    heap_release(new_stack);
    return true;
}
bool task_wait_elf(uint32_t pid, uint32_t timeout_ms, unsigned *exit_code) {
    uint32_t parent = task_current()->pid, started = frame_clock;
    for (;;) {
        Task *t = task_by_pid(pid);
        if (!t || !t->exec_image || t->parent_pid != parent)
            return false;
        if (t->state == TASK_EXITED) {
            if (exit_code) *exit_code = t->exit_code;
            return true;
        }
        if ((uint32_t)(frame_clock - started) >= timeout_ms)
            return false;
        task_sleep_ms(10);
    }
}
uint32_t task_spawn_service(const char *name, void (*service)(void)) {
    return spawn(-1, name, 3, 0, service, false);
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
    if (t->exec_image)
        for (unsigned fd = 0; fd < EXEC_FD_MAX; fd++)
            exec_close_fd(t, fd);
    for (unsigned i = 0; i < t->exec_segments; i++) {
        if (t->exec_allocation[i].cookie &&
            memory_release(&t->exec_allocation[i]))
            kernel_fault();
        t->exec_allocation[i] = (PmmAllocation){0};
    }
    t->exec_segments = 0;
    t->exec_physical_bytes = 0;
    unsigned slot=(unsigned)(t-tasks);
    for (unsigned i=0;i<t->exec_heap_pages;i++)
        if (exec_heap_pages[slot][i].cookie &&
            memory_release(&exec_heap_pages[slot][i]))
            kernel_fault();
    t->exec_heap_pages=0;
    t->exec_heap_break=t->exec_heap_base=0;
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
/* Userspace addresses are offsets under the USER_CODE segment base.
 * The kernel cannot safely dereference holes in the PTE map. Validate all
 * touched pages before copying any bytes (also protects supervisor code). */
static bool exec_user_range(const Task *t, uint32_t off, uint32_t n, bool writable) {
    if (!t->exec_image || t->ring != 3)
        return false;
    if (off >= LUNA_ELF_STACK_OFFSET &&
        off < LUNA_ELF_STACK_OFFSET + USER_STACK_BYTES)
        return n <= LUNA_ELF_STACK_OFFSET + USER_STACK_BYTES - off;
    if (off >= LUNA_ELF_IMAGE_BYTES || n > LUNA_ELF_IMAGE_BYTES - off)
        return false;
    if (!n)
        return true;
    uint32_t first = off >> 12, last = (off + n - 1u) >> 12;
    for (uint32_t i = first; i <= last; i++)
        if (!(t->exec_permissions[i] & LUNA_ELF_READ) ||
            (writable && !(t->exec_permissions[i] & LUNA_ELF_WRITE)))
            return false;
    return true;
}
static bool exec_string(Task *t, uint32_t addr, char *out, size_t max) {
    if (!max)
        return false;
    for (size_t i = 0; i < max; i++) {
        if (!exec_user_range(t, addr, (uint32_t)(i + 1u), false))
            return false;
        char c = *(const char *)(uintptr_t)(USER_CODE + addr + (uint32_t)i);
        out[i] = c;
        if (!c)
            return i > 0;
    }
    return false;
}
static bool exec_path(Task *t, const char *path, char out[PATH_CAP]) {
    if (*path == '/')
        return fs_path(path, out);
    char joined[256];
    if (strlen(t->exec_cwd) + strlen(path) + 2u > sizeof(joined))
        return false;
    strcpy(joined, t->exec_cwd);
    strcat(joined, "/");
    strcat(joined, path);
    return fs_path(joined, out);
}
/* Copy guest argv/envp arrays before creating another task. Reject
 * pointers outside mapped pages and non-terminated/oversized vectors. */
static bool exec_guest_vector(Task *t, uint32_t offsets, char strings[][256],
                              const char **out, unsigned limit, int *argc) {
    if (!offsets) {
        *argc = 0;
        return true;
    }
    for (unsigned i = 0; i <= limit; i++) {
        uint32_t at = offsets + i * 4u, ptr;
        if (at < offsets || !exec_user_range(t, at, 4u, false))
            return false;
        memcpy(&ptr, (const void *)(uintptr_t)(USER_CODE + at), 4u);
        if (!ptr) {
            *argc = (int)i;
            return true;
        }
        if (i == limit || !exec_string(t, ptr, strings[i], 256u))
            return false;
        out[i] = strings[i];
    }
    return false;
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
        uint32_t fd = f->ebx, pos = f->ecx, length = f->edx;
        if (length > 4096u || !exec_user_range(t, pos, length, false)) {
            f->eax = (uint32_t)-1;
            break;
        }
        if (fd == 1 || fd == 2) {
            /* Terminal currently prints bounded text, not arbitrary binary. */
            if (length > 256u) { f->eax = (uint32_t)-1; break; }
            char buffer[257];
            memcpy(buffer, (const void *)(uintptr_t)(USER_CODE + pos), length);
            buffer[length] = 0;
            if (strlen(buffer) != length) { f->eax = (uint32_t)-1; break; }
            shell_print(buffer);
            f->eax = length;
            break;
        }
        if (fd < 3 || fd >= 3 + EXEC_FD_MAX ||
            !t->exec_fds[fd - 3].open || !(t->exec_fds[fd - 3].flags & 3u)) {
            f->eax = (uint32_t)-1;
            break;
        }
        if (t->exec_fds[fd - 3].kind) {
            unsigned kind = t->exec_fds[fd - 3].kind;
            unsigned id = t->exec_fds[fd - 3].pipe_id;
            if (kind == 3u || kind == 4u) {
                int wrote = exec_net_owner == t->pid ?
                    (kind == 4u ?
                     tls_write((const void *)(uintptr_t)(USER_CODE + pos), length) :
                     stream_write((const void *)(uintptr_t)(USER_CODE + pos), length))
                    : -1;
                f->eax = wrote < 0 ? (uint32_t)-1 : (uint32_t)wrote;
                break;
            }
            if (kind != 2u || id >= EXEC_PIPE_COUNT || !exec_pipes[id].used ||
                !exec_pipes[id].readers) {
                f->eax = (uint32_t)-1;
                break;
            }
            /* A bounded, in-kernel ring buffer; no blocking semantics yet. */
            uint32_t irq = drv_irq_save();
            ExecPipe *p = &exec_pipes[id];
            if (length > EXEC_PIPE_CAP - p->length) {
                f->eax = (uint32_t)-1;
            } else {
                memcpy(p->buffer + p->length,
                       (const void *)(uintptr_t)(USER_CODE + pos), length);
                p->length += length;
                f->eax = length;
            }
            drv_irq_restore(irq);
            break;
        }
        const char *path = t->exec_fds[fd - 3].path;
        File *file = fs_find(path);
        uint32_t before = file ? file->size : 0u;
        if (!file || file->dir ||
            (!(t->exec_fds[fd - 3].flags & 1024u) &&
             t->exec_fds[fd - 3].offset != before) ||
            fs_write_bytes(path, (const void *)(uintptr_t)(USER_CODE + pos), length, true)) {
            f->eax = (uint32_t)-1;
            break;
        }
        /* fs_write_bytes can replace directory storage: do not dereference file. */
        t->exec_fds[fd - 3].offset = before + length;
        f->eax = length;
        break;
    }
    case SYS_EXEC_OPEN: {
        uint32_t addr = f->ebx, flags = f->ecx;
        /* Linux i386 O_{RDONLY,WRONLY,RDWR,CREAT,TRUNC,APPEND} subset. */
        if (flags & ~(3u | 64u | 512u | 1024u) || (flags & 3u) == 3u ||
            ((flags & 3u) && !(flags & (512u | 1024u))) ||
            ((flags & (64u | 512u | 1024u)) && !(flags & 3u))) {
            f->eax = (uint32_t)-1;
            break;
        }
        char raw[PATH_CAP], path[PATH_CAP];
        if (!exec_string(t, addr, raw, sizeof(raw)) || !exec_path(t, raw, path)) {
            f->eax = (uint32_t)-1;
            break;
        }
        File *file = fs_find(path);
        if ((!file && !(flags & 64u)) || (file && file->dir)) {
            f->eax = (uint32_t)-1;
            break;
        }
        unsigned slot = 0;
        for (; slot < EXEC_FD_MAX; slot++)
            if (!t->exec_fds[slot].open)
                break;
        if (slot == EXEC_FD_MAX) { f->eax = (uint32_t)-1; break; }
        if ((flags & 512u) || (!file && (flags & 64u))) {
            if (fs_write_bytes(path, "", 0, false)) {
                f->eax = (uint32_t)-1; break;
            }
            file = fs_find(path);
        }
        if (!file) { f->eax = (uint32_t)-1; break; }
        strcpy(t->exec_fds[slot].path, path);
        t->exec_fds[slot].offset = (flags & 1024u) ? file->size : 0;
        t->exec_fds[slot].flags = flags;
        t->exec_fds[slot].open = true;
        f->eax = slot + 3u;
        break;
    }
    case SYS_EXEC_READ: {
        uint32_t fd = f->ebx, addr = f->ecx, length = f->edx;
        if (length > 4096u || !exec_user_range(t, addr, length, true) ||
            fd < 3 || fd >= 3 + EXEC_FD_MAX ||
            !t->exec_fds[fd - 3].open ||
            (t->exec_fds[fd - 3].flags & 3u) == 1u) {
            f->eax = (uint32_t)-1;
            break;
        }
        if (t->exec_fds[fd - 3].kind) {
            unsigned kind = t->exec_fds[fd - 3].kind;
            unsigned id = t->exec_fds[fd - 3].pipe_id;
            if (kind == 3u || kind == 4u) {
                int n = exec_net_owner == t->pid ?
                    (kind == 4u ?
                     tls_read((void *)(uintptr_t)(USER_CODE + addr), length) :
                     stream_read((void *)(uintptr_t)(USER_CODE + addr), length))
                    : -1;
                f->eax = n < 0 ? (uint32_t)-1 : (uint32_t)n;
                break;
            }
            if (kind != 1u || id >= EXEC_PIPE_COUNT || !exec_pipes[id].used) {
                f->eax = (uint32_t)-1;
                break;
            }
            uint32_t irq = drv_irq_save();
            ExecPipe *p = &exec_pipes[id];
            if (!p->length && p->writers) {
                f->eax = (uint32_t)-1; /* Would block; no errno yet. */
            } else {
                size_t n = p->length < length ? p->length : length;
                memcpy((void *)(uintptr_t)(USER_CODE + addr), p->buffer, n);
                memmove(p->buffer, p->buffer + n, p->length - n);
                p->length -= (uint32_t)n;
                f->eax = (uint32_t)n;
            }
            drv_irq_restore(irq);
            break;
        }
        File *file = fs_find(t->exec_fds[fd - 3].path);
        if (!file || file->dir) { f->eax = (uint32_t)-1; break; }
        uint32_t offset = t->exec_fds[fd - 3].offset;
        if (offset >= file->size) { f->eax = 0; break; } /* EOF is not an error. */
        size_t amount = file->size - offset;
        if (amount > length) amount = length;
        if (amount && !fs_read_bytes(file, offset, (void *)(uintptr_t)(USER_CODE + addr), amount)) {
            f->eax = (uint32_t)-1; break;
        }
        t->exec_fds[fd - 3].offset = offset + (uint32_t)amount;
        f->eax = (uint32_t)amount;
        break;
    }
    case SYS_EXEC_CLOSE: {
        uint32_t fd = f->ebx;
        if (!t->exec_image || t->ring != 3 || fd < 3 || fd >= 3 + EXEC_FD_MAX ||
            !t->exec_fds[fd - 3].open) {
            f->eax = (uint32_t)-1; break;
        }
        exec_close_fd(t, fd - 3);
        f->eax = 0;
        break;
    }
    case SYS_EXEC_LSEEK: {
        uint32_t fd = f->ebx, whence = f->edx;
        int32_t delta = (int32_t)f->ecx;
        if (!t->exec_image || fd < 3 || fd >= 3 + EXEC_FD_MAX ||
            !t->exec_fds[fd - 3].open || whence > 2u) {
            f->eax = (uint32_t)-1; break;
        }
        File *file = fs_find(t->exec_fds[fd - 3].path);
        if (!file || file->dir) { f->eax = (uint32_t)-1; break; }
        int64_t base = whence == 0u ? 0 : whence == 1u ?
            t->exec_fds[fd - 3].offset : file->size;
        int64_t wanted = base + delta;
        if (wanted < 0 || wanted > INT32_MAX ||
            ((t->exec_fds[fd - 3].flags & 3u) && (uint32_t)wanted != file->size)) {
            f->eax = (uint32_t)-1; break;
        }
        t->exec_fds[fd - 3].offset = (uint32_t)wanted;
        f->eax = (uint32_t)wanted;
        break;
    }
    case SYS_EXEC_STAT: {
        char raw[PATH_CAP], path[PATH_CAP];
        uint32_t addr = f->ecx; /* ebx: NUL path; ecx: 20-byte LunaGuestStat */
        if (!exec_string(t, f->ebx, raw, sizeof(raw)) ||
            !exec_path(t, raw, path) || !exec_user_range(t, addr, 20u, true)) {
            f->eax = (uint32_t)-1; break;
        }
        File *file = fs_find(path);
        if (!file) { f->eax = (uint32_t)-1; break; }
        uint32_t value[5] = {file->size,
                             file->mode ? file->mode : (file->dir ? 0040755u : 0100644u),
                             0, file->mtime, file->ctime};
        memcpy((void *)(uintptr_t)(USER_CODE + addr), value, sizeof(value));
        f->eax = 0;
        break;
    }
    case SYS_EXEC_CHDIR: {
        char raw[PATH_CAP], path[PATH_CAP];
        if (!exec_string(t, f->ebx, raw, sizeof(raw)) || !exec_path(t, raw, path)) {
            f->eax = (uint32_t)-1; break;
        }
        File *file = fs_find(path);
        if (!file || !file->dir) { f->eax = (uint32_t)-1; break; }
        strcpy(t->exec_cwd, path);
        f->eax = 0;
        break;
    }
    case SYS_EXEC_GETCWD: {
        uint32_t addr = f->ebx, capacity = f->ecx;
        size_t len = strlen(t->exec_cwd) + 1u;
        if (!capacity || len > capacity || !exec_user_range(t, addr, (uint32_t)len, true)) {
            f->eax = (uint32_t)-1; break;
        }
        memcpy((void *)(uintptr_t)(USER_CODE + addr), t->exec_cwd, len);
        f->eax = addr;
        break;
    }
    case SYS_EXEC_MKDIR:
    case SYS_EXEC_UNLINK: {
        char raw[PATH_CAP], path[PATH_CAP];
        if (!exec_string(t, f->ebx, raw, sizeof(raw)) || !exec_path(t, raw, path)) {
            f->eax = (uint32_t)-1; break;
        }
        int rc = call == SYS_EXEC_MKDIR ? fs_create(path, true) : fs_remove(path);
        f->eax = rc ? (uint32_t)-1 : 0;
        break;
    }
    case SYS_EXEC_RENAME: {
        char raw1[PATH_CAP], raw2[PATH_CAP], path1[PATH_CAP], path2[PATH_CAP];
        if (!exec_string(t, f->ebx, raw1, sizeof(raw1)) ||
            !exec_string(t, f->ecx, raw2, sizeof(raw2)) ||
            !exec_path(t, raw1, path1) || !exec_path(t, raw2, path2)) {
            f->eax = (uint32_t)-1; break;
        }
        f->eax = fs_rename(path1, path2) ? (uint32_t)-1 : 0;
        break;
    }
    case SYS_EXEC_GETPID:
        f->eax = t->exec_image ? t->pid : (uint32_t)-1;
        break;
    case SYS_EXEC_PIPE: {
        uint32_t user_array = f->ebx;
        if (!exec_user_range(t, user_array, sizeof(uint32_t) * 2u, true)) {
            f->eax = (uint32_t)-1; break;
        }
        unsigned rd = EXEC_FD_MAX, wr = EXEC_FD_MAX, pipe = EXEC_PIPE_COUNT;
        for (unsigned i = 0; i < EXEC_FD_MAX; i++)
            if (!t->exec_fds[i].open) {
                if (rd == EXEC_FD_MAX) rd = i;
                else { wr = i; break; }
            }
        for (unsigned i = 0; i < EXEC_PIPE_COUNT; i++)
            if (!exec_pipes[i].used) { pipe = i; break; }
        if (wr == EXEC_FD_MAX || pipe == EXEC_PIPE_COUNT) {
            f->eax = (uint32_t)-1; break;
        }
        ExecPipe *p = &exec_pipes[pipe];
        memset(p, 0, sizeof(*p));
        p->used = true; p->readers = 1; p->writers = 1;
        t->exec_fds[rd].open = true;
        t->exec_fds[rd].kind = 1; t->exec_fds[rd].pipe_id = (uint8_t)pipe;
        t->exec_fds[wr].open = true;
        t->exec_fds[wr].kind = 2; t->exec_fds[wr].pipe_id = (uint8_t)pipe;
        t->exec_fds[wr].flags = 1;
        uint32_t result[2] = {rd + 3u, wr + 3u};
        memcpy((void *)(uintptr_t)(USER_CODE + user_array), result, sizeof(result));
        f->eax = 0;
        break;
    }
    case SYS_EXEC_FSYNC: {
        uint32_t fd = f->ebx;
        if (!t->exec_image || fd < 3u || fd >= 3u + EXEC_FD_MAX ||
            !t->exec_fds[fd - 3u].open || t->exec_fds[fd - 3u].kind) {
            f->eax = (uint32_t)-1; break;
        }
        /* LunaFS2 persists the entire generation atomically, not just a
         * single inode. Surface snapshot/flush errors to the caller. */
        f->eax = fs_save() ? 0u : (uint32_t)-1;
        break;
    }
    case SYS_EXEC_SPAWN: {
        char raw[PATH_CAP], path[PATH_CAP];
        if (!exec_string(t, f->ebx, raw, sizeof(raw)) ||
            !exec_path(t, raw, path)) {
            f->eax = (uint32_t)-1; break;
        }
        char arg_storage[24][256], env_storage[16][256];
        const char *arg_vector[24], *env_vector[16];
        int argc = 0, envc = 0;
        if (!exec_guest_vector(t, f->ecx, arg_storage, arg_vector, 24, &argc) ||
            !argc ||
            !exec_guest_vector(t, f->edx, env_storage, env_vector, 16, &envc)) {
            f->eax = (uint32_t)-1; break;
        }
        uint32_t pid = task_spawn_elf_args(path, argc, arg_vector, envc, env_vector);
        f->eax = pid ? pid : (uint32_t)-1;
        break;
    }
    case SYS_EXEC_OPENDIR: {
        char raw[PATH_CAP], path[PATH_CAP];
        if (!exec_string(t, f->ebx, raw, sizeof(raw)) ||
            !exec_path(t, raw, path)) {
            f->eax = (uint32_t)-1; break;
        }
        File *directory = fs_find(path);
        if (!directory || !directory->dir) {
            f->eax = (uint32_t)-1; break;
        }
        unsigned slot=0;
        for (; slot<EXEC_FD_MAX; slot++)
            if (!t->exec_fds[slot].open) break;
        if (slot==EXEC_FD_MAX) {f->eax=(uint32_t)-1;break;}
        strcpy(t->exec_fds[slot].path, path);
        t->exec_fds[slot].flags = 0u;
        t->exec_fds[slot].offset = 0u;
        t->exec_fds[slot].kind = 5u; /* directory cursor */
        t->exec_fds[slot].open = true;
        f->eax = slot+3u;
        break;
    }
    case SYS_EXEC_BRK: {
        /* ABI: brk(0) queries current end. Successful calls return requested
         * end, failures return old end. Physical memory is allocated only
         * for touched pages. The heap cannot exceed 2 MiB or the 8 MiB
         * program VA span. */
        uint32_t request=f->ebx;
        uint32_t base=t->exec_heap_base;
        if (!t->exec_image || !base) {
            f->eax=0; break;
        }
        if (!request) {
            f->eax=t->exec_heap_break; break;
        }
        if (request<base || request>LUNA_ELF_IMAGE_BYTES ||
            request-base>EXEC_HEAP_LIMIT) {
            f->eax=t->exec_heap_break; break;
        }
        uint32_t want=(request-base+4095u)/4096u;
        unsigned slot=(unsigned)(t-tasks), old=t->exec_heap_pages;
        if (want>old) {
            unsigned j=old;
            for (;j<want;j++)
                if (memory_allocate(4096u,4096u,UINT32_MAX,0u,true,true,
                                    &exec_heap_pages[slot][j]))
                    break;
            if (j!=want) {
                for (unsigned k=old;k<j;k++)
                    if (memory_release(&exec_heap_pages[slot][k]))
                        kernel_fault();
                f->eax=t->exec_heap_break;
                break;
            }
        }
        uint32_t irq=drv_irq_save();
        for (unsigned i=old;i<want;i++) {
            uint32_t v=base+i*4096u, page=v>>12;
            uint32_t *table=page<1024u ?
                user_tables[slot] : user_large_tables[slot];
            table[page&1023u]=exec_heap_pages[slot][i].address|7u;
            t->exec_permissions[page]=LUNA_ELF_READ|LUNA_ELF_WRITE;
        }
        for (unsigned i=want;i<old;i++) {
            uint32_t page=(base+i*4096u)>>12;
            uint32_t *table=page<1024u ?
                user_tables[slot] : user_large_tables[slot];
            table[page&1023u]=0;
            t->exec_permissions[page]=0;
        }
        __asm__ volatile("mov %0,%%cr3"::"r"(t->directory):"memory");
        drv_irq_restore(irq);
        if (want<old) {
            for (unsigned i=want;i<old;i++) {
                if (memory_release(&exec_heap_pages[slot][i]))
                    kernel_fault();
                exec_heap_pages[slot][i]=(PmmAllocation){0};
            }
        }
        t->exec_heap_pages=want;
        t->exec_heap_break=request;
        f->eax=request;
        break;
    }
    case SYS_EXEC_READDIR: {
        uint32_t fd=f->ebx, ptr=f->ecx;
        /* LunaGuestDirent: inode, mode, mtime, name[128] (140 bytes). */
        if (!t->exec_image || fd<3u || fd>=3u+EXEC_FD_MAX ||
            !t->exec_fds[fd-3u].open || t->exec_fds[fd-3u].kind!=5u ||
            !exec_user_range(t,ptr,140u,true)) {
            f->eax=(uint32_t)-1; break;
        }
        const char *base=t->exec_fds[fd-3u].path;
        size_t nbase=strlen(base);
        unsigned at=t->exec_fds[fd-3u].offset;
        f->eax=0u; /* end of directory */
        for (;at<(unsigned)fs_file_capacity;at++) {
            File *file=&files[at];
            if (!file->used) continue;
            const char *name=0;
            if (nbase==1u && base[0]=='/' && file->path[0]=='/')
                name=file->path+1u;
            else if (!strncmp(file->path,base,nbase) &&
                     file->path[nbase]=='/')
                name=file->path+nbase+1u;
            if (!name || !*name || strchr(name,'/')) continue;
            struct {
                uint32_t ino, mode, mtime;
                char name[128];
            } entry = {0};
            entry.ino=at+1u; /* currently snapshot-relative, not stable inode */
            entry.mode=file->mode ? file->mode :
                (file->dir ? 0040755u : 0100644u);
            entry.mtime=file->mtime;
            strcpy(entry.name,name);
            memcpy((void *)(uintptr_t)(USER_CODE+ptr),&entry,sizeof(entry));
            t->exec_fds[fd-3u].offset=at+1u;
            f->eax=1u;
            break;
        }
        if (f->eax==0u)
            t->exec_fds[fd-3u].offset=(uint32_t)fs_file_capacity;
        break;
    }
    case SYS_EXEC_TCP_CONNECT: {
        char host[128];
        uint32_t port = f->ecx, secure = f->edx;
        if (!t->exec_image || !exec_string(t, f->ebx, host, sizeof(host)) ||
            !port || port > 65535u || secure > 1u) {
            f->eax = (uint32_t)-1; break;
        }
        unsigned slot = 0;
        for (; slot < EXEC_FD_MAX; slot++)
            if (!t->exec_fds[slot].open) break;
        if (slot == EXEC_FD_MAX) {
            f->eax = (uint32_t)-1; break;
        }
        uint32_t irq = drv_irq_save();
        if (exec_net_owner) {
            drv_irq_restore(irq);
            f->eax = (uint32_t)-1;
            break;
        }
        exec_net_owner = t->pid;
        drv_irq_restore(irq);
        if (stream_connect(host, port) < 0) {
            exec_net_owner = 0;
            f->eax = (uint32_t)-1; break;
        }
        if (secure) {
            char error[128] = {0};
            if (!tls_connect(host, error, sizeof(error))) {
                stream_close();
                exec_net_owner = 0;
                f->eax = (uint32_t)-1;
                break;
            }
        }
        t->exec_fds[slot].open = true;
        t->exec_fds[slot].flags = 3u; /* bidirectional stream */
        t->exec_fds[slot].kind = secure ? 4u : 3u;
        f->eax = slot + 3u;
        break;
    }
    case SYS_EXEC_CHMOD:
    case SYS_EXEC_UTIME: {
        char raw[PATH_CAP], path[PATH_CAP];
        if (!exec_string(t, f->ebx, raw, sizeof(raw)) ||
            !exec_path(t, raw, path)) {
            f->eax = (uint32_t)-1; break;
        }
        f->eax = (call == SYS_EXEC_CHMOD ?
                  fs_chmod(path, f->ecx) : fs_set_mtime(path, f->ecx))
                     ? (uint32_t)-1 : 0u;
        break;
    }
    case SYS_EXEC_EXECVE: {
        char raw[PATH_CAP], path[PATH_CAP];
        if (!exec_string(t, f->ebx, raw, sizeof(raw)) ||
            !exec_path(t, raw, path)) {
            f->eax = (uint32_t)-1; break;
        }
        char arg_storage[24][256], env_storage[16][256];
        const char *arg_vector[24], *env_vector[16];
        int argc = 0, envc = 0;
        if (!exec_guest_vector(t, f->ecx, arg_storage, arg_vector, 24, &argc) ||
            !argc ||
            !exec_guest_vector(t, f->edx, env_storage, env_vector, 16, &envc) ||
            !task_execve_elf(t, f, path, argc, arg_vector, envc, env_vector))
            f->eax = (uint32_t)-1;
        /* On success the new program starts when this trap returns. */
        break;
    }
    case SYS_EXEC_WAITPID: {
        uint32_t pid = f->ebx, status = f->ecx, opts = f->edx;
        if (!t->exec_image || opts > 1u ||
            (status && !exec_user_range(t, status, 4u, true))) {
            f->eax = (uint32_t)-1; break;
        }
        Task *child = task_by_pid(pid);
        if (!child || !child->exec_image || child->parent_pid != t->pid) {
            f->eax = (uint32_t)-1; break;
        }
        if (opts == 1u && child->state != TASK_EXITED) {
            f->eax = 0; break; /* Limited WNOHANG support. */
        }
        unsigned code;
        if (!task_wait_elf(pid, 60000u, &code)) {
            f->eax = (uint32_t)-1; break;
        }
        if (status) {
            uint32_t encoded = (code & 255u) << 8;
            memcpy((void *)(uintptr_t)(USER_CODE + status), &encoded, 4u);
        }
        f->eax = pid;
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
