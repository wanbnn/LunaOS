#!/usr/bin/env python3
import hashlib
import json
import subprocess
from pathlib import Path
from firmwarepack import SECTOR

p = Path("dist/LunaOS.img")
data = p.read_bytes()
m = json.loads(Path("dist/manifest.json").read_text())
assert data[510:512] == b"\x55\xaa"
assert len(data) == m["bytes"] and len(data) <= 120_000_000
assert hashlib.sha256(data).hexdigest() == m["sha256"]
assert (
    data[512 : 512 + len(Path("build/stage2.bin").read_bytes())]
    == Path("build/stage2.bin").read_bytes()
)
assert data[512 + 32768 : 512 + 32768 + m["kernel_bytes"]] == Path("build/kernel.bin").read_bytes()
assert not subprocess.check_output(["nm", "-u", "build/kernel.elf"]).strip(), (
    "unresolved external dependencies"
)
assert b"/lib/ld-linux" not in Path("build/kernel.bin").read_bytes()
print("PASS: boot signature, image layout, size budget, SHA-256 and freestanding link")

# Firmware is an independent binary region, beyond both LunaFS snapshots.
firmware_offset = m["firmware_sector"] * 512
assert data[firmware_offset:firmware_offset + 8] == b"LUNAFW1\0"
assert firmware_offset == SECTOR * 512
assert firmware_offset + m["firmware_archive_bytes"] <= len(data)
print("PASS: binary firmware region layout")

# A complete source snapshot is shipped as data, not as native radio drivers.
import struct
source_offset = 32768 * 512
magic, length, digest, commit = struct.unpack_from("<8sI32s40s", data, source_offset)
assert magic == b"LUNASRC1"
source = data[source_offset + 512:source_offset + 512 + length]
assert hashlib.sha256(source).digest() == digest
assert digest.hex() == m["wireless_sources"]["sha256"]
assert commit.decode() == m["wireless_sources"]["commit"]
assert data[16383 * 512:16383 * 512 + 8] == b"LUNAIMG1"
assert struct.unpack_from("<I", data, 16383 * 512 + 12)[0] == len(data)
print("PASS: installer metadata / complete wireless source archive hash")
