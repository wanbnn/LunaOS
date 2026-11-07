#!/usr/bin/env python3
"""Measures host CPU time consumed by the QEMU guest while idle, typing and clicking."""

import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from guest import Guest, memory, symbol_address, wait_until  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
HZ = os.sysconf("SC_CLK_TCK")


def cpu_ticks(pid):
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(") ", 1)[1].split()
    return int(fields[11]) + int(fields[12])  # utime + stime


def window_cpu(pid, seconds, action=None):
    t0 = cpu_ticks(pid)
    start = time.monotonic()
    if action:
        action(seconds)
    else:
        time.sleep(seconds)
    elapsed = time.monotonic() - start
    used = cpu_ticks(pid) - t0
    return used / HZ / elapsed * 100.0, elapsed


def read_u32(g, name):
    return struct.unpack("<I", memory(g, symbol_address(name), 4))[0]


def main():
    disk = ROOT / "build/test/cpu_probe.img"
    shutil.copy(ROOT / "dist/LunaOS.img", disk)
    g = Guest(disk)
    pid = g.p.pid
    try:
        g.open_app("terminal")
        g.wait("luna> ", 20)
        time.sleep(1.0)

        print("\n--- baseline: guest idle, no input ---")
        idle_pct, _ = window_cpu(pid, 5.0)
        print(f"host CPU while guest idle: {idle_pct:.1f}%")

        print("\n--- compositor state (idle) ---")
        print("render_ms  =", read_u32(g, "compositor_render_ms"))
        print("fps        =", read_u32(g, "compositor_fps"))
        print("missed     =", read_u32(g, "compositor_missed"))

        print("\n--- typing 20 chars into the focused window ---")

        def type_for(seconds):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                for ch in "abcdefghij":
                    g.qcommand("send-key", {"keys": [{"type": "qcode", "data": ch}]})
                    time.sleep(0.03)

        type_pct, _ = window_cpu(pid, 5.0, type_for)
        print(f"host CPU while typing: {type_pct:.1f}%")

        print("\n--- mouse motion only (no click) ---")

        def move_only(seconds):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                for _ in range(10):
                    g.qcommand("input-send-event", {"events": [
                        {"type": "rel", "data": {"axis": "x", "value": 9}},
                        {"type": "rel", "data": {"axis": "y", "value": 3}}]})
                time.sleep(0.02)
        print(f"host CPU while only moving: {window_cpu(pid, 4.0, move_only)[0]:.1f}%")

        print("\n--- mouse motion + clicking ---")

        def mouse_for(seconds):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                for _ in range(10):
                    g.qcommand("input-send-event", {"events": [
                        {"type": "rel", "data": {"axis": "x", "value": 9}},
                        {"type": "rel", "data": {"axis": "y", "value": 3}}]})
                g.qcommand("input-send-event", {"events": [
                    {"type": "btn", "data": {"button": "left", "down": True}}]})
                time.sleep(0.04)
                g.qcommand("input-send-event", {"events": [
                    {"type": "btn", "data": {"button": "left", "down": False}}]})
                time.sleep(0.04)

        mouse_pct, _ = window_cpu(pid, 5.0, mouse_for)
        print(f"host CPU while clicking/moving: {mouse_pct:.1f}%")

        print("\n--- compositor state (after interaction) ---")
        print("render_ms  =", read_u32(g, "compositor_render_ms"))
        print("fps        =", read_u32(g, "compositor_fps"))
        print("missed     =", read_u32(g, "compositor_missed"))

        print("\n--- cached composition benchmark (shell command) ---")
        print(g.command("bench"))
    finally:
        g.close()


if __name__ == "__main__":
    main()
