# Native runtime components

The guest remains a custom kernel and bootloader, not Linux. These vendored libraries
supply language/cryptography/math functionality; no Linux userspace binaries run in it.

- QuickJS 2026-06-04, official archive from https://bellard.org/quickjs/quickjs-2026-06-04.tar.xz.
  The single upstream adaptation disables CONFIG_ATOMICS under LUNA_FREESTANDING;
  no pthread API is advertised. Used by the standalone `js` command, **not** a DOM/CSS engine.
- BearSSL 0.6, official archive https://bearssl.org/bearssl-0.6.tar.gz. TLS 1.2 AEAD/ECDHE,
  explicit hardware entropy, hostname/chain/time checks. Unix entropy/time backends disabled.
- musl 1.2.5, https://musl.libc.org/releases/musl-1.2.5.tar.gz. Only selected ISO-C/math
  objects are linked. Allocation, diagnostics, clock hooks and I/O belong to LunaOS.
  This does not constitute a POSIX compatibility layer or a Linux syscall ABI.
- GCC arithmetic support is supplied by the host 32-bit libgcc static archive; its
  license and runtime exception are in assets/licenses and in the guest license directory.

Each upstream source tree retains its original license. The image includes corresponding
notices in /etc/licenses. Root CA data in assets/tls is a pinned public CA snapshot;
a development CA or its private key is never included in the release image.

WebKit is **not** integrated. See docs/browser-engine.md for the actual missing work.

## uACPI

`uacpi/` contains the MIT-licensed AML interpreter and resource parser, pinned
at `c0edecef62bcac6ce3f783540099190bafadadda` (upstream 7.0.0). See
`uacpi/LUNA-PIN.txt` and `uacpi/LICENSE`. Upstream test fixtures are omitted.
The kernel uses the reduced-hardware build for boot-time ACPI storage discovery,
without SCI/GPE takeover. The license is also included in `/etc/licenses/uACPI.txt`.
