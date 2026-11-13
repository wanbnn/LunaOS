# ELF32 ring-3 execution milestone

**Scope:** first native binary execution path for LunaOS, not yet POSIX or Git upstream.

The `elfrun FILE` lunaSH built-in starts a restricted ELF32 program asynchronously, using CPL 3, a private read-only code page and the already isolated 16KiB user stack. Unsupported or corrupt ELF files are rejected before process creation.

## ELF image format accepted today

- ELF32 little-endian i386; ET_EXEC or ET_DYN; exactly one `PT_LOAD` segment.
- A segment with the execute bit set and no writable bit, `p_vaddr` and entry offset within one 4096-byte page.
- Files up to 4096 bytes; non-zero `p_filesz`, `p_memsz`; `p_filesz <= p_memsz`.
- Entry pointer must lie inside the declared segment. No relocation, shared libraries, stack arguments or dynamic linker.
- The program's virtual pointers are *segment-relative offsets* within the GDT base `USER_CODE = 0x40000000`. It cannot access kernel memory or files through raw pointers.

This is deliberately **not** a loader for a standard Linux executable, and it cannot load the real Git binary yet.

## User/kernel boundary: experimental int 0x80

| eax | Operation | Arguments | Result |
| --- | --- | --- | --- |
| 3 | `exit(code)` | ebx=exit code | Ends task |
| 6 | `console_write(fd,offset,len)` | ebx=1 or 2; ecx=code/stack offset; edx=length <=256 | bytes or -1 |
| 7 | `file_open(path,flags)` | ebx=NUL-terminated code/stack offset; ecx=0 (read-only) | fd >=3 or -1 |
| 8 | `file_read(fd,offset,len)` | ebx=fd; ecx=writable user-stack offset; edx=len <=4096 | bytes or -1 |
| 9 | `file_close(fd)` | ebx=fd | 0 or -1 |

Each task has 8 read-only FDs; file lookup is by canonical LunaFS2 path. A descriptor is not a raw kernel pointer. The sandbox rejects invalid user offsets, pointers crossing mapping boundaries, attempts to write to the executable page, and unsupported file modes. This does not yet implement `fork`, `execve`, `waitpid`, `pipe`, `stat`, `mmap`, write/rename/fsync/locks, symlinks, UTF-8 path semantics or secure per-process CWD.

## Automated validation

`make test-elf32` validates the parser and malformed headers with ASan and UBSan on the host. CI compiles the loader with freestanding i386 flags and builds the complete LunaOS ISO.

`tests/elf32_guest.py` is the **real guest smoke test**: starts QEMU from a disk image, downloads a hand-crafted ELF image to LunaFS2 over HTTP, runs `elfrun /tmp/hello.elf`, checks ring-3 stdout and reads `/home/luna/welcome.txt` through the new file syscalls. Run it only once QEMU is installed and `make all` has succeeded.

### Blocking requirements before upstream Git can execute

Increase per-process address space beyond a single RX page and static stack; implement ELF multi-segment loading, dynamic/static relocation as required, allocator and stable syscall ABI, process launch/wait, POSIX VFS semantics and a usable freestanding libc. Then compile Git upstream against a native LunaOS sysroot. Native Git cannot run through this small ELF proof-of-concept.
