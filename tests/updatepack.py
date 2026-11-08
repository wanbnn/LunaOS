#!/usr/bin/env python3
"""End-to-end v2 full/differential package layout and corruption tests."""
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from updatepack import (build, build_delta, chunks, CHUNK, HEADER, RECORD,
                        IMAGE_BYTES, DELTA_MAGIC, DELTA_BITMAP_BYTES)

root = Path(__file__).resolve().parents[1]
commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
output = root / 'build/update-test'
manifest = build(root / 'dist/LunaOS.img', output, commit)
image = (root / 'dist/LunaOS.img').read_bytes()
magic, version, size, chunk, count, revision = HEADER.unpack_from(manifest)
assert (magic, version, size, chunk, count, revision.decode()) == (b'LUNAUPD1', 1, IMAGE_BYTES, CHUNK, 248, commit)
assert len(manifest) == HEADER.size + count * RECORD.size
package = (output / 'update.pack').read_bytes()
assert package.startswith(manifest)
position = len(manifest)
record_data = list(chunks(image))
for i in range(count):
    sector, packed_bytes, digest = RECORD.unpack_from(manifest, HEADER.size + i * RECORD.size)
    assert not 8192 <= sector < 16384
    packed = package[position:position + packed_bytes]
    position += packed_bytes
    raw = zlib.decompress(packed) if packed_bytes else bytes(CHUNK)
    assert len(raw) == CHUNK and hashlib.sha256(raw).digest() == digest
    assert raw == image[sector * 512:sector * 512 + CHUNK]
assert position == len(package)  # no trailing garbage

# A real exact-base delta: change the first boot chunk and one other chunk.
base_commit = '0' * 40
previous = bytearray(image)
previous[100000:100040] = base_commit.encode()
previous[9 * CHUNK + 13] ^= 0x17
base_path = output / 'base.img'
base_path.write_bytes(previous)
delta_name, changed = build_delta(root / 'dist/LunaOS.img', base_path,
                                  output, commit, base_commit)
assert changed >= 2
delta = (output / delta_name).read_bytes()
assert delta[:8] == DELTA_MAGIC
assert delta[8:48].decode() == base_commit
bitmap = delta[48:48 + DELTA_BITMAP_BYTES]
assert bitmap[0] & 1
assert bitmap[-1] == 0
offset = 48 + DELTA_BITMAP_BYTES
assert delta[offset:offset + len(manifest)] == manifest
offset += len(manifest)
for i, (_, target) in enumerate(record_data):
    selected = bool(bitmap[i // 8] & (1 << (i % 8)))
    sector = RECORD.unpack_from(manifest, HEADER.size + i * RECORD.size)[0]
    expected_selected = i == 0 or target != previous[sector * 512:sector * 512 + CHUNK]
    assert selected == expected_selected
    if selected:
        compressed_bytes = RECORD.unpack_from(manifest, HEADER.size + i * RECORD.size)[1]
        packed = delta[offset:offset + compressed_bytes]
        offset += compressed_bytes
        decoded = zlib.decompress(packed) if compressed_bytes else bytes(CHUNK)
        assert decoded == target
assert offset == len(delta)
assert len(delta) < len(package)
with tempfile.TemporaryDirectory() as temp:
    for bad in ['main', 'a' * 39, '../bad', 'A' * 40]:
        try:
            build(root / 'dist/LunaOS.img', temp, bad)
            raise AssertionError('accepted bad commit')
        except ValueError:
            pass
    try:
        build(root / 'dist/LunaOS.img', temp, 'f' * 40)
        raise AssertionError('accepted image built from another commit')
    except ValueError:
        pass
print('PASS: single full/differential packages, SHA-256, ordering, strict identity and excluded sectors')
