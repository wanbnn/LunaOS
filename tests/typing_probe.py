#!/usr/bin/env python3
"""Checks that typed text reaches the screen and where the typing CPU goes."""

import os
import shutil
import struct
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from guest import Guest, memory, symbol_address  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
HZ = os.sysconf("SC_CLK_TCK")


def cpu_ticks(pid):
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(") ", 1)[1].split()
    return int(fields[11]) + int(fields[12])


def cpu_pct(pid, seconds, action=None):
    t0 = cpu_ticks(pid)
    start = time.monotonic()
    if action:
        action(seconds)
    else:
        time.sleep(seconds)
    return (cpu_ticks(pid) - t0) / HZ / (time.monotonic() - start) * 100.0


def u32(g, name):
    return struct.unpack("<I", memory(g, symbol_address(name), 4))[0]


def main():
    disk = ROOT / "build/test/typing.img"
    shutil.copy(ROOT / "dist/LunaOS.img", disk)
    g = Guest(disk)

    def reader():
        while True:
            try:
                g.read(0.2)
            except Exception:
                return

    threading.Thread(target=reader, daemon=True).start()
    try:
        g.open_app("terminal")
        time.sleep(1.0)
        pid = g.p.pid

        def typing(seconds, keys=("a", "b", "c")):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                for k in keys:
                    g.qcommand("send-key", {"keys": [{"type": "qcode", "data": k}]})
                    time.sleep(0.03)

        print(f"idle:            {cpu_pct(pid, 4.0):5.1f}%")
        print(f"typing (33/s):   {cpu_pct(pid, 4.0, typing):5.1f}%")
        print(f"render_ms={u32(g, 'compositor_render_ms')} missed={u32(g, 'compositor_missed')}")

        # does the typed text reach the screen?
        g.qcommand("screendump", {"filename": str(ROOT / "build/test/typing.ppm")})
        from PIL import Image
        im = Image.open(ROOT / "build/test/typing.ppm").convert("RGB")
        win = struct.unpack("<9iIBBBx", memory(g, symbol_address("windows"), 44))
        x, y = win[1], win[2]
        crop = im.crop((x + 20, y + 46, x + 300, y + 120))
        bright = sum(1 for p in crop.getdata() if min(p) > 200)
        print(f"window at ({x},{y}); bright pixels in the content area: {bright}")
        im.save(ROOT / "build/test/typing.png")

        # typing into an app that does not echo to the serial
        g.open_app("notes")
        time.sleep(0.5)
        print(f"typing in Notes: {cpu_pct(pid, 4.0, typing):5.1f}%")
        print(f"render_ms={u32(g, 'compositor_render_ms')} missed={u32(g, 'compositor_missed')}")
    finally:
        g.close()


if __name__ == "__main__":
    main()
