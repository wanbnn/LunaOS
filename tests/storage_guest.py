#!/usr/bin/env python3
"""Non-destructive QEMU AHCI/NVMe discovery and sector-read smoke test."""
from pathlib import Path
import hashlib
import re
import shutil
from guest import ROOT, TEST, Guest

boot=TEST/"storage-boot.img"
sata=TEST/"storage-sata.img"
nvme=TEST/"storage-nvme.img"
shutil.copyfile(ROOT/"dist/LunaOS.img",boot)
for path,signature in [(sata,b"AHCI TEST DISK"),(nvme,b"NVME TEST DISK")]:
    with path.open("wb") as fd: fd.truncate(128*1024*1024)
    with path.open("r+b") as fd: fd.write(signature)
before={p:hashlib.sha256(p.read_bytes()).hexdigest() for p in [boot,sata,nvme]}
extras=[
    "-device","ich9-ahci,id=ahci0",
    "-drive",f"file={sata},if=none,id=sata0,format=raw",
    "-device","ide-hd,drive=sata0,bus=ahci0.0",
    "-drive",f"file={nvme},if=none,id=nvmedisk,format=raw",
    "-device","nvme,serial=LUNA0001,drive=nvmedisk",
    "-device","qemu-xhci,id=xhci0",
]
g=Guest(boot,extra_args=extras,ram=256)
try:
    found=g.command("storage", "Storage:")
    sata_match=re.search(r"disk(\d+) SATA/AHCI ",found)
    nvme_match=re.search(r"disk(\d+) NVMe ",found)
    if not sata_match or not nvme_match:
        print("CONTROLLER DIAGNOSTICS:",g.transcript[-4500:],flush=True)
        assert sata_match and nvme_match,found
    assert "USB PCI" in found,found
    for target in (sata_match.group(1),nvme_match.group(1)):
        g.command("storage verify "+target,"two consistent physical reads",timeout=40)
finally:
    g.close()
for path in [boot,sata,nvme]:
    assert hashlib.sha256(path.read_bytes()).hexdigest()==before[path],str(path)+" modified by read-only test"
print("PASS: QEMU AHCI and NVMe real PCI DMA-backed reads; no writes to test images",flush=True)
