# LunaOS native ELF32 user-mode execution — Milestone 3

**State: validated in QEMU for native ELF programs; not a full POSIX ABI, and
upstream Git 2.56.1 still does NOT execute in the LunaOS guest.**
Implementation is in PR #8 (draft; no main merge).

## What is implemented

- ELF32 i386 little-endian, static ET_EXEC/ET_DYN subset with up to 16 program
  headers, 64 KiB maximum guest program image and 256 KiB maximum ELF file.
- Multiple PT_LOAD segments mapped to **private physical pages per process**;
  permissions validated at load time. Code/data pages cannot overlap, and no
  ELF segment may request simultaneous write+execute permissions.
- Single GDT-relative address range 0..64 KiB for program code/data, an
  existing 16 KiB private stack mapped at 0x10000..0x13fff.
- Standard-style initial i386 stack layout: argc, argv pointers, NULL, envp
  pointers, NULL and AT_NULL terminator. All pointers are segment-relative
  offsets. Up to 24 argv strings and 16 envp strings, at most 255 characters
  each. lunaSH exports HOME, PWD, PATH and set variables without shell secrets.
- Process creation, private page mapping, exit code, resource release on exit,
  bounded child wait (`elfrun FILE [ARGS...]`, `elfwait PID`).
- Basic file descriptors (8 per process), LunaFS2 read/write, seek, metadata,
  mkdir, rename, unlink, chdir, getcwd. Paths are resolved against each
  process's own CWD, not the mutable shell global. Guest data pointers are
  checked against actual page permissions before kernel copy.
- Freestanding **real C guest SDK**: `sdk/guest/crt0.S`,
  `sdk/guest/posix.[ch]` and `sdk/guest/luna32.ld`, with
  `make luna-guest-hello` producing `build/guest/hello.elf`.

## Early LunaOS syscall ABI (int 0x80)

| eax | ABI operation | Arguments (EBX, ECX, EDX) |
|---|---|---|
| 3 | exit | exit code, 0, 0 |
| 6 | write | fd, guest buffer, bytes <= 4096; console fd 1/2 max 256 bytes |
| 7 | open | NUL-terminated path, flags, 0 |
| 8 | read | fd, writable guest buffer, bytes <= 4096 |
| 9 | close | fd, 0, 0 |
| 10 | lseek | fd, signed delta, SEEK_SET(0)/CUR(1)/END(2) |
| 11 | luna_stat | path, pointer to {size,mode,reserved}, 0 |
| 12 | chdir | path, 0, 0 |
| 13 | getcwd | buffer, capacity, 0 |
| 14 | mkdir | path, mode ignored, 0 |
| 15 | unlink | path, 0, 0 |
| 16 | rename | source, destination, 0 |
| 17 | getpid | 0, 0, 0 |

Open flags use Linux i386 O_RDONLY=0, O_WRONLY=1, O_RDWR=2,
O_CREAT=64, O_TRUNC=512, O_APPEND=1024 for compatibility.
Writing currently supports **append-only** and truncated/new files; it is
not a general POSIX pwrite or random-access overwrite. The descriptor API
uses -1 on failure, does not expose `errno`, and does not guarantee the
atomicity, permission bits, symlinks or timestamps expected by upstream Git.
The `stat` structure is a LunaOS-specific three-field record, not POSIX
`struct stat`. File rename uses current LunaFS2 semantics.

### Build and run a native C program

On an Ubuntu development workstation with GCC multilib and binutils:

```sh
make luna-guest-hello
file build/guest/hello.elf
readelf -l build/guest/hello.elf
```

The binary is for LunaOS, not Linux. Transfer it to an installed LunaOS
disk (e.g. with lunaSH `wget` from a local server), then:

```sh
elfrun /tmp/hello.elf Apple Banana
elfwait <PID printed by elfrun>
cat /tmp/guest-c-after.txt
```

The program runs in ring 3, receives the SysV-like argv/env stack and uses
native syscalls to manipulate LunaFS2 without any Linux host relay.

## Test evidence and remaining limitations

- Host parser regression with ASan+UBSan, freestanding i386 compilation,
  full LunaOS ISO build and guest QEMU smoke are in
  `.github/workflows/elf32-exec.yml`.
- Guest QEMU smoke tests exercise a >12 KiB **two-PT_LOAD** ELF program, real
  argv/envp, file writes/renames, process wait and exit status.
- Guest C SDK smoke uses a C program compiled with gcc -m32 -nostdlib. The
  CI must pass before considering the C SDK validated.
- No ELF interpreter, relocations, dynamic libraries or general ET_DYN
  runtime relocation. Maximum memory limits make **Git upstream too large**.
- No general process spawning API, fork, execve, pipes, signals, blocking
  device I/O, user memory allocator, full POSIX stat/mtime/chmod/locks/
  fsync/rename, mmap, libc or sockets yet.
- Hardware i386 without NX cannot enforce non-executable data pages;
  the loader **rejects W+X metadata** and maps code read-only, but this is
  not a hardware-enforced execute-never guarantee. Full isolation and
  robustness require further security review.
- ELF code and data need dedicated virtual memory growth rather than a
  fixed 64 KiB cap, with VM-aware heap, true POSIX VFS and libc before
  compiling/running Git official. The PR remains draft until all full
  guest Git acceptance tests in `tests/git-full-acceptance.md` pass.
