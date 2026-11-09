#!/usr/bin/env python3
"""End-to-end tests on the actual BIOS-booted disk, never the release disk."""

import datetime
import hashlib
import http.server
import json
import shutil
import threading
import time
import wave

import re
import sys
from guest import ROOT, TEST, Guest

# The live file system starts right after the kernel payload; keep this in
# step with the layout the kernel uses.
VOLUME_START = int(re.search(r"#define LUNA_VOLUME_START (\d+)u",
                             (ROOT / "kernel" / "install.h").read_text()).group(1))


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        body = b"<h1>Luna native TCP works</h1>\n<p>HTTP fixture: deterministic network test.</p>\n"
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Fixture)
threading.Thread(target=server.serve_forever, daemon=True).start()


shutil.copyfile(ROOT / "dist/LunaOS.img", TEST / "disk.img")
g = None
try:
    g = Guest(TEST / "disk.img")
    g.screenshot("desktop")
    g.command("uname", "LunaOS 0.3.0-dev i386")
    g.command("pwd", "/home/luna")
    g.command("mkdir work")
    g.command("cd work")
    g.command("pwd", "/home/luna/work")
    g.command('echo "hello lunar world" > note.txt')
    g.command("cat note.txt", "hello lunar world")
    g.command("cat note.txt | grep lunar | wc", "1 lines")
    g.command("echo second >> note.txt")
    g.command("tail note.txt", "second")
    g.command("cp note.txt copy.txt")
    g.command("mv copy.txt moved.txt")
    g.command("rm moved.txt")
    g.command('set MISSION "native kernel"')
    g.command("echo $MISSION", "native kernel")
    g.command("echo '|' '&&' '<'", "| && <")
    g.command("true || echo SKIP && echo CHAIN_OK", "CHAIN_OK")
    g.command('echo "unterminated', "unclosed quote")
    g.command("cat < note.txt", "hello lunar world")
    g.command("false && echo BAD || echo RECOVERED", "RECOVERED")
    g.command("cd ..")
    g.command("source hello.lsh", "Hello from lunaSH")
    g.command("echo persisted > saved.txt")
    g.command("sync", "snapshot committed")
    g.command("net", "RTL8139 ready")
    g.command("ping 10.0.2.2", "Reply from", timeout=15)
    port = server.server_address[1]
    g.command(f"wget http://10.0.2.2:{port}/fixture /tmp/http.txt", "Saved", timeout=20)
    g.command("cat /tmp/http.txt", "Luna native TCP works")
    g.command("audio", "AC97 / 48 kHz stereo")
    g.command("beep 660", "Tone started: AC97")
    time.sleep(0.4)
    g.command("cat /etc/licenses/Lucide.txt", "Lucide Icons and Contributors")
    g.open_app("terminal")
    g.buf = ""
    for k in ["e", "c", "h", "o", "spc", "k", "b", "d", "ret"]:
        g.key(k)
    g.wait("luna> ")
    assert "kbd" in g.buf
    print("PASS: PS/2 keyboard input")
    g.screenshot("terminal")
    g.open_app("files")
    g.screenshot("files")
    g.open_app("browser")
    g.screenshot("browser")
    g.open_app("settings")
    g.screenshot("controls")
    g.close()
    g = None
    with wave.open(str(TEST / "audio.wav"), "rb") as recording:
        audio = recording.readframes(recording.getnframes())
        assert audio and any(audio), "AC97 DMA produced no nonzero PCM output"
    shutil.copyfile(TEST / "audio.wav", ROOT / "dist/audio-test.wav")
    print("PASS: AC97 DMA produced nonzero PCM in QEMU recording")
    g = Guest(TEST / "disk.img")
    g.command("cat saved.txt", "persisted")
    g.command("echo newer > saved.txt")
    g.command("sync", "snapshot committed")
    g.close()
    g = None
    # Damage the newest snapshot header; the previous completed generation must survive.
    with (TEST / "disk.img").open("r+b") as f:
        f.seek(VOLUME_START * 512)
        f.write(b"INVALID!")
    g = Guest(TEST / "disk.img", network=False, ram=64)
    g.command("net", "no supported RTL8139")
    g.command("cat saved.txt", "persisted")
    g.close()
    g = None
    report = {
        "result": "PASS",
        "utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "image_sha256": hashlib.sha256((ROOT / "dist/LunaOS.img").read_bytes()).hexdigest(),
        "verified": [
            "custom BIOS boot",
            "64 MiB RAM",
            "native GUI and Lucide assets",
            "PS/2 keyboard",
            "lunaSH parser and commands",
            "LunaFS persistence across boot",
            "fallback after corrupt snapshot header",
            "RTL8139 Ethernet and ARP",
            "IPv4 ICMP ping",
            "TCP HTTP real fixture download",
            "AC97 DMA playback captured as nonzero WAV audio",
            "offline boot",
        ],
        "not_verified": [
            "physical hardware",
            "DNS end-to-end",
            "audible host output",
            "Wi-Fi or Bluetooth (not implemented)",
        ],
    }
    (ROOT / "dist/test-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(
        "PASS: boot, desktop, keyboard, shell, filesystem, persistence, Ethernet/ARP/ICMP/TCP/HTTP and 64 MiB offline boot"
    )
finally:
    if g:
        g.close()
    server.shutdown()
