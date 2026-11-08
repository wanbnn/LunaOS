#!/usr/bin/env python3
"""Publishable, bounded update chunks. Never include the live LunaFS slots."""
import argparse
import hashlib
from pathlib import Path
import re
import struct
import zlib

CHUNK = 256 * 1024
IMAGE_BYTES = 66 * 1024 * 1024
HEADER = struct.Struct('<8sIIII40s')
RECORD = struct.Struct('<II32s')


def build(image, output, commit):
    if not re.fullmatch('[0-9a-f]{40}', commit):
        raise ValueError('expected a full commit SHA')
    data = Path(image).read_bytes()
    if len(data) != IMAGE_BYTES or data[510:512] != b'\x55\xaa':
        raise ValueError('expected a 66 MiB LunaOS BIOS/UEFI image')
    if data[7168 * 512:7168 * 512 + 8] != b'LUNAIMG1':
        raise ValueError('missing LunaOS image metadata')
    if commit.encode() not in data[:4 * 1024 * 1024]:
        raise ValueError('image does not contain the requested compiled commit')
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    records = []
    for offset in range(0, len(data), CHUNK):
        if 4 * 1024 * 1024 <= offset < 8 * 1024 * 1024:
            continue
        raw = data[offset:offset + CHUNK]
        packed = zlib.compress(raw, 9) if any(raw) else b''
        index = len(records)
        if packed:
            (output / f'chunk-{index:03d}.z').write_bytes(packed)
        records.append(RECORD.pack(offset // 512, len(packed), hashlib.sha256(raw).digest()))
    manifest = HEADER.pack(b'LUNAUPD1', 1, IMAGE_BYTES, CHUNK, len(records), commit.encode()) + b''.join(records)
    (output / 'update.manifest').write_bytes(manifest)
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', default='dist/LunaOS.img')
    parser.add_argument('--output', default='dist/update')
    parser.add_argument('--commit', required=True)
    args = parser.parse_args()
    build(args.image, args.output, args.commit)
