#!/usr/bin/env python3
"""Exercise real BIOS-booted heap/DMA growth with multiple QEMU RAM sizes.
Use LUNA_TEST_DIR=/tmp/luna-memory-test on WSL for Unix sockets.
"""
import json
import re
import shutil
from guest import ROOT, TEST, Guest

results = []
for ram in (64, 256, 4096):
    disk = TEST / f"memory-{ram}.img"
    shutil.copyfile(ROOT / "dist/LunaOS.img", disk)
    g = Guest(disk, ram=ram)
    try:
        before = g.command("free", "Physical memory / firmware map")
        def amount(label, text):
            match = re.search(re.escape(label) + r"\s*(\d+) KiB", text)
            assert match, text
            return int(match.group(1))
        firmware = amount("Firmware usable RAM:", before)
        addressable = amount("Addressable RAM (i386 non-PAE):", before)
        available = amount("Free pages (shared by heap and DMA):", before)
        assert firmware > (ram - 3) * 1024
        assert addressable > (min(ram, 3072) - 3) * 1024
        reserved = amount("Reserved (kernel/boot/mappings/metadata):", before)
        allocated = amount("Allocated pages (heap + DMA):", before)
        assert available + reserved + allocated == addressable
        assert available > (4 * 1024 if ram == 64 else addressable // 2)
        size = 4 if ram == 64 else 96
        g.command(f"memtest {size}", f"MEMORY PASS: {size} MiB", timeout=40)
        # Tests actual libc malloc + QuickJS allocation beyond the former 4 MiB quota.
        if ram > 64:
            g.command("js 'new Uint8Array(12*1024*1024).length'", "12582912", timeout=20)
        after = g.command("free", "Physical memory / firmware map")
        assert amount("Free pages (shared by heap and DMA):", after) == available
        if ram == 4096:
            assert "requires PAE or x86-64" in after
        results.append({"ram_mib": ram, "firmware_usable_kib": firmware,
                        "addressable_kib": addressable, "free_kib": available,
                        "allocation_test_mib": size})
    finally:
        g.close()
(ROOT / "dist/memory-report.json").write_text(json.dumps({"result": "PASS", "cases": results}, indent=2) + "\n")
print("PASS: native 64/256/4096 MiB boots, heap+DMA allocation/release, >64 MiB use and >4 MiB JavaScript")
