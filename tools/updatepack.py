#!/usr/bin/env python3
"""Luna Update Engine v2: one bounded HTTPS stream per full or differential package."""
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
DELTA_MAGIC = b'LUNADLT2'
DELTA_BITMAP_BYTES = 32
EXCLUDED_START = 8 * 1024 * 1024
EXCLUDED_END = 12 * 1024 * 1024


def check_commit(commit):
    if not re.fullmatch('[0-9a-f]{40}', commit):
        raise ValueError('expected a full commit SHA')


def read_image(path, commit=None):
    raw = Path(path).read_bytes()
    if len(raw) != IMAGE_BYTES or raw[510:512] != b'\x55\xaa':
        raise ValueError('expected a 66 MiB LunaOS BIOS/UEFI image')
    if raw[16383 * 512:16383 * 512 + 8] != b'LUNAIMG1':
        raise ValueError('missing LunaOS image metadata')
    if commit is not None and commit.encode() not in raw[:4 * 1024 * 1024]:
        raise ValueError('image does not contain the requested compiled commit')
    return raw


def chunks(image):
    for offset in range(0, len(image), CHUNK):
        if EXCLUDED_START <= offset < EXCLUDED_END:
            continue
        yield offset, image[offset:offset + CHUNK]


def build(image, output, commit, legacy_assets=False):
    check_commit(commit)
    data = read_image(image, commit)
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    records, payload = [], []
    for offset, raw in chunks(data):
        packed = zlib.compress(raw, 9) if any(raw) else b''
        if packed:
            payload.append(packed)
            if legacy_assets:
                (output / f'chunk-{len(records):03d}.z').write_bytes(packed)
        records.append(RECORD.pack(offset // 512, len(packed), hashlib.sha256(raw).digest()))
    manifest = HEADER.pack(b'LUNAUPD1', 1, IMAGE_BYTES, CHUNK, len(records), commit.encode()) + b''.join(records)
    (output / 'update.manifest').write_bytes(manifest)  # local tests only; never publish separately
    (output / 'update.pack').write_bytes(manifest + b''.join(payload))
    return manifest


def build_delta(image, base_image, output, commit, base_commit):
    """Build one differential package targeted to an exact installed build."""
    check_commit(commit)
    check_commit(base_commit)
    target = read_image(image, commit)
    previous = read_image(base_image, base_commit)
    output = Path(output)
    manifest = (output / 'update.manifest').read_bytes()
    if manifest[24:64] != commit.encode():
        raise ValueError('build full package before building delta')
    bitmap = bytearray(DELTA_BITMAP_BYTES)
    payload = []
    changed = 0
    for i, ((offset, raw), (_, old)) in enumerate(zip(chunks(target), chunks(previous))):
        # Always include MBR: installed disk IDs and partition metadata are preserved separately.
        if i and raw == old:
            continue
        bitmap[i // 8] |= 1 << (i % 8)
        packed = zlib.compress(raw, 9) if any(raw) else b''
        expected = RECORD.unpack_from(manifest, HEADER.size + i * RECORD.size)[1]
        if len(packed) != expected:
            raise ValueError('inconsistent target manifest')
        payload.append(packed)
        changed += 1
    filename = 'update-from-' + base_commit + '.pack'
    (output / filename).write_bytes(
        DELTA_MAGIC + base_commit.encode() + bytes(bitmap) + manifest + b''.join(payload)
    )
    return filename, changed


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', default='dist/LunaOS.img')
    parser.add_argument('--output', default='dist/update')
    parser.add_argument('--commit', required=True)
    parser.add_argument('--base-image')
    parser.add_argument('--base-commit')
    parser.add_argument('--legacy-assets', action='store_true', help='publish compatibility assets for v1 clients')
    args = parser.parse_args()
    if bool(args.base_image) != bool(args.base_commit):
        parser.error('--base-image and --base-commit must be supplied together')
    build(args.image, args.output, args.commit, legacy_assets=args.legacy_assets)
    if args.base_image:
        build_delta(args.image, args.base_image, args.output, args.commit, args.base_commit)
