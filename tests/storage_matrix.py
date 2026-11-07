#!/usr/bin/env python3
"""Strict, isolated storage simulation matrix. No physical disk paths accepted."""
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
from pathlib import Path
import hashlib
import json
import os
import signal
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
KINDS=("sdhci","emmc","emmc-acpi","ide","ahci","nvme","usb","raid","raid2")
BASE=ROOT/"build/storage-matrix"
BASE.mkdir(parents=True,exist_ok=True)
REPORT=ROOT/"dist/storage-matrix-report.json"
FIRMWARE=ROOT/".tools/qemu-storage-build"
fixture=json.loads((FIRMWARE/"fixture.json").read_text())
resume_cases={}
start=time.monotonic()
result={"started_utc":datetime.now(timezone.utc).isoformat(),"result":"RUNNING",
        "kernel_sha256":hashlib.sha256((ROOT/"build/kernel.bin").read_bytes()).hexdigest(),
        "iso_sha256":hashlib.sha256((ROOT/"dist/LunaOS.iso").read_bytes()).hexdigest(),
        "qemu_fixture":fixture,"cases":{},
        "limits":["Emulated devices, not physical hardware certification",
                  "No firmware boot-from-native-target claim",
                  "Positive UEFI cases use MMIO below 4GiB; high MMIO is a separate rejection case",
                  "xHCI/UAS, RST/VMD and unimplemented protocols are not supported by passing this matrix"]}


def invoke(case,phase,script,uefi=False):
    work=BASE/(case+"-"+phase)
    work.mkdir(exist_ok=True)
    env=os.environ.copy()
    env.update(LUNA_STORAGE_KINDS=case,LUNA_TEST_DIR=str(work),LUNA_TEST_OFFLINE="1",
               LUNA_QEMU=str(FIRMWARE/("qemu-system-x86_64" if uefi else "qemu-system-i386")),
               LUNA_UEFI="1" if uefi else "0",LUNA_STORAGE_REPORT=str(work/"install-report.json"))
    if case=="emmc-acpi":env["LUNA_ACPI_STORAGE_FIXTURE"]="1"
    else:env.pop("LUNA_ACPI_STORAGE_FIXTURE",None)
    log=work/"run.log"
    begin=time.monotonic()
    with log.open("w") as stream:
        process=subprocess.Popen([sys.executable,str(ROOT/"tests"/script)],cwd=ROOT,env=env,
                                 stdout=stream,stderr=subprocess.STDOUT,start_new_session=True)
        try:
            code=process.wait(timeout=1200)
        except subprocess.TimeoutExpired:
            code=-1
        finally:
            # A failed Guest constructor may leave its QEMU child alive.
            try:os.killpg(process.pid,signal.SIGTERM)
            except ProcessLookupError:pass
            try:process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                try:os.killpg(process.pid,signal.SIGKILL)
                except ProcessLookupError:pass
                process.wait(timeout=5)
    text=log.read_text()
    ok=code==0 and "PASS:" in text and "SKIP:" not in text
    data={"result":"PASS" if ok else "FAIL","seconds":round(time.monotonic()-begin,2),
          "log":str(log.relative_to(ROOT)),"exit_code":code}
    if not ok:print(f"FAIL: {case} {phase}\n{text[-4000:]}",flush=True)
    else:print(f"PASS: {case} {phase} ({data['seconds']} s)",flush=True)
    return data


def run_case(case):
    phases={}
    for phase,script,uefi in [("read","storage_interfaces.py",False),
                              ("install","storage_install.py",False),
                              ("fault-install","storage_install_failure.py",False),
                              ("uefi-iso","storage_boot_case.py",True)]:
        previous=resume_cases.get(case,{}).get("phases",{}).get(phase)
        if phase!="uefi-iso" and previous and previous.get("result")=="PASS":
            phases[phase]={**previous,"reused_from_same_kernel_and_iso":True}
            print(f"REUSE: {case} {phase} (same kernel/ISO fixture hashes)",flush=True)
        else:
            phases[phase]=invoke(case,phase,script,uefi)
        if phases[phase]["result"]!="PASS":break
    return case,{"result":"PASS" if len(phases)==4 and all(p["result"]=="PASS" for p in phases.values()) else "FAIL",
                 "phases":phases}


def save():
    REPORT.write_text(json.dumps(result,indent=2)+"\n")


def main():
    if os.environ.get("LUNA_STORAGE_RESUME")=="1":
        previous=json.loads(REPORT.read_text())
        for key in ("kernel_sha256","iso_sha256","qemu_fixture"):
            assert previous[key]==result[key],f"Cannot reuse evidence after changing {key}"
        archive=BASE/"initial-matrix-report.json"
        if not archive.exists():archive.write_text(json.dumps(previous,indent=2)+"\n")
        resume_cases.update(previous["cases"])
        faults=ROOT/"dist/storage-fault-report.json"
        if faults.exists():
            old_faults=json.loads(faults.read_text())
            assert old_faults["kernel_sha256"]==result["kernel_sha256"]
            for case,data in old_faults["cases"].items():
                resume_cases.setdefault(case,{"phases":{}})["phases"]["fault-install"]=data
        result["resumed_from"]=str(archive.relative_to(ROOT))
        # Preserve the initial high-BAR failures alongside the explicit
        # rejection tests; supported UEFI configurations are always rerun.
        for case in ("nvme","raid","raid2"):
            path=BASE/(case+"-uefi-iso")/"run.log"
            saved=path.with_name("run.high-mmio.log")
            if path.exists() and not saved.exists():saved.write_text(path.read_text())
    save()
    with ThreadPoolExecutor(max_workers=3) as pool:
        jobs=[pool.submit(run_case,kind) for kind in KINDS]
        for job in as_completed(jobs):
            case,data=job.result();result["cases"][case]=data;save()
    result["high_mmio"]=invoke("high-mmio","rejection","storage_high_mmio.py",True)
    result["negative"]=invoke("negative","rejection","storage_negative.py")
    result["seconds"]=round(time.monotonic()-start,2)
    result["result"]="PASS" if len(result["cases"])==len(KINDS) and all(c["result"]=="PASS" for c in result["cases"].values()) and result["negative"]["result"]=="PASS" and result["high_mmio"]["result"]=="PASS" else "FAIL"
    save()
    rows=["# Storage simulation results", "", f"Result: **{result['result']}**", "",
          "| Backend | Read | Install/write/readback/flush | EIO rejection | UEFI ISO |", "|---|---|---|---|---|"]
    for case in KINDS:
        phases=result["cases"][case]["phases"]
        rows.append("| "+case+" | "+" | ".join(phases.get(p,{}).get("result","NOT RUN") for p in ("read","install","fault-install","uefi-iso"))+" |")
    rows += ["", "Negative/rejection matrix: "+result["negative"]["result"], "High MMIO rejection: "+result["high_mmio"]["result"], "", "QEMU fixture: "+fixture["qemu"], "", *["- "+s for s in result["limits"]]]
    (ROOT/"dist/storage-matrix-report.md").write_text("\n".join(rows)+"\n")
    print(f"{result['result']}: storage matrix; report {REPORT}",flush=True)
    sys.exit(0 if result["result"]=="PASS" else 1)


if __name__=="__main__":
    main()
