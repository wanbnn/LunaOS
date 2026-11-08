#!/usr/bin/env python3
"""Hold an AHCI request in QEMU and verify that desktop input still runs."""

import shutil
import time

from guest import ROOT, TEST, Guest, memory, symbol_address, wait_until
from storage_interfaces import device_args


def main():
    source = TEST / "storage-multitask-boot.img"
    target = TEST / "storage-multitask-target.img"
    shutil.copyfile(ROOT / "dist/LunaOS.img", source)
    with target.open("wb") as disk:
        disk.truncate(128 * 1024 * 1024)
        disk.write(b"LunaOS storage concurrency fixture")
    g = Guest(source, extra_args=device_args("ahci", target))
    try:
        g.open_app("terminal")
        g.wait("luna> ")
        storage_sleeps = symbol_address("storage_sleeps")
        old_sleeps = memory(g, storage_sleeps, 4)
        g.qcommand("block_set_io_throttle", {
            "device": "target", "bps": 256, "bps_rd": 0, "bps_wr": 0,
            "iops": 0, "iops_rd": 0, "iops_wr": 0,
        })
        g.buf = ""
        for char in "storage verify 5":
            mark = len(g.buf)
            g.serial.send(char.encode())
            deadline = time.monotonic() + 5
            while len(g.buf) == mark:
                assert time.monotonic() < deadline, f"No echo for {char!r}"
                g.read()
        g.serial.send(b"\n")
        g.buf = ""
        started = time.monotonic()
        wait_until(lambda: memory(g, storage_sleeps, 4) != old_sleeps)
        g.open_app("notes")
        document = symbol_address("document")
        g.key("z")
        wait_until(lambda: memory(g, document, 1) == b"z")
        g.read()
        assert "two consistent physical reads" not in g.buf, g.buf
        g.wait("two consistent physical reads", 8)
        print(f"PASS: Notes accepted input while AHCI read waited ({time.monotonic() - started:.2f}s)")
    finally:
        g.close()


if __name__ == "__main__":
    main()
