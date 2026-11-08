#!/usr/bin/env python3
"""Round-trip the release packer and reject wrong identity/layout inputs."""
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import zlib
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from updatepack import build, CHUNK, HEADER, RECORD, IMAGE_BYTES

root = Path(__file__).resolve().parents[1]
commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
output = root / 'build/update-test'
manifest = build(root / 'dist/LunaOS.img', output, commit)
image = (root / 'dist/LunaOS.img').read_bytes()
magic, version, size, chunk, count, revision = HEADER.unpack_from(manifest)
assert (magic, version, size, chunk, count, revision.decode()) == (b'LUNAUPD1', 1, IMAGE_BYTES, CHUNK, 248, commit)
assert len(manifest) == HEADER.size + count * RECORD.size
for i in range(count):
    sector, packed_bytes, digest = RECORD.unpack_from(manifest, HEADER.size + i * RECORD.size)
    assert not 8192 <= sector < 16384
    raw = zlib.decompress((output / f'chunk-{i:03d}.z').read_bytes()) if packed_bytes else bytes(CHUNK)
    assert len(raw) == CHUNK and hashlib.sha256(raw).digest() == digest
    assert raw == image[sector * 512:sector * 512 + CHUNK]
with tempfile.TemporaryDirectory() as temp:
    for bad in ['main', 'a'*39, '../bad', 'A'*40]:
        try:
            build(root / 'dist/LunaOS.img', temp, bad)
            raise AssertionError('accepted bad commit')
        except ValueError:
            pass
    try:
        build(root / 'dist/LunaOS.img', temp, 'f'*40)
        raise AssertionError('accepted image built from another commit')
    except ValueError:
        pass
print('PASS: update package identity, bounded chunks, checksums, BIOS/UEFI and excluded user-data sectors')
