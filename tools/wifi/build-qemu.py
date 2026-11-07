#!/usr/bin/env python3
"""Build test-only Wi-Fi models into the pinned storage QEMU tree."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
ROOT = Path(__file__).resolve().parents[2]
source = ROOT / '.tools/qemu-storage-src'
build = ROOT / '.tools/qemu-storage-build'
if not (build / 'build.ninja').exists():
    subprocess.run(['python3', str(ROOT / 'tools/storage/build-qemu.py')], check=True)
if (source / 'VERSION').read_text().strip() != '10.2.0':
    raise SystemExit('Wi-Fi models require pinned QEMU 10.2.0; rebuild the storage fixture')
model = ROOT / 'tests/fixtures/wifi/rtw8822be.c'
shutil.copyfile(model, source / 'hw/net/lab_rtw8822be.c')
meson = source / 'hw/net/meson.build'
entry = "system_ss.add(when: 'CONFIG_PCI', if_true: files('lab_rtw8822be.c'))\n"
content = meson.read_text()
if entry not in content:
    meson.write_text(content + '\n' + entry)
env = os.environ.copy()
env['PYTHONPATH'] = str(ROOT / '.tools/python')
env['LD_LIBRARY_PATH'] = str(ROOT / '.tools/usr/lib/x86_64-linux-gnu')
env['PKG_CONFIG_SYSROOT_DIR'] = str(ROOT / '.tools')
env['PKG_CONFIG_LIBDIR'] = str(ROOT / '.tools/usr/lib/x86_64-linux-gnu/pkgconfig')
subprocess.run(['ninja', '-j4', 'qemu-system-i386', 'qemu-system-x86_64'], cwd=build, env=env, check=True)
(build / 'wifi-fixture.json').write_text(json.dumps({
    'qemu': '10.2.0', 'model_sha256': hashlib.sha256(model.read_bytes()).hexdigest(),
    'scope': 'partial RTL8822BE firmware transfer and passive RX; no firmware CPU or TX'
}, indent=2)+'\n')
