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
    mov(1, 0x10000)                # ecx -> writable user stack
    mov(2, 24)
    call(8)                        # read(fd, stack, 24), result in eax
    code.extend(b"\x89\xc2")       # bytes -> edx
    mov(3, 1)
    mov(1, 0x10000)
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


DATA = sample_elf()
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(DATA)))
        self.end_headers()
        self.wfile.write(DATA)

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
finally:
    if g:
        g.close()
    server.shutdown()
