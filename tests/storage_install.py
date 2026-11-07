#!/usr/bin/env python3
"""Destructive installer exercise on dedicated throwaway QEMU native disks.

NEVER use a hardware device path here. These tests create a new raw image in
build/test and verify setup's entire copy/flush/readback/commit sequence.
"""
import json
import os
import shutil

from storage_interfaces import device_args, image_digest, supports_kind
from guest import ROOT, TEST, Guest, memory, symbol_address, wait_until

SOURCE=ROOT/"dist/LunaOS.img"
def stage(g, expected, timeout=40):
    def reached():
        page=int.from_bytes(memory(g,symbol_address("setup_page"),4),"little",signed=True)
        assert page != 11, f"installer failed on page 11\n{g.transcript[-2500:]}"
        return page==expected
    wait_until(reached,timeout)

def type_text(g,value):
    for char in value:
        g.key({' ':'spc','-':'minus','_':'underscore'}.get(char,char))

report={}
for kind in os.environ.get("LUNA_STORAGE_KINDS", "ahci nvme usb sdhci emmc raid raid2").split():
    if not supports_kind(kind):
        report[kind]={"result":"SKIP","reason":"QEMU emmc model unavailable"}
        continue
    source=TEST/f"storage-install-{kind}-source.img"
    target=TEST/f"storage-install-{kind}-target.img"
    shutil.copyfile(SOURCE,source)
    with target.open("wb") as stream:
        stream.truncate((4096 if kind in ("sdhci", "emmc") else 128)*1024*1024)
    with target.open("r+b") as stream:
        stream.write(b"DISPOSABLE INSTALL TARGET - "+kind.encode())
    before=image_digest(target)
    extras=["-drive",f"file={target},format=raw,if=none,id=install_target"]
    if kind=="ahci":
        extras += ["-device","ich9-ahci,id=testahci",
                   "-device","ide-hd,drive=install_target,bus=testahci.0"]
    elif kind=="nvme":
        extras += ["-device","nvme,serial=INSTALL0001,drive=install_target"]
    else:
        extras=device_args(kind,target)
    g=Guest(source,extra_args=extras,setup=True,ram=256)
    try:
        stage(g,0)
        g.key("ret");stage(g,1)
        g.key("ret");stage(g,2)
        wait_until(lambda:memory(g,symbol_address("detected"),1)==b"\x01",30)
        g.key("ret");stage(g,3)
        g.key("ret");stage(g,4)
        g.key("ret");stage(g,5)
        type_text(g,"storageuser")
        g.key("tab");type_text(g,"testingpass88")
        g.key("tab");type_text(g,"testingpass88")
        g.key("ret");stage(g,6)
        # Legacy IDE0 is the protected source; the first native disk is #5.
        g.key("down")
        g.key("ret");stage(g,7)
        selected=int.from_bytes(memory(g,symbol_address("selected_disk"),4),"little",signed=True)
        assert selected==5, f"unexpected destination {selected}"
        language=int.from_bytes(memory(g,symbol_address("wizard_language"),4),"little")
        type_text(g,("apagar " if language else "erase ")+str(selected))
        g.key("ret");stage(g,8)
        stage(g,9,600)
        g.wait("INSTALL: verified destination committed", 10)
    finally:
        g.close()
    after=image_digest(target)
    assert before != after,kind+" installer did not write target"
    with SOURCE.open("rb") as orig, target.open("rb") as installed:
        orig.seek(512);installed.seek(512)
        assert orig.read(1024*1024)==installed.read(1024*1024),kind+" boot payload mismatch"
        installed.seek(510)
        assert installed.read(2)==b"\x55\xaa",kind+" missing boot signature"
    report[kind]={"result":"PASS","target_size":target.stat().st_size,
                  "verified":"installer selected native disk5, copied, read-back verified, committed boot"}
    print("PASS: destructive install only to disposable QEMU "+kind+" image",flush=True)
(ROOT/"dist"/"storage-install-report.json").write_text(json.dumps(report,indent=2)+"\n")
