#!/usr/bin/env python3
"""Boot the actual ISO with 4 GiB RAM and a disposable 64 GiB eMMC.

Run once normally and once with LUNA_UEFI=1. Never writes a physical disk.
"""
import hashlib
from guest import ROOT, TEST, Guest, UEFI
from storage_interfaces import device_args

iso = ROOT / "dist/LunaOS.iso"
target = TEST / "emmc-64g-iso-target.img"
with target.open("wb") as stream:
    stream.truncate(64 * 1024**3)
    stream.write(b"LUNA ISO EMMC READ TEST")
before = hashlib.sha256(iso.read_bytes()).digest()
guest = Guest(iso, cdrom=True, ram=4096, extra_args=device_args("emmc", target))
try:
    status = guest.command("storage", "disk5 eMMC/SDHCI MMC/eMMC user area [installable]")
    assert "eMMC/SDHCI PCI" in status and "driver ready" in status, status
    guest.command("storage verify 5", "two consistent physical reads")
    if UEFI:
        assert "UEFI: firmware-loaded RAM source" in guest.transcript
    else:
        assert "ATAPI: read-only LunaOS CD installation source" in guest.transcript
finally:
    guest.close()
assert hashlib.sha256(iso.read_bytes()).digest() == before
with target.open("rb") as stream:
    assert stream.read(len(b"LUNA ISO EMMC READ TEST")) == b"LUNA ISO EMMC READ TEST"
print(f"PASS: {'UEFI' if UEFI else 'BIOS'} ISO, 4 GiB RAM, 64 GiB eMMC installable; physical read; unchanged ISO")
