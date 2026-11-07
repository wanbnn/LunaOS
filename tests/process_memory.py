#!/usr/bin/env python3
"""Actual per-PID physical RAM growth, attribution and browser lifecycle in QEMU.
Run with LUNA_TEST_DIR=/tmp/luna-process-memory after make all.
"""
import http.server
import json
import re
import shutil
import threading
import time
from guest import ROOT, TEST, Guest, memory, symbol_address, wait_until

class Fixture(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b'''<!doctype html><html><body><h1>Per-process RAM</h1>
<script>globalThis.kept = new Uint8Array(8*1024*1024); kept[0]=73;</script>
<p>This browser owns an eight MiB live allocation.</p></body></html>'''
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *args):
        pass

server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Fixture)
threading.Thread(target=server.serve_forever, daemon=True).start()
path = TEST / "process-memory.img"
shutil.copyfile(ROOT / "dist/LunaOS.img", path)
g = Guest(path, ram=256)
def processes():
    text = g.command("ps", "RAM KiB  HEAP KiB")
    result = {}
    pattern = r"^(\d+)\s+(\d+)\s+(\w+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(.+)$"
    for match in re.finditer(pattern, text, re.M):
        pid, ring, state, ram, heap, stack, switches, name = match.groups()
        result[name.strip()] = {"pid": int(pid), "ram_kib": int(ram), "heap_kib": int(heap), "stack_kib": int(stack)}
    assert "Luna kernel / compositor" in result, text
    return result

def settled():
    wait_until(lambda: memory(g, symbol_address("browser_loading"), 1) == b"\0", 30)
    wait_until(lambda: memory(g, symbol_address("ui_dirty"), 1) == b"\0", 30)

try:
    g.command("browse luna://welcome")
    settled()
    before = processes()
    baseline = before["Luna Browser"]
    shell_before = before["lunaSH"]["heap_kib"]
    url = f"http://10.0.2.2:{server.server_port}/large"
    g.command(f"browse {url}")
    # Wait for the navigation event to be consumed, not just its pending flag.
    wait_until(lambda: memory(g, symbol_address("browser_url"), 256).split(b"\0")[0].decode() == url, 30)
    settled()
    large = processes()
    browser = large["Luna Browser"]
    assert browser["heap_kib"] >= baseline["heap_kib"] + 8*1024, (before, large)
    assert browser["ram_kib"] == browser["heap_kib"] + browser["stack_kib"] + 8
    assert large["lunaSH"]["heap_kib"] == shell_before
    # Process-owned pages must not also inflate the kernel's RAM row.
    assert large["Luna kernel / compositor"]["ram_kib"] <= before["Luna kernel / compositor"]["ram_kib"] + 256
    g.open_app("tasks")
    settled()
    g.screenshot("process-ram")
    old_pid = browser["pid"]
    g.command(f"kill {old_pid}", "Task terminated")
    after_close = processes()
    assert "Luna Browser" not in after_close
    g.command(f"browse {url}")
    wait_until(lambda: memory(g, symbol_address("browser_url"), 256).split(b"\0")[0].decode() == url, 30)
    settled()
    reopened = processes()["Luna Browser"]
    assert reopened["pid"] != old_pid and reopened["heap_kib"] >= 8*1024
    g.command("browse luna://welcome")
    wait_until(lambda: memory(g, symbol_address("browser_url"), 256).split(b"\0")[0] == b"luna://welcome", 30)
    settled()
    small = processes()["Luna Browser"]
    assert small["heap_kib"] < reopened["heap_kib"] - 7*1024
    g.command(f"kill {small['pid']}", "Task terminated")
    assert "Luna Browser" not in processes()
    (ROOT / "dist/process-memory-report.json").write_text(json.dumps({
        "result": "PASS", "before": before, "large_document": large,
        "reopened": reopened, "after_navigation": small,
        "verified": ["PID-exclusive arenas", "physical RAM includes heap, stacks and tables",
                     "8 MiB browser growth attributed to browser", "no kernel double count",
                     "navigation shrinks RAM", "close/reopen uses new PID without stale pointers"]
    }, indent=2) + "\n")
    print("PASS: per-process RAM, real browser growth, no kernel double counting, navigation release and close/reopen lifecycle")
finally:
    g.close()
    server.shutdown()
