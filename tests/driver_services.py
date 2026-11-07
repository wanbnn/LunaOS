#!/usr/bin/env python3
"""Run native service tests against QEMU EDU DMA/INTx and binary firmware."""
import hashlib
import json
import shutil
import sys
from guest import ROOT, TEST, Guest

sys.path.insert(0, str(ROOT / 'tools'))
from firmwarepack import build_archive, SECTOR, fnv

fixture_root = TEST / 'firmware'
fixture_dir = fixture_root / 'luna-test'
fixture_dir.mkdir(parents=True, exist_ok=True)
payload = bytes((i * 37) & 255 for i in range(70000))
(fixture_dir / 'pattern.bin').write_bytes(payload)
archive = build_archive(fixture_root)
disk = TEST / 'services.img'
shutil.copyfile(ROOT / 'dist/LunaOS.img', disk)
with disk.open('r+b') as f:
    f.seek(SECTOR * 512)
    f.write(archive)
g = Guest(disk, extra_args=['-device', 'edu'])
try:
    g.command('drvinfo', 'Native driver services v1')
    result = g.command('drvtest', 'DRIVER SERVICES PASS', timeout=30)
    assert 'SKIP:' not in result and 'FAIL:' not in result, result
    assert 'DMA round trip binary payload' in result
    assert 'hardware interrupt / shared handlers' in result
    # A second run detects unreleased DMA pages, mappings, claims and IRQ slots.
    g.command('drvtest', 'DRIVER SERVICES PASS', timeout=30)
    info = g.command('fwinfo luna-test/pattern.bin', 'Firmware bytes: 70000')
    assert f'{fnv(payload):08x}' in info.lower(), info
    g.command('fwinfo missing.bin', 'firmware error: -2')
finally:
    g.close()
# Corrupt the binary payload while keeping the directory/header intact.
with disk.open('r+b') as f:
    f.seek(SECTOR * 512 + 1024 + 130)
    byte = f.read(1)
    f.seek(-1, 1)
    f.write(bytes([byte[0] ^ 0xff]))
g = Guest(disk, extra_args=['-device', 'edu'])
try:
    g.command('fwinfo luna-test/pattern.bin', 'firmware error: -5')
finally:
    g.close()
report = {
    'result': 'PASS',
    'image_sha256': hashlib.sha256((ROOT/'dist/LunaOS.img').read_bytes()).hexdigest(),
    'verified': [
        'E820-backed coherent DMA allocation, mask, boundary and stale-handle checks',
        'IRQ-save locks and rejection of blocking with IRQs disabled',
        'bounded work queue, duplicate suppression, cancellation, self requeue',
        'timer deadlines, cancellation, completion and timeout',
        'reference-counted packets and queue ownership',
        'PCI BAR probing, exclusive claims, MMIO access and bounds',
        'real EDU shared INTx interrupt and deferred work',
        'real EDU bidirectional bus-master DMA with byte-for-byte comparison',
        '70000-byte binary firmware with embedded NUL bytes and corruption rejection',
        'repeated service run without leaked driver resources',
    ],
    'not_implemented': ['MSI/APIC', 'IOMMU', 'Linux mac80211/cfg80211 ABI',
                        'functional Wi-Fi radio driver', 'USB/Bluetooth HCI'],
}
(ROOT/'dist/driver-services-report.json').write_text(json.dumps(report, indent=2)+'\n')
print('PASS: native driver services, real PCI DMA/IRQ, firmware integrity and teardown')
