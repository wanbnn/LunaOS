#!/usr/bin/env python3
"""Unsupported/absent storage must never become an installable disk."""
import json
import os
from pathlib import Path
import shutil
import subprocess
from guest import ROOT, TEST, Guest, env
from storage_interfaces import image_digest

source=TEST/"negative-source.img"
target=TEST/"negative-target.img"
shutil.copyfile(ROOT/"dist/LunaOS.img",source)
with target.open("wb") as stream:
    stream.truncate(128*1024*1024)
    stream.write(b"NEGATIVE STORAGE TEST DO NOT MODIFY")
base=["-drive",f"file={target},if=none,id=target,format=raw"]
report={}

def check(name,args,expected):
    before=image_digest(target)
    g=Guest(source,extra_args=args,ram=256)
    try:
        status=g.command("storage","Storage:")
        assert "disk5 " not in status and "[installable]" not in status, status
        assert expected in status,status
        print(f"PASS: {name}; no target registered/installable",flush=True)
    finally:g.close()
    assert image_digest(target)==before, name+" modified target"
    report[name]={"result":"PASS","verified":"no published native target; target fingerprint unchanged"}

check("xhci-no-driver",base+["-device","qemu-xhci,id=hc","-device","usb-storage,drive=target,bus=hc.0"],"USB PCI")
check("nvme-4kn",base+["-device","nvme,id=hc,serial=NEGATIVE0001","-device",
                       "nvme-ns,drive=target,bus=hc,logical_block_size=4096,physical_block_size=4096"],"NVMe PCI")
check("sdhci-empty",["-device","sdhci-pci"],"SDHCI phase 1")

original=(ROOT/"tests/fixtures/storage/emmc-acpi.asl").read_text()
variants={
    "acpi-absent":(original.replace("Return (0x0F)","Return (Zero)"),"no supported present SDHCI"),
    "acpi-sdio-uid2":(original.replace('Name (_UID, "1")','Name (_UID, "2")'),"no supported present SDHCI"),
    "acpi-ram-resource":(original.replace("0xFED80000","0x00100000"),"SDHCI phase 8"),
    "acpi-not-powered":(original.replace("Store (One, PWRD)","Store (Zero, PWRD)"),"unsupported or ambiguous MMIO"),
}
compiler=shutil.which("iasl") or str(ROOT/".tools/usr/bin/iasl")
for name,(text,expected) in variants.items():
    asl=TEST/(name+".asl");asl.write_text(text)
    prefix=TEST/name
    subprocess.run([compiler,"-p",str(prefix),str(asl)],check=True,stdout=subprocess.DEVNULL)
    env["LUNA_ACPI_STORAGE_FIXTURE"]="1"
    check(name,["-acpitable",f"file={prefix}.aml"],expected)
(TEST/"negative-report.json").write_text(json.dumps(report,indent=2)+"\n")
print("PASS: unsupported interface, 4Kn, absent media, ACPI status/UID/resources/power failure matrix")
