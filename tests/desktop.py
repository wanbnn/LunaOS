#!/usr/bin/env python3
"""Keyboard UX, cached composition, separate quick panel, settings and autocomplete."""

import hashlib
import json
import os
import re
import shutil
import struct
import time

from guest import ROOT, TEST, Guest, memory, symbol_address, wait_until

WIN = struct.Struct("<9iIBBBx")
base = symbol_address("windows")
active = symbol_address("active_app")
pbase = symbol_address("prefs")


def all_windows(g):
    return [WIN.unpack_from(memory(g, base, WIN.size * 6), i * WIN.size) for i in range(6)]


def focus(g):
    return struct.unpack("<i", memory(g, active, 4))[0]


def pref(g, index):
    return struct.unpack("<i", memory(g, pbase + index * 4, 4))[0]


def scene_pixels(g, x, y, w, h):
    """Raw pixels of the compositor scene buffer, the frame the guest draws."""
    base = symbol_address("scene")
    return memory(g, base + (y * 1024 + x) * 4, w * h * 4)


def render_ms(g):
    return struct.unpack("<I", memory(g, symbol_address("compositor_render_ms"), 4))[0]


def boolean(g, name):
    return memory(g, symbol_address(name), 1) != b"\0"


def move(g, x, y):
    if memory(g, symbol_address("absolute_pointer"), 1) == b"\x01":
        g.qcommand("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": (x * 32767 + 1022) // 1023}},
            {"type": "abs", "data": {"axis": "y", "value": (y * 32767 + 766) // 767}}]})
        wait_until(lambda: abs(int.from_bytes(memory(g, symbol_address("mouse_x"), 4), "little") - x) <= 1 and abs(int.from_bytes(memory(g, symbol_address("mouse_y"), 4), "little") - y) <= 1)
        return
    xa = symbol_address("mouse_x")
    ya = symbol_address("mouse_y")

    def position():
        return struct.unpack("<i", memory(g, xa, 4))[0], struct.unpack("<i", memory(g, ya, 4))[0]

    for _ in range(40):
        ox, oy = position()
        if (ox, oy) == (x, y):
            return
        dx = max(-80, min(80, x - ox))
        dy = max(-80, min(80, y - oy))
        g.qcommand(
            "input-send-event",
            {
                "events": [
                    {"type": "rel", "data": {"axis": "x", "value": dx}},
                    {"type": "rel", "data": {"axis": "y", "value": dy}},
                ]
            },
        )
        wait_until(lambda: position() == (ox + dx, oy + dy))
    raise AssertionError("pointer did not reach target")


def click(g, x, y):
    move(g, x, y)
    for down in [True, False]:
        g.qcommand(
            "input-send-event",
            {"events": [{"type": "btn", "data": {"button": "left", "down": down}}]},
        )
        time.sleep(0.04)
    time.sleep(0.06)


def combo(g, *keys):
    g.qcommand("send-key", {"keys": [{"type": "qcode", "data": k} for k in keys]})
    time.sleep(0.2)


def line(g):
    return memory(g, symbol_address("shell_line"), 256).split(b"\0")[0].decode()


def type_keys(g, text):
    for c in text:
        g.key("spc" if c == " " else "slash" if c == "/" else "dot" if c == "." else c)


shutil.copyfile(ROOT / "dist/LunaOS.img", TEST / "desktop.img")
g = Guest(TEST / "desktop.img")
try:
    assert focus(g) == 0
    # Hovering the dock may only repaint the tooltip. The glass of the dock
    # must not be rasterized again: a second pass over the frame buffer would
    # pile another shadow on the one already there.
    dock_x, dock_y = (1024 - 538) // 2, 768 - 94
    shadow = scene_pixels(g, dock_x, dock_y + 76, 538, 12)
    for icon in range(7):
        move(g, dock_x + 16 + icon * 74 + 25, dock_y + 30)
    assert scene_pixels(g, dock_x, dock_y + 76, 538, 12) == shadow, "dock shadow was repainted"
    assert render_ms(g) <= 2, f"hover costs {render_ms(g)} ms"
    move(g, 512, 384)
    for key in ["f1", "f2", "f3", "f4", "f5", "f6"]:
        g.key(key)
    assert focus(g) == 0 and sum(w[10] for w in all_windows(g)) == 1
    g.key("meta_l")
    wait_until(lambda: boolean(g, "launcher_open"))
    g.screenshot("app-search")
    g.key("esc")
    wait_until(lambda: not boolean(g, "launcher_open"))
    g.open_app("terminal")
    wait_until(lambda: focus(g) == 1)
    g.wait("luna> ")
    g.shell_ready = True
    type_keys(g, "unam")
    g.key("tab")
    wait_until(lambda: line(g) == "uname ")
    g.buf = ""
    g.key("ret")
    g.wait("luna> ")
    assert "LunaOS 0.3.0" in g.buf
    type_keys(g, "cat wel")
    g.key("tab")
    wait_until(lambda: line(g) == "cat welcome.txt ")
    combo(g, "ctrl", "c")
    wait_until(lambda: line(g) == "")
    type_keys(g, "echo ac")
    g.key("left")
    g.key("b")
    assert line(g) == "echo abc"
    combo(g, "ctrl", "c")
    wait_until(lambda: line(g) == "")
    g.open_app("files")
    g.open_app("browser")
    assert focus(g) == 3
    combo(g, "alt", "tab")
    wait_until(lambda: focus(g) == 2)
    combo(g, "alt", "f4")
    wait_until(lambda: all_windows(g)[2][10] == 0)
    # This path must open a popup, not the Settings app.
    click(g, 850, 26)
    wait_until(lambda: boolean(g, "controls_open"))
    assert not all_windows(g)[4][10]
    g.screenshot("quick-controls")
    g.key("esc")
    wait_until(lambda: not boolean(g, "controls_open"))
    g.open_app("settings")
    wait_until(lambda: focus(g) == 4)
    w = all_windows(g)[4]
    x, y = w[1] + 20, w[2] + 46
    # Appearance changes the actual compositor state and wallpaper.
    click(g, x + 70, y + 62 + 4 * 47 + 18)
    click(g, x + 235, y + 108)
    wait_until(lambda: pref(g, 11) == 1)
    click(g, x + 198 + 24 + 2 * 51, y + 250)
    wait_until(lambda: pref(g, 12) == 2)
    click(g, x + 198 + 180, y + 338)
    wait_until(lambda: pref(g, 10) == 1)
    g.screenshot("settings-appearance")
    # Sound volume slider writes a real preference consumed by the AC97 mixer.
    click(g, x + 70, y + 62 + 3 * 47 + 18)
    click(g, x + 198 + 22 + 180, y + 197)
    wait_until(lambda: pref(g, 7) < 50)
    g.screenshot("settings-sound")
    click(g, x + 70, y + 62 + 2 * 47 + 18)
    g.screenshot("settings-displays")
    click(g, x + 70, y + 62 + 1 * 47 + 18)
    g.screenshot("settings-mouse")
    # Save all current controls, then re-open the same writable image to test restoration.
    click(g, x + 85, y + (w[4] - 66) - 30)
    wait_until(lambda: pref(g, 11) == 1)
    g.open_app("home")
    g.open_app("files")
    g.open_app("tasks")
    assert all(w[10] for w in all_windows(g)), "all six apps must be open for the benchmark"
    result = g.command("bench")
    fps = int(re.search(r"benchmark: (\d+) fps", result).group(1))
    if os.access("/dev/kvm", os.R_OK | os.W_OK):
        assert fps >= 60, result
    g.screenshot("desktop-v03")
    g.close()
    g = None
    g = Guest(TEST / "desktop.img")
    assert pref(g, 11) == 1 and pref(g, 12) == 2 and pref(g, 10) == 1
    report = {
        "result": "PASS",
        "image_sha256": hashlib.sha256((ROOT / "dist/LunaOS.img").read_bytes()).hexdigest(),
        "composition_benchmark_fps": fps,
        "target_fps": 60,
        "hardware_refresh_change_supported": False,
        "verified": [
            "F1-F6 global launch shortcuts removed",
            "dock hover repaints only the tooltip",
            "Super app search",
            "Alt+Tab focus switching",
            "Alt+F4 closes focused application",
            "command Tab completion",
            "path Tab completion",
            "terminal caret editing",
            "Ctrl+C",
            "quick panel distinct from Settings application",
            "theme and wallpaper switching",
            "audio volume control",
            "settings persisted across boot",
            "cached composition throughput benchmark",
        ],
        "limits": [
            "No modern DOM/CSS engine integrated",
            "Monitor refresh remains firmware/host controlled",
        ],
    }
    (ROOT / "dist/desktop-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print("PASS: desktop UX and settings; measured cached composition:", fps, "fps")
finally:
    if g:
        g.close()
