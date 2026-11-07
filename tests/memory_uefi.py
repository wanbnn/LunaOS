#!/usr/bin/env python3
"""Verify physical allocation preserves firmware ACPI/NVS and the UEFI RAM disk.
Run with LUNA_UEFI=1 LUNA_TEST_DIR=/tmp/luna-memory-uefi after make all.
"""
import shutil
from guest import ROOT, TEST, Guest, UEFI

assert UEFI, "Set LUNA_UEFI=1 to exercise the UEFI handoff"
path = TEST / "memory-uefi.img"
shutil.copyfile(ROOT / "dist/LunaOS.img", path)
guest = Guest(path, ram=256)
try:
    before = guest.command("free", "Physical memory / firmware map")
    guest.command("memtest 96", "MEMORY PASS: 96 MiB", timeout=40)
    guest.command("fwinfo iwlwifi-so-a0-gf-a0-89.ucode", "Firmware bytes: 1737012", timeout=30)
    guest.command("js 'new Uint8Array(12*1024*1024).length'", "12582912", timeout=20)
    after = guest.command("free", "Physical memory / firmware map")
    label = "Free pages (shared by heap and DMA):"
    assert before.split(label)[1].splitlines()[0] == after.split(label)[1].splitlines()[0]
finally:
    guest.close()
print("PASS: UEFI 256 MiB, 96 MiB allocation/release, reserved boot image preserved, Intel firmware and growing JS heap")
