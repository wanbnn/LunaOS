#!/usr/bin/env python3
"""A real native HTTP download larger than the guest's physical RAM."""

import hashlib
import http.server
import json
import os
import shutil
import socket
import threading
import time

from guest import ROOT, TEST, Guest, memory, symbol_address
from storage_interfaces import device_args

SIZE = int(os.environ.get("LUNA_DOWNLOAD_BYTES", 96 * 1024 * 1024 + 1))
RAM = int(os.environ.get("LUNA_DOWNLOAD_RAM", 64))
BLOCK = b"x" * 65536
BACKEND = os.environ.get("LUNA_NET_BACKEND", "user")


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.send_response(200)
        self.send_header("Content-Length", str(SIZE))
        self.end_headers()
        self.server.transfer_started = time.perf_counter()
        remaining = SIZE
        while remaining:
            piece = BLOCK[: min(remaining, len(BLOCK))]
            self.wfile.write(piece)
            remaining -= len(piece)
        self.server.transfer_seconds = time.perf_counter() - self.server.transfer_started

    def log_message(self, *args):
        pass


def backing_file(guest):
    capacity = int.from_bytes(memory(guest, symbol_address("fs_file_capacity"), 4), "little")
    pointer = int.from_bytes(memory(guest, symbol_address("files"), 4), "little")
    records = memory(guest, pointer, capacity * 156)
    for offset in range(0, len(records), 156):
        record = records[offset : offset + 156]
        if record[:128].split(b"\0")[0] == b"/home/luna/stream.bin":
            assert int.from_bytes(record[132:136], "little") == SIZE
            assert int.from_bytes(record[140:144], "little") == 0, "file unexpectedly cached in RAM"
            return int.from_bytes(record[148:156], "little")
    raise AssertionError("download not found")


server = http.server.ThreadingHTTPServer(
    ("0.0.0.0" if BACKEND == "tap" else "127.0.0.1", 0), Handler
)
threading.Thread(target=server.serve_forever, daemon=True).start()
image = TEST / "stream.img"
shutil.copyfile(ROOT / "dist/LunaOS.img", image)
with image.open("r+b") as disk:
    disk.truncate(max(320 * 1024 * 1024, 66 * 1024 * 1024 + SIZE * 2 + 16 * 1024 * 1024))
    disk.seek(440)
    identity = (
        int.from_bytes(
            hashlib.sha256((ROOT / "dist/LunaOS.img").read_bytes()).digest()[:4], "little"
        )
        or 1
    )
    disk.write(identity.to_bytes(4, "little"))


def boot_guest(network=True):
    return Guest(
        ROOT / "dist/LunaOS.iso",
        network=network and BACKEND == "user",
        ram=RAM,
        cdrom=True,
        extra_args=device_args("ahci", image)
        + (
            [
                "-netdev",
                "tap,id=benchmark,ifname=luna-test,script=no,downscript=no",
                "-device",
                "rtl8139,netdev=benchmark,romfile=",
            ]
            if network and BACKEND == "tap"
            else []
        ),
        boot_timeout=600,
    )


guest = boot_guest()
try:
    assert "native boot volume matched MBR ID on disk 5" in guest.transcript
    command_started = time.perf_counter()
    guest.command(
        f"wget http://10.0.2.2:{server.server_port}/large /home/luna/stream.bin",
        f"Saved {SIZE} bytes",
        timeout=900,
    )
    command_seconds = time.perf_counter() - command_started
    status = guest.command("net")
    payload = backing_file(guest)
finally:
    guest.close()
    server.shutdown()

actual = hashlib.sha256()
expected = hashlib.sha256()
remaining = SIZE
with image.open("rb") as disk:
    disk.seek(payload)
    while remaining:
        count = min(remaining, len(BLOCK))
        data = disk.read(count)
        assert len(data) == count
        actual.update(data)
        expected.update(BLOCK[:count])
        remaining -= count
assert actual.digest() == expected.digest()
print("PASS: complete disk payload SHA-256 independently verified", flush=True)
guest = boot_guest(network=False)
try:
    backing_file(guest)
    guest.command(
        f"write /home/luna/check.ljs \"console.log(luna.fs.read('stream.bin',{SIZE - 4},4))\""
    )
    guest.command("dev run /home/luna/check.ljs", "xxxx")
finally:
    guest.close()
report = {
    "result": "PASS",
    "bytes": SIZE,
    "guest_ram_mib": RAM,
    "sha256": actual.hexdigest(),
    "network_backend": BACKEND,
    "command_seconds": command_seconds,
    "command_mbps": SIZE * 8 / command_seconds / 1_000_000,
    "server_send_seconds": server.transfer_seconds,
    "server_send_mbps": SIZE * 8 / server.transfer_seconds / 1_000_000,
    "network_status": status,
    "verified": [
        "native HTTP streaming to AHCI disk",
        "file larger than guest RAM" if SIZE > RAM * 1024 * 1024 else "requested-size transfer",
        "no full-file RAM cache",
        "full SHA-256 match",
        "guest restart",
        "SDK ranged disk read",
    ],
}
(TEST / "report.json").write_text(json.dumps(report, indent=2) + "\n")
print(f"PASS: {SIZE}-byte download with {RAM} MiB RAM and persistent ranged reads", flush=True)

print(
    f"THROUGHPUT: end-to-end {report['command_mbps']:.2f} Mbps, server send {report['server_send_mbps']:.2f} Mbps",
    flush=True,
)
