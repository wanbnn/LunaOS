#!/usr/bin/env python3
"""QEMU-only smoke for the restricted ELF32 loader and guest file syscalls.

Uses the actual LunaOS disk, RTL8139 HTTP, ring-3 loader, stdout, open/read/close.
This is NOT a Git/posix compatibility test.
"""
import http.server
import shutil
import struct
import threading

from guest import Guest, ROOT, TEST


def sample_elf() -> bytes:
    code = bytearray()
    patches = {}

    def mov(reg: int, value: int):
        code.append(0xB8 + reg)
        code.extend(struct.pack("<I", value))

    def place_ref(reg: int, symbol: str):
        code.append(0xB8 + reg)
        patches[symbol] = len(code)
        code.extend(b"\x00" * 4)

    def call(number: int):
        mov(0, number)
        code.extend(b"\xcd\x80")

    def label(symbol: str, data: bytes):
        off = len(code)
        for name, where in list(patches.items()):
            if name == symbol:
                struct.pack_into("<I", code, where, off)
        code.extend(data)
        return off

    # Write marker from RX code mapping: SYS_EXEC_WRITE=6, fd=1.
    mov(3, 1)   # ebx stdout
    place_ref(1, "marker")  # ecx pointer, resolved after instructions
    mov(2, len(b"LUNA ELF32 RING3 OK\n"))
    call(6)

    # Open /home/luna/welcome.txt with read-only O_RDONLY=0.
    place_ref(3, "path")
    mov(1, 0)
    call(7)
    code.extend(b"\x89\xc3")       # fd -> ebx
    mov(1, 0x810000)                # ecx -> writable user stack
    mov(2, 24)
    call(8)                        # read(fd, stack, 24), result in eax
    code.extend(b"\x89\xc2")       # bytes -> edx
    mov(3, 1)
    mov(1, 0x810000)
    call(6)
    mov(0, 3)
    code.extend(b"\x31\xdb\xcd\x80") # exit(0)
    label("marker", b"LUNA ELF32 RING3 OK\n")
    label("path", b"/home/luna/welcome.txt\x00")

    ehdr = struct.pack(
        "<16sHHIIIIIHHHHHH",
        b"\x7fELF" + bytes([1, 1, 1]) + b"\x00" * 9,
        2, 3, 1, 0, 52, 0, 0, 52, 32, 1, 0, 0, 0
    )
    phdr = struct.pack("<IIIIIIII", 1, 84, 0, 0, len(code), len(code), 5, 1)
    image = ehdr + phdr + code
    assert len(image) < 4096
    return image




def sample_large_multisegment_elf() -> bytes:
    """Two PT_LOADs in different PDEs, sparse >5 MiB VA and user argv/envp."""
    code = bytearray()
    data = bytearray()
    labels = {}
    fail_jumps = []
    def mov(reg: int, value: int):
        code.append(0xB8 + reg)
        code.extend(struct.pack("<I", value))
    def sys(no: int):
        mov(0, no)
        code.extend(b"\xcd\x80")
    def dword(text: bytes):
        offset = 0x500000 + len(data)
        data.extend(text)
        return offset
    def jne_fail():
        code.extend(b"\x0f\x85")
        fail_jumps.append(len(code))
        code.extend(bytes(4))
    success = dword(b"ARGV ENV MULTISEG OK\n")
    welcome = dword(b"/home/luna/welcome.txt\x00")
    old = dword(b"/tmp/luna-elf-created.tmp\x00")
    new = dword(b"/tmp/luna-elf-created.txt\x00")
    payload = dword(b"WRITTEN BY NATIVE RING3\n")

    # SysV-style i386 initial stack: argc, argv[], 0, envp[], 0, AT_NULL.
    code.extend(b"\x83\x3c\x24\x03")      # cmp dword [esp],3
    jne_fail()
    code.extend(b"\x8b\x4c\x24\x08")      # mov ecx,[esp+8], argv[1]
    code.extend(b"\x80\x39\x41")          # cmp byte [ecx],'A'
    jne_fail()
    code.extend(b"\x8b\x4c\x24\x14")      # mov ecx,[esp+20], envp[0]
    code.extend(b"\x80\x39\x48")          # cmp byte [ecx],'H'
    jne_fail()
    mov(3, 1)
    mov(1, success)
    mov(2, len(b"ARGV ENV MULTISEG OK\n"))
    sys(6)

    # Read a real LunaFS2 file into the private 16 KiB stack.
    mov(3, welcome)
    mov(1, 0)
    sys(7)
    code.extend(b"\x89\xc3")             # mov ebx,eax (fd)
    mov(1, 0x810000)
    mov(2, 24)
    sys(8)
    code.extend(b"\x89\xc2")             # mov edx,eax (length)
    mov(3, 1)
    mov(1, 0x810000)
    sys(6)

    # O_WRONLY|O_CREAT|O_TRUNC = 1|64|512 = 577.
    mov(3, old)
    mov(1, 577)
    sys(7)
    code.extend(b"\x89\xc3")
    mov(1, payload)
    mov(2, len(b"WRITTEN BY NATIVE RING3\n"))
    sys(6)                                # write(fd, read-only data segment, len)
    mov(0, 9)                             # close(fd)
    code.extend(b"\xcd\x80")
    mov(3, old)
    mov(1, new)
    sys(16)                               # rename(old,new)
    code.extend(b"\x85\xc0")             # test eax,eax
    jne_fail()

    mov(3, 0)                             # successful exit
    mov(0, 3)
    code.extend(b"\xcd\x80")
    fail_offset = len(code)
    mov(3, 7)
    mov(0, 3)
    code.extend(b"\xcd\x80")
    for patch in fail_jumps:
        struct.pack_into("<i", code, patch, fail_offset - (patch + 4))
    if len(code) >= 4096:
        raise AssertionError("smoke code unexpectedly oversized")
    code.extend(b"\x90" * (5000 - len(code)))  # exceed one 4 KiB page
    ehdr = struct.pack(
        "<16sHHIIIIIHHHHHH",
        b"\x7fELF" + bytes([1, 1, 1]) + b"\x00" * 9,
        2, 3, 1, 0, 52, 0, 0, 52, 32, 2, 0, 0, 0
    )
    ph0 = struct.pack("<IIIIIIII", 1, 4096, 0, 0, len(code), 8192, 5, 4096)
    ph1 = struct.pack("<IIIIIIII", 1, 12288, 0x500000, 0x500000,
                      len(data), 4096, 6, 4096)
    image = bytearray(12288 + len(data))
    image[:len(ehdr)] = ehdr
    image[52:84] = ph0
    image[84:116] = ph1
    image[4096:4096+len(code)] = code
    image[12288:] = data
    return bytes(image)


MULTI_DATA = sample_large_multisegment_elf()
C_DATA = (ROOT / "build/guest/hello.elf").read_bytes()
Z_DATA = (ROOT / "build/guest/zlib-smoke.elf").read_bytes()
SPAWN_DATA = (ROOT / "build/guest/spawn-smoke.elf").read_bytes()
EXECVE_DATA = (ROOT / "build/guest/execve-smoke.elf").read_bytes()
assert len(Z_DATA) <= 16 * 1024 * 1024, "zlib guest ELF exceeds loader budget"
assert len(C_DATA) <= 256 * 1024, "guest ELF exceeds loader file limit"

DATA = sample_elf()
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        data = (EXECVE_DATA if self.path.endswith("/execve.elf") else
                SPAWN_DATA if self.path.endswith("/spawn.elf") else
                Z_DATA if self.path.endswith("/zlib.elf") else
                C_DATA if self.path.endswith("/c-hello.elf") else
                MULTI_DATA if self.path.endswith("/multi.elf") else DATA))
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
TEST.mkdir(parents=True, exist_ok=True)
shutil.copyfile(ROOT / "dist/LunaOS.img", TEST / "elf32-disk.img")
g = None
try:
    g = Guest(TEST / "elf32-disk.img", network=True, ram=128)
    g.command("elfrun /missing/file.elf", "elfrun: unsupported ELF32")
    port = server.server_address[1]
    g.command(f"wget http://10.0.2.2:{port}/hello.elf /tmp/hello.elf", "Saved", timeout=30)
    g.command("elfrun /tmp/hello.elf", "started ring-3 process")
    g.wait("LUNA ELF32 RING3 OK", 15)
    g.wait("Welcome to LunaOS", 15)
    print("PASS: guest ELF32 executed ring 3; stdout and read-only open/read syscalls", flush=True)

    g.command(f"wget http://10.0.2.2:{port}/multi.elf /tmp/multi.elf", "Saved", timeout=30)
    launched = g.command("elfrun /tmp/multi.elf Apple Banana", "started ring-3 process")
    import re
    match = re.search(r"started ring-3 process pid=(\d+)", launched)
    assert match, launched
    g.command("elfwait " + match.group(1), "exit code 0", timeout=25)
    assert "ARGV ENV MULTISEG OK" in g.transcript, g.transcript[-3000:]
    g.command("cat /tmp/luna-elf-created.txt", "WRITTEN BY NATIVE RING3")
    print("PASS: sparse >5 MiB guest VA, multi-PDE ELF32, argv/envp, open/read/write/rename, child wait", flush=True)
    g.command(f"wget http://10.0.2.2:{port}/c-hello.elf /tmp/c-hello.elf", "Saved", timeout=30)
    launched = g.command("elfrun /tmp/c-hello.elf Apple Banana", "started ring-3 process")
    match = re.search(r"started ring-3 process pid=(\d+)", launched)
    assert match, launched
    g.command("elfwait " + match.group(1), "exit code 0", timeout=25)
    assert "LUNA GUEST C POSIX OK" in g.transcript, g.transcript[-3000:]
    g.command("cat /tmp/guest-c-after.txt", "C SDK FILE WRITE OK")
    print("PASS: actual freestanding C ELF32, i386 crt0, argv/envp and file syscalls in QEMU", flush=True)
    g.command(f"wget http://10.0.2.2:{port}/zlib.elf /tmp/zlib.elf", "Saved", timeout=30)
    launched = g.command("elfrun /tmp/zlib.elf", "started ring-3 process")
    match = re.search(r"started ring-3 process pid=(\d+)", launched)
    assert match, launched
    g.command("elfwait " + match.group(1), "exit code 0", timeout=35)
    assert "LUNA ZLIB V1.4 GUEST OK" in g.transcript, g.transcript[-4000:]
    print("PASS: upstream zlib 1.4 native i386 compress2 and uncompress in LunaOS QEMU", flush=True)
    g.command(f"wget http://10.0.2.2:{port}/spawn.elf /tmp/spawn.elf", "Saved", timeout=30)
    launched = g.command("elfrun /tmp/spawn.elf", "started ring-3 process")
    match = re.search(r"started ring-3 process pid=(\d+)", launched)
    assert match, launched
    g.command("elfwait " + match.group(1), "exit code 0", timeout=35)
    assert "LUNA CHILD EXECUTE OK" in g.transcript, g.transcript[-4000:]
    assert "LUNA SPAWN WAIT OK" in g.transcript, g.transcript[-4000:]
    print("PASS: native child spawn/argv/env and waitpid exit-code forwarding in LunaOS", flush=True)

    g.command(f"wget http://10.0.2.2:{port}/execve.elf /tmp/execve.elf", "Saved", timeout=30)
    launched = g.command("elfrun /tmp/execve.elf", "started ring-3 process")
    match = re.search(r"started ring-3 process pid=(\d+)", launched)
    assert match, launched
    g.command("elfwait " + match.group(1), "exit code 0", timeout=35)
    assert "EXECVE SAME PID FD OK" in g.transcript, g.transcript[-4000:]
    g.command("cat /tmp/execve-fd.txt", "EXECVE SAME PID FD OK")
    print("PASS: failed execve preserves caller, successful execve replaces image without PID/FD loss", flush=True)

    g.close()
    g = None
    g = Guest(TEST / "elf32-disk.img", network=False, ram=128)
    g.command("cat /tmp/guest-c-after.txt", "C SDK FILE WRITE OK")
    print("PASS: fsync guest file/rename survived LunaOS reboot", flush=True)




finally:
    if g:
        g.close()
    server.shutdown()
