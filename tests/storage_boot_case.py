#!/usr/bin/env python3
"""Actual release ISO + selected storage controller; works in BIOS or UEFI."""
import hashlib
import os
from guest import ROOT, TEST, Guest, UEFI
from storage_interfaces import device_args

kind = os.environ["LUNA_STORAGE_KINDS"]
labels = {"ide": "IDE", "ahci": "SATA/AHCI", "nvme": "NVMe", "usb": "USB",
          "sdhci": "eMMC/SDHCI", "emmc": "eMMC/SDHCI", "emmc-acpi": "eMMC/SDHCI",
          "raid": "RAID", "raid2": "RAID"}
assert kind in labels, kind
iso = ROOT / "dist/LunaOS.iso"
size_mib = 65536 if kind in ("emmc", "emmc-acpi") else 4096 if kind=="sdhci" else 128
target = TEST / (kind + "-iso-target.img")
marker = b"LUNA MATRIX ISO TEST " + kind.encode()
with target.open("wb") as stream:
    stream.truncate(size_mib * 1024**2)
    stream.write(marker)
before = hashlib.sha256(iso.read_bytes()).hexdigest()
args=device_args(kind,target)
if UEFI:
    # This kernel is i386 without PAE. Positive cases use its documented
    # addressable MMIO range; default high allocations are tested separately.
    args += ["-fw_cfg","name=opt/ovmf/X-PciMmio64Mb,string=0"]
g = Guest(iso, cdrom=True, ram=4096 if "emmc" in kind else 256,extra_args=args)
try:
    status = g.command("storage", "Storage:")
    if kind=="ide":
        assert "IDE1: QEMU HARDDISK" in status, status
        index="1"
    else:
        assert f"disk5 {labels[kind]} " in status and "[installable]" in status, status
        index="5"
    if kind=="emmc-acpi":
        assert "no PCI SDHCI host found" in status and "ACPI 80860F14:" in status, status
        def pci_devices(bus):
            for dev in bus.get("devices", []):
                yield dev
                if "pci_bridge" in dev:
                    yield from pci_devices(dev["pci_bridge"])
        for bus in g.qcommand("query-pci")["return"]:
            assert all(d["class_info"]["class"] != 0x0805 for d in pci_devices(bus))
    g.command("storage verify " + index, "two consistent physical reads")
    if UEFI:
        assert "UEFI: firmware-loaded RAM source" in g.transcript
    else:
        assert "ATAPI: read-only LunaOS CD installation source" in g.transcript
finally:
    g.close()
assert hashlib.sha256(iso.read_bytes()).hexdigest()==before
with target.open("rb") as stream:
    assert stream.read(len(marker))==marker
print(f"PASS: {kind} {'UEFI' if UEFI else 'BIOS'} ISO boot, device initialized, physical reads; ISO unchanged")
