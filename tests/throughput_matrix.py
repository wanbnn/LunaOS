#!/usr/bin/env python3
"""Controlled 100 MB native downloads through a real Linux TCP/TAP peer.

Every link and qdisc is confined to a disposable user/network namespace.
Reported target rates are test ceilings, not claims of achieved throughput.
"""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--rates", default="100,700,1000,10000")
parser.add_argument("--rtt-ms", type=float, default=30)
parser.add_argument("--inside", action="store_true")
parser.add_argument("--rate", type=int)
args = parser.parse_args()
base = ROOT / os.environ.get("LUNA_TEST_DIR", "build/test-throughput")
base.mkdir(parents=True, exist_ok=True)


def command(*words):
    subprocess.run(words, check=True, cwd=ROOT)


if args.inside:
    for kind in ("user", "net"):
        parent = os.environ.get(f"LUNA_PARENT_{kind.upper()}_NS")
        if not parent or parent == os.readlink(f"/proc/self/ns/{kind}"):
            raise SystemExit("Benchmark requires isolated user and network namespaces")
    command("ip", "link", "set", "lo", "up")
    command("ip", "tuntap", "add", "dev", "luna-test", "mode", "tap")
    command("ip", "addr", "add", "10.0.2.2/24", "dev", "luna-test")
    command("ip", "link", "set", "luna-test", "up")
    command("sysctl", "-w", "net.ipv4.tcp_wmem=4096 16777216 134217728")
    command(
        "tc",
        "qdisc",
        "add",
        "dev",
        "luna-test",
        "root",
        "netem",
        "delay",
        f"{args.rtt_ms}ms",
        "rate",
        f"{args.rate}mbit",
        "limit",
        "100000",
    )
    env = os.environ.copy()
    env.update(LUNA_NET_BACKEND="tap", LUNA_DOWNLOAD_BYTES="100000000", LUNA_DOWNLOAD_RAM="4096")
    subprocess.run([sys.executable, "tests/download_streaming.py"], check=True, cwd=ROOT, env=env)
else:
    results = []
    for rate in map(int, args.rates.split(",")):
        case = base / f"{rate}mbps-{args.rtt_ms:g}ms"
        case.mkdir(exist_ok=True)
        env = os.environ.copy()
        env["LUNA_TEST_DIR"] = str(case)
        for kind in ("user", "net"):
            env[f"LUNA_PARENT_{kind.upper()}_NS"] = os.readlink(f"/proc/self/ns/{kind}")
        with (case / "runner.log").open("w") as log:
            run = subprocess.run(
                [
                    "unshare",
                    "-Urn",
                    sys.executable,
                    __file__,
                    "--inside",
                    "--rate",
                    str(rate),
                    "--rtt-ms",
                    str(args.rtt_ms),
                ],
                cwd=ROOT,
                env=env,
                stdout=log,
                stderr=subprocess.STDOUT,
            )
        if run.returncode:
            print((case / "runner.log").read_text(), file=sys.stderr)
            raise SystemExit(run.returncode)
        report = json.loads((case / "report.json").read_text())
        report.update(
            target_mbps=rate,
            rtt_ms=args.rtt_ms,
            ideal_payload_seconds=100000000 * 8 / (rate * 1000000),
            achieved_fraction=report["server_send_mbps"] / rate,
            meets_85_percent=report["server_send_mbps"] >= rate * 0.85,
        )
        results.append(report)
        print(
            f"{rate} Mbps / {args.rtt_ms:g} ms: server {report['server_send_mbps']:.2f} Mbps, "
            f"end-to-end {report['command_mbps']:.2f} Mbps, SHA-256 PASS",
            flush=True,
        )
    (base / "matrix.json").write_text(json.dumps(results, indent=2) + "\n")
