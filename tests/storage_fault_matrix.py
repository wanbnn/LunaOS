#!/usr/bin/env python3
"""Standalone EIO subset of the full matrix, reusable when changing error paths."""
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import sys
from storage_matrix import KINDS, ROOT, invoke

results={}
with ThreadPoolExecutor(max_workers=3) as pool:
    jobs={pool.submit(invoke,kind,"fault-install","storage_install_failure.py"):kind for kind in KINDS}
    for job in as_completed(jobs):
        results[jobs[job]]=job.result()
report={"kernel_sha256":hashlib.sha256((ROOT/"build/kernel.bin").read_bytes()).hexdigest(),"cases":results,
        "result":"PASS" if len(results)==len(KINDS) and all(r["result"]=="PASS" for r in results.values()) else "FAIL"}
(ROOT/"dist/storage-fault-report.json").write_text(json.dumps(report,indent=2)+"\n")
sys.exit(0 if report["result"]=="PASS" else 1)
