#!/usr/bin/env python3
"""Exercise the production RTL8822BE driver and WPA2 through QEMU PCIe/DMA."""

import hashlib
import json
import os
import shutil
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / "build/wifi-join-qemu"
WORK.mkdir(parents=True, exist_ok=True)
os.environ["LUNA_TEST_DIR"] = str(WORK)
os.environ["LUNA_TEST_OFFLINE"] = "1"
os.environ["LUNA_QEMU"] = str(ROOT / ".tools/qemu-storage-build/qemu-system-i386")
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tests/fixtures/wifi"))
# The guest harness reads these environment settings at import time.
from access_point import AccessPoint  # noqa: E402
from guest import Guest, memory, symbol_address, wait_until  # noqa: E402


def click(x, y):
    events = [
        {"type": "abs", "data": {"axis": "x", "value": x * 32767 // 1023}},
        {"type": "abs", "data": {"axis": "y", "value": y * 32767 // 767}},
    ]
    guest.qcommand("input-send-event", {"events": events})
    for down in (True, False):
        guest.qcommand(
            "input-send-event",
            {"events": [{"type": "btn", "data": {"down": down, "button": "left"}}]},
        )
        time.sleep(0.1)


def settings_position():
    guest.open_app("settings")
    window = struct.unpack("<9iIBBBx", memory(guest, symbol_address("windows") + 44 * 4, 44))
    return window[1] + 20, window[2] + 46


def connect(password="password"):
    x, y = settings_position()
    click(x + 260, y + 278)
    wait_until(lambda: memory(guest, symbol_address("network_password"), 1) == b"\1")
    for key in password:
        guest.key(key)
    guest.key("ret")


def wait_state(needle):
    deadline = time.monotonic() + 30
    while True:
        assert not ap.errors, ap.errors
        state = guest.command("wifi status")
        if needle in state:
            return state
        assert time.monotonic() < deadline, state
        time.sleep(0.2)


ap = AccessPoint()
guest = None
report = {"result": "RUNNING", "transport": "RTL8822BE PCIe/DMA and independent 802.11 peer"}
(WORK / "report.json").write_text(json.dumps(report, indent=2) + "\n")
try:
    disk = WORK / "join.img"
    shutil.copyfile(ROOT / "dist/LunaOS.img", disk)
    device = (
        f"lab-rtw8822be,firmware={ROOT}/assets/firmware/rtw88/rtw8822b_fw.bin,air-port={ap.port}"
    )
    guest = Guest(disk, network=False, extra_args=["-device", device])
    diagnostic = guest.command("wifidiag")
    assert "experimental STA TX/RX ready" in diagnostic, diagnostic
    x, y = settings_position()
    click(x + 65, y + 62 + 5 * 47 + 18)
    wait_state("link=2")
    guest.command("wifi results", "Luna-Lab-WPA2 channel=1 security=WPA2-PSK")
    connect()
    wait_state("error=13")
    assert "auth-rejected" in ap.events
    connect("wrongpass")
    wait_state("error=110")
    assert "bad-password" in ap.events
    connect()
    wait_state("link=5")
    assert "WPA2-authorized" in ap.events and "DHCP-ack" in ap.events, ap.events
    status = guest.command("net", "Wi-Fi/generic interface: IPv4 ready")
    assert "192.168.50.9" in status, status
    guest.command("ping 1.1.1.1", "Reply")
    guest.command("ping lab.test", "Reply")
    guest.command("wget http://lab.test/ /tmp/wifi-page", "Saved")
    guest.command("cat /tmp/wifi-page", "PCIe Wi-Fi works")
    guest.command("browse http://lab.test/")
    wait_until(
        lambda: memory(guest, symbol_address("browser_url"), 256).split(b"\0")[0]
        == b"http://lab.test/",
        30,
    )
    wait_until(lambda: memory(guest, symbol_address("browser_loading"), 1) == b"\0", 60)
    css_size = int.from_bytes(memory(guest, symbol_address("sheet_length"), 4), "little")
    css_ptr = int.from_bytes(memory(guest, symbol_address("sheet_text"), 4), "little")
    assert css_size == len(ap.css) + 1, (css_size, guest.command("net"), ap.events, ap.errors)
    assert memory(guest, css_ptr, len(ap.css)) == ap.css
    guest.command("ping 1.1.1.1", "Reply")
    diagnostic = guest.command("wifidiag")
    assert "mic=0" in diagnostic and "replay=1" in diagnostic, diagnostic
    ap.send(ap.header(0xC0) + struct.pack("<H", 3))
    wait_state("link=2")
    guest.command("wifidiag", "AP disconnect reason=3")
    connect()
    wait_state("link=5")
    guest.command("ping 1.1.1.1", "Reply")
    assert not ap.errors, ap.errors
    report = {
        "result": "PASS",
        "image_sha256": hashlib.sha256((ROOT / "dist/LunaOS.img").read_bytes()).hexdigest(),
        "transport": "RTL8822BE PCIe TXBD/RXBD and raw 802.11 peer, Ethernet NIC disabled",
        "verified": [
            "firmware transfer",
            "scan",
            "authentication refusal",
            "wrong password timeout",
            "manual retry",
            "WPA2 four-way handshake",
            "CCMP authenticated TX/RX",
            "DHCP",
            "ARP",
            "DNS",
            "ICMP",
            "HTTP",
            "350 KB stylesheet",
            "continued ICMP after transfer",
            "CCMP retry replay rejected",
            "AP disconnect and successful reconnection",
        ],
        "pending": [
            "firmware CPU execution",
            "physical RF and link-layer ACK timing",
            "MitraStar stability",
        ],
        "peer_events": ap.events,
    }
    (WORK / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(
        "PASS: native RTL8822BE PCIe/DMA -> WPA2 -> CCMP -> DHCP -> DNS/ICMP/HTTP/CSS and retry",
        flush=True,
    )
except BaseException as error:
    report.update(result="FAIL", error=repr(error))
    (WORK / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    raise
finally:
    if guest:
        (WORK / "serial.log").write_text(guest.transcript)
        guest.close()
    ap.close()
    (WORK / "peer.json").write_text(
        json.dumps({"events": ap.events, "errors": ap.errors}, indent=2) + "\n"
    )
