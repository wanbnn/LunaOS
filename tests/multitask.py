#!/usr/bin/env python3
"""Actual hardware CPL/protection, independent tasks and PS/2 window interactions."""

import hashlib
import json
import re
import shutil
import struct
import subprocess
import time

from guest import ROOT, TEST, Guest, memory, symbol_address, wait_until

# Compile ABI offsets from the actual header instead of guessing struct layouts.
layout_c = TEST / "task-layout.c"
layout_o = TEST / "task-layout.o"
layout_bin = TEST / "task-layout.bin"
layout_c.write_text(
    '#include "task.h"\nconst unsigned layout[]={sizeof(Task),offsetof(Task,state),offsetof(Task,pid),offsetof(Task,directory),offsetof(Task,observed_ring),offsetof(Task,app)};\n'
)
subprocess.run(
    ["gcc", "-m32", "-ffreestanding", "-Ikernel", "-c", str(layout_c), "-o", str(layout_o)],
    check=True,
)
subprocess.run(
    ["objcopy", "-O", "binary", "-j", ".rodata", str(layout_o), str(layout_bin)], check=True
)
stride, state_off, pid_off, dir_off, ring_off, app_off = struct.unpack(
    "<6I", layout_bin.read_bytes()
)
task_base = symbol_address("tasks")


def task_info(g):
    data = memory(g, task_base, stride * 20)
    result = {}
    for i in range(20):

        def field(offset):
            return struct.unpack_from("<I", data, i * stride + offset)[0]

        app = field(app_off)
        if field(state_off) not in [0, 5]:
            result[app] = {
                "pid": field(pid_off),
                "state": field(state_off),
                "directory": field(dir_off),
                "ring": field(ring_off),
            }
    return result


WIN = struct.Struct("<9iIBBBx")
window_base = symbol_address("windows")
active = symbol_address("active_app")


def windows(g):
    data = memory(g, window_base, WIN.size * 6)
    return [WIN.unpack_from(data, i * WIN.size) for i in range(6)]


def focused(g):
    return struct.unpack("<i", memory(g, active, 4))[0]


def wait_focus(g, app):
    wait_until(lambda: focused(g) == app)


def move(g, x, y):
    if memory(g, symbol_address("absolute_pointer"), 1) == b"\x01":
        g.qcommand("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": (x * 32767 + 1022) // 1023}},
            {"type": "abs", "data": {"axis": "y", "value": (y * 32767 + 766) // 767}}]})
        wait_until(lambda: abs(int.from_bytes(memory(g, symbol_address("mouse_x"), 4), "little") - x) <= 1 and abs(int.from_bytes(memory(g, symbol_address("mouse_y"), 4), "little") - y) <= 1)
        return
    xaddr = symbol_address("mouse_x")
    yaddr = symbol_address("mouse_y")

    def position():
        return (
            struct.unpack("<i", memory(g, xaddr, 4))[0],
            struct.unpack("<i", memory(g, yaddr, 4))[0],
        )

    while True:
        oldx, oldy = position()
        dx = x - oldx
        dy = y - oldy
        if not dx and not dy:
            return
        a = max(-80, min(80, dx))
        b = max(-80, min(80, dy))
        g.qcommand(
            "input-send-event",
            {
                "events": [
                    {"type": "rel", "data": {"axis": "x", "value": a}},
                    {"type": "rel", "data": {"axis": "y", "value": b}},
                ]
            },
        )
        wait_until(lambda: position() == (oldx + a, oldy + b), 20)


def press(g, down):
    g.qcommand(
        "input-send-event", {"events": [{"type": "btn", "data": {"button": "left", "down": down}}]}
    )
    time.sleep(0.35)


def click(g, x, y):
    move(g, x, y)
    press(g, True)
    press(g, False)


shutil.copyfile(ROOT / "dist/LunaOS.img", TEST / "multitask.img")
g = Guest(TEST / "multitask.img")
try:
    g.open_app("terminal")
    wait_focus(g, 1)
    g.wait("luna> ")
    result = g.command("ps", "Luna kernel / compositor")
    assert "lunaSH" in result
    g.command("kill 1", "protected")
    g.command("ringtest", "12 hardware protection probes")
    # These are real outb, segment-limit and page-fault instructions, not mock flags.
    deadline = time.monotonic() + 20
    while len(re.findall(r"RINGTEST PASS ring=[123][^\n]*\n", g.transcript)) < 12:
        assert time.monotonic() < deadline, g.transcript[-4000:]
        g.read()
    assert "RINGTEST FAIL" not in g.transcript and "FATAL" not in g.transcript
    for ring in [1, 2, 3]:
        assert g.transcript.count(f"RINGTEST PASS ring={ring}") == 4
    g.command("echo ALIVE_AFTER_PROBES", "ALIVE_AFTER_PROBES")
    print(
        "PASS: CPU executed rings 1/2/3 and rejected I/O, unmapped pages, kernel segments and privileged syscalls",
        flush=True,
    )
    for key, app in [("files", 2), ("browser", 3), ("settings", 4), ("tasks", 5)]:
        g.open_app(key)
        wait_focus(g, app)
    w = windows(g)
    pids = [row[9] for row in w]
    assert len(set(pids)) == 6 and all(pids)
    info = task_info(g)
    assert all(info[a]["ring"] == 3 for a in range(6))
    assert len({info[a]["directory"] for a in range(6)}) == 6
    private_stack_pages = set()
    for a in range(6):
        directory = memory(g, info[a]["directory"], 4096)
        pde = struct.unpack_from("<I", directory, 256 * 4)[0]
        table = memory(g, pde & ~4095, 4096)
        code = struct.unpack_from("<I", table, 0)[0]
        stack = struct.unpack_from("<I", table, 16 * 4)[0]
        assert code & 7 == 5 and stack & 7 == 7
        private_stack_pages.add(stack & ~4095)
    assert len(private_stack_pages) == 6
    print("PASS: observed CPL 3, distinct CR3 directories and private stack physical pages")
    g.screenshot("task-manager")
    # Maximize and restore to exactly the original rectangle.
    original = w[5]
    x, y = original[1:3]
    click(g, x + 76, y + 23)
    wait_until(lambda: windows(g)[5][12] == 1)
    assert windows(g)[5][3:5] == (1008, 606)
    w = windows(g)[5]
    click(g, w[1] + 76, w[2] + 23)
    wait_until(lambda: windows(g)[5][12] == 0)
    assert windows(g)[5][1:5] == original[1:5]
    # Drag a floating window; verify that the kernel stores the new real rectangle.
    w = windows(g)[5]
    move(g, w[1] + 350, w[2] + 23)
    press(g, True)
    move(g, w[1] + 290, w[2] + 4)
    press(g, False)
    wait_until(lambda: windows(g)[5][1] != w[1])
    assert windows(g)[5][1] == w[1] - 60
    g.screenshot("floating-windows")
    # Minimize retains task PID and state; dock restores that same task.
    w = windows(g)[5]
    pid = w[9]
    click(g, w[1] + 50, w[2] + 23)
    wait_until(lambda: windows(g)[5][11] == 1)
    assert windows(g)[5][9] == pid
    click(g, 259 + 5 * 74 + 25, 710)
    wait_focus(g, 5)
    assert windows(g)[5][9] == pid and windows(g)[5][11] == 0
    # Close ends the client; reopening gets a new PID instead of hiding a panel.
    w = windows(g)[5]
    click(g, w[1] + 24, w[2] + 23)
    wait_until(lambda: windows(g)[5][10] == 0)
    g.open_app("tasks")
    wait_focus(g, 5)
    assert windows(g)[5][9] != pid
    # End task in the GUI really terminates the selected client.
    for key in ["f", "i", "l", "e", "s"]:
        g.key(key)
    w = windows(g)[5]
    click(g, w[1] + 220, w[2] + 46 + 180)
    click(g, w[1] + w[3] - 95, w[2] + w[4] - 41)
    wait_until(lambda: windows(g)[2][10] == 0)
    print("PASS: Task Manager End task button")
    g.open_app("files")
    wait_focus(g, 2)
    # User task termination cannot affect the protected kernel task.
    g.command(f"kill {pids[3]}", "Task terminated")
    wait_until(lambda: windows(g)[3][10] == 0)
    g.open_app("browser")
    wait_focus(g, 3)
    assert windows(g)[3][9] != pids[3]
    # Sleeping shell suspends its stack, not the compositor or other clients.
    g.buf = ""
    for c in "sleep 10":
        mark = len(g.buf)
        g.serial.send(c.encode())
        wait_until(lambda: g.read(0.01) or len(g.buf) > mark)
    g.serial.send(b"\n")
    wait_until(lambda: task_info(g)[1]["state"] == 4)
    g.open_app("tasks")
    wait_focus(g, 5)
    assert task_info(g)[1]["state"] == 4, "shell should still be sleeping while manager responds"
    g.wait("luna> ", 20)
    for _ in range(5):
        g.key("backspace")
    g.screenshot("task-manager")
    report = {
        "result": "PASS",
        "image_sha256": hashlib.sha256((ROOT / "dist/LunaOS.img").read_bytes()).hexdigest(),
        "verified": [
            "actual CPL 1, 2 and 3 execution",
            "12 CPU privilege protection probes",
            "private page directories and stacks",
            "independent PIDs",
            "protected ring-0 kernel",
            "PS/2 dragging",
            "maximize and restore geometry",
            "minimize retains PID",
            "dock restores minimized clients",
            "close terminates task",
            "reopen allocates new PID",
            "kill terminates selected application",
            "other windows respond while shell sleeps",
        ],
    }
    (ROOT / "dist/multitask-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(
        "PASS: task lifecycle, protected system task and floating windows via real mouse/keyboard",
        flush=True,
    )
finally:
    g.close()
