#!/usr/bin/env python3
"""Validate the ISO structure and boot without any raw disk attached."""
import hashlib
import json
import struct
from guest import ROOT,Guest,memory,symbol_address

path=ROOT/'dist/LunaOS.iso';data=path.read_bytes()
manifest=json.loads((ROOT/'dist/iso-manifest.json').read_text())
assert len(data)<=120_000_000 and hashlib.sha256(data).hexdigest()==manifest['sha256']
assert data[16*2048:16*2048+7]==b'\x01CD001\x01'
assert data[17*2048:17*2048+7]==b'\x00CD001\x01'
cat_lba=struct.unpack_from('<I',data,17*2048+71)[0];catalog=data[cat_lba*2048:(cat_lba+1)*2048]
assert sum(struct.unpack('<16H',catalog[:32]))&65535==0
assert catalog[32:34]==bytes([0x88,4])
assert struct.unpack_from('<I',catalog,40)[0]==manifest['image_lba']
embedded=data[manifest['image_lba']*2048:]
assert hashlib.sha256(embedded).hexdigest()==manifest['embedded_img_sha256']
assert embedded[510:512]==b'\x55\xaa'
g=Guest(path,setup=True,cdrom=True)
try:
    assert 'ATAPI: read-only LunaOS CD installation source' in g.buf
    assert int.from_bytes(memory(g,symbol_address('source_bytes'),4),'little')>1_000_000
    g.screenshot('iso-setup')
    g.key('esc');g.command('cat /etc/luna-release','LunaOS')
    g.command('echo optical > /tmp/cd-test')
    g.command('sync','files remain in RAM')
finally:g.close()
assert hashlib.sha256(path.read_bytes()).hexdigest()==manifest['sha256']
(ROOT/'dist/iso-report.json').write_text(json.dumps({'result':'PASS','iso_sha256':manifest['sha256'],
    'verified':['ISO9660 volume and El Torito boot catalog checksum','native BIOS CD boot without HDD','native ATAPI embedded-image reads','setup and live desktop','optical medium remains read-only'],
    'uefi':False},indent=2)+'\n')
print('PASS: ISO structure, native CD boot, ATAPI and read-only live mode')
