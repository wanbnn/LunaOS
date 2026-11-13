# Git upstream port — development entry point

This directory is a **host-side build and compatibility fixture**, not a Git executable installed in LunaOS.

## Pinned source

- Upstream: https://github.com/git/git
- Release: `v2.56.1` (security fixes from 2026-11-10)
- Exact commit: `03b41523c576f26ad34085378717a0c1b287470f`
- Source location: `third_party/git` (Git submodule; fetch with `--recurse-submodules`)
- Git's own GPL-2.0 license, notices and history stay in its upstream source checkout. Do not link this code into the proprietary kernel without complying with upstream licensing requirements.

## Start from a host workstation

```sh
git submodule update --init --recursive third_party/git
git -C third_party/git fetch --depth=1 origin tag v2.56.1  # only needed if tags are missing
git -C third_party/git rev-parse HEAD
make git-upstream-host-build
make git-upstream-host-test
```

These targets compile the **actual upstream Git** on the *developer host* and exercise offline workflows using a temporary local bare remote; they do not run Git in the guest. The host requires GCC, make, Python 3, zlib, libcurl, OpenSSL and expat development headers.

## Architecture work required next

1. Implement a user-mode executable loader and process launch for `git`, with argv/env/stdio/exit, pipes and signals. **Do not run upstream Git as a kernel built-in**.
2. Add POSIX-compatible VFS/libc semantics, including directory traversal, file metadata, atomic rename, locking, memory allocation, fsync, chmod, symlink and stat.
3. Port zlib and upstream Git to a LunaOS sysroot. Build a native executable and demonstrate its startup inside the guest (QEMU).
4. Integrate smart Git HTTP/HTTPS with verified certificates and secure auth and SSH with strict host-key checking.
5. Execute the same host fixture on the guest using real Git, then expand the suite to remote HTTPS/SSH and the full command matrix in `tests/git-full-acceptance.md`.
6. Only then integrate launcher dispatch in `lunaSH` to a real executable, and consider a production merge.

The existing LunaFS2 is not a drop-in POSIX file system and the current lunaSH command dispatcher does not execute ordinary ELF programs. Merely shipping upstream source or building it on Linux **does not satisfy the user's full Git requirement**.

## CI scope

`.github/workflows/git-upstream.yml` builds the pinned Git 2.56.1 on Ubuntu and runs `tests/git_upstream_host.py`. It never needs GitHub credentials and does not access network remotes. A green job verifies the baseline and dependency integrity, **not** LunaOS compatibility.

## Milestone 4: guest VM, process and compression groundwork

The branch now contains an 8 MiB sparse ELF32 application virtual
address span with segment-by-segment physical allocation and a guarded
16 KiB stack above the image. C guest interfaces expose a bounded pipe,
snapshot-backed fsync and a real child-creation/wait mechanism
(`luna_spawn`/`luna_waitpid`). The upstream zlib v1.4 submodule
can be built for freestanding i386 with `make luna-zlib-guest`, and
`make luna-guest-zlib-smoke` builds a guest compression test.

See `docs/elf32-execution.md` for the exact syscall semantics
and limitations. The CI workflow
`.github/workflows/elf32-exec.yml` verifies native execution.
These pieces are **NOT POSIX-compliant enough for official Git**:
`fork/execve`, blocking inherited pipes, mmap, symlinks,
full stat/mtime/mode, locking/fsync semantics and per-process
network sockets/Smart HTTP/SSH still need implementation.
