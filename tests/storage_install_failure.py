#!/usr/bin/env python3
"""Inject EIO on data-sector write: setup must abort without committing boot."""
import os
import shutil
from guest import ROOT, TEST, Guest, memory, symbol_address, wait_until
from storage_interfaces import device_args
from storage_install import start_install

kind=os.environ["LUNA_STORAGE_KINDS"]
source=TEST/"fault-source.img"
target=TEST/"fault-target.img"
config=TEST/"blkdebug.conf"
shutil.copyfile(ROOT/"dist/LunaOS.img",source)
with target.open("wb") as stream:
    stream.truncate((4096 if kind in ("sdhci","emmc","emmc-acpi") else 128)*1024*1024)
    stream.write(b"DISPOSABLE WRITE ERROR TARGET")
config.write_text('[inject-error]\nevent = "write_aio"\nerrno = "5"\nsector = "1"\nonce = "on"\n')
args=device_args(kind,f"blkdebug:{config}:{target}")
g=Guest(source,extra_args=args,setup=True,ram=256)
try:
    start_install(g,1 if kind=="ide" else 5)
    def failed():
        g.read(0)
        page=int.from_bytes(memory(g,symbol_address("setup_page"),4),"little",signed=True)
        assert page!=9,"installer committed despite injected write error"
        return page==11
    wait_until(failed,60)
    phase=int.from_bytes(memory(g,symbol_address("install_phase"),4),"little")
    assert phase==1,f"failed before exercising data copy (phase {phase})"
    g.wait("SETUP: installation failed",10)
    assert "INSTALL: verified destination committed" not in g.transcript
finally:g.close()
with target.open("rb") as stream:
    stream.seek(510);assert stream.read(2)!=b"\x55\xaa","boot committed on failed target"
print(f"PASS: {kind} EIO at LBA1 aborted installation; boot signature not committed")
