#!/usr/bin/env python3
"""Check ISO structure and exact association with the raw LunaOS image."""
import hashlib
import json
import struct
from pathlib import Path

iso=Path('dist/LunaOS.iso').read_bytes()
manifest=json.loads(Path('dist/iso-manifest.json').read_text())
raw=Path('dist/LunaOS.img').read_bytes()
assert len(iso)==manifest['bytes'] and len(iso)<=120_000_000
assert hashlib.sha256(iso).hexdigest()==manifest['sha256']
assert hashlib.sha256(raw).hexdigest()==manifest['base_img_sha256']
assert iso[16*2048:16*2048+7]==b'\x01CD001\x01'
assert struct.unpack_from('<I',iso,16*2048+80)[0]*2048==len(iso)
assert iso[17*2048:17*2048+7]==b'\x00CD001\x01'
cat_lba=struct.unpack_from('<I',iso,17*2048+71)[0]
catalog=iso[cat_lba*2048:(cat_lba+1)*2048]
assert catalog[0:2]==b'\x01\x00' and catalog[30:32]==b'\x55\xaa'
assert sum(struct.unpack('<16H',catalog[:32]))&65535==0
assert catalog[32:34]==bytes([0x88,4])
assert struct.unpack_from('<I',catalog,40)[0]==manifest['image_lba']
image=iso[manifest['image_lba']*2048:]
assert len(image)==len(raw) and image[512:]==raw[512:]
assert image[510:512]==b'\x55\xaa' and image[444:446]==b'\x01\xcd'
assert hashlib.sha256(image).hexdigest()==manifest['embedded_img_sha256']
checksums=Path('dist/SHA256SUMS').read_text()
assert manifest['sha256']+'  LunaOS.iso' in checksums
assert manifest['base_img_sha256']+'  LunaOS.img' in checksums
print('PASS: ISO9660 / El Torito / embedded image / SHA-256 / size budget')
