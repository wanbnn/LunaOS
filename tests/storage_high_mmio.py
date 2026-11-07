#!/usr/bin/env python3
"""Default OVMF high 64-bit BARs must not wrap into the i386 MMIO window."""
import json
from guest import ROOT, TEST, Guest, UEFI
from storage_interfaces import device_args, image_digest
assert UEFI,"Requires OVMF"
report={}
for kind in ("nvme","raid","raid2"):
    target=TEST/(kind+"-high-mmio.img")
    with target.open("wb") as stream:stream.truncate(128*1024*1024)
    before=image_digest(target)
    g=Guest(ROOT/"dist/LunaOS.iso",cdrom=True,ram=256,extra_args=device_args(kind,target))
    try:
        status=g.command("storage","Storage:")
        assert "disk5 " not in status and "[installable]" not in status,status
        high=[]
        for bus in g.qcommand("query-pci")["return"]:
            for dev in bus["devices"]:
                if dev["class_info"]["class"] not in (0x0108,0x0104):continue
                high += [r["address"] for r in dev.get("regions",[]) if r.get("type")=="memory" and r["address"]>0xffffffff]
        assert high,"Fixture did not allocate high MMIO; rejection was not exercised"
        report[kind]={"result":"PASS","high_mmio_addresses":high,"verified":"not registered; no 32-bit address truncation"}
    finally:g.close()
    assert image_digest(target)==before
    print(f"PASS: {kind} unsupported high MMIO rejected without writes",flush=True)
(TEST/"high-mmio-report.json").write_text(json.dumps(report,indent=2)+"\n")
