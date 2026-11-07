#!/usr/bin/env python3
"""Validate Intel firmware archive reads in LunaOS; QEMU has no AX211 radio."""
import shutil
from guest import ROOT, TEST, Guest

image = TEST / "intel-firmware.img"
shutil.copyfile(ROOT / "dist/LunaOS.img", image)
firmware = ROOT / "assets/firmware/iwlwifi-so-a0-gf-a0-89.ucode"
guest = Guest(image)
try:
    result = guest.command("fwinfo iwlwifi-so-a0-gf-a0-89.ucode", "Firmware bytes: 1737012", timeout=30)
    assert "Firmware" in result
    guest.command("cat /etc/licenses/Intel-WiFi-Firmware.txt", "Intel Corporation")
    guest.command("wifidiag intel", "Radio, scan and connection inactive.")
    # A repeated large load detects leaked firmware allocations in the 2 MiB pool.
    guest.command("fwinfo iwlwifi-so-a0-gf-a0-89.ucode", "Firmware bytes: 1737012", timeout=30)
finally:
    guest.close()
print("PASS: Intel 1.74 MB firmware loaded twice by the native kernel, license installed, no false radio readiness")
