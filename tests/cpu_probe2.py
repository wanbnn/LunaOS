#!/usr/bin/env python3
"""Decomposes the compositor cost: cached scene vs. invalidated surface, per window count."""

import os
import shutil
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from guest import Guest, memory, symbol_address, wait_until  # noqa: E402

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
    used = cpu_ticks(pid) - t0
    return used / HZ / (time.monotonic() - start) * 100.0


def u32(g, name):
    return struct.unpack("<I", memory(g, symbol_address(name), 4))[0]


def state(g, tag):
    print(f"  [{tag}] render_ms={u32(g, 'compositor_render_ms')} "
          f"fps={u32(g, 'compositor_fps')} missed={u32(g, 'compositor_missed')}")


def main():
    disk = ROOT / "build/test/cpu_probe2.img"
    shutil.copy(ROOT / "dist/LunaOS.img", disk)
    g = Guest(disk)
    pid = g.p.pid
    try:
        time.sleep(1.5)
        print("\n=== A. 1 window open (Home), fully idle ===")
        state(g, "1 window")
        print(f"  host CPU idle: {cpu_pct(pid, 4.0):.1f}%")

        print("\n=== B. open all 6 apps, let surfaces cache, then measure ===")
        for app in ("terminal", "files", "browser", "settings", "tasks", "notes"):
            g.open_app(app)
        time.sleep(2.0)
        state(g, "6 windows")
        print(f"  host CPU idle (6 windows): {cpu_pct(pid, 4.0):.1f}%")

        print("\n=== C. pure mouse motion over the desktop (no click, no invalidation) ===")
        def move(seconds):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                for _ in range(8):
                    g.qcommand("input-send-event", {"events": [
                        {"type": "rel", "data": {"axis": "x", "value": 5}},
                        {"type": "rel", "data": {"axis": "y", "value": 2}}]})
                time.sleep(0.02)
        print(f"  host CPU during mouse motion: {cpu_pct(pid, 4.0, move):.1f}%")
        state(g, "after motion")

        print("\n=== D. typing (invalidates the focused app surface each keystroke) ===")
        def typing(seconds):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                g.qcommand("send-key", {"keys": [{"type": "qcode", "data": "a"}]})
                time.sleep(0.03)
        print(f"  host CPU while typing: {cpu_pct(pid, 4.0, typing):.1f}%")
        state(g, "after typing")

        print("\n=== E. cached composition benchmark (120 cached frames) ===")
        res = g.command("bench")
        for line in res.splitlines():
            if "benchmark" in line:
                print("  " + line.strip())
        state(g, "after bench")
    finally:
        g.close()


if __name__ == "__main__":
    main()
