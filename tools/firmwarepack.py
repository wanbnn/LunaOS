#!/usr/bin/env python3
"""Build Luna's bounded binary firmware archive; preserves embedded zero bytes."""
import argparse
from pathlib import Path
import re
import struct

SECTOR = 16384
LIMIT = 2 * 1024 * 1024
MAX_FILE = 2 * 1024 * 1024
MAX_ENTRIES = 16


def fnv(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def build_archive(root):
    root = Path(root)
    entries = sorted(p for p in root.rglob('*') if p.suffix in ('.bin', '.ucode', '.fw'))
    if len(entries) > MAX_ENTRIES:
        raise ValueError('firmware archive supports at most 16 files')
    directory_size = (len(entries) * 128 + 511) // 512 * 512
    payload = bytearray()
    directory = bytearray()
    for path in entries:
        if path.is_symlink() or not path.is_file():
            raise ValueError(f'expected a regular firmware file: {path}')
        name = path.relative_to(root).as_posix()
        if len(name.encode()) >= 96 or not re.fullmatch(r'[A-Za-z0-9_./-]+', name):
            raise ValueError(f'invalid firmware name: {name}')
        if any(part in ('', '.', '..') for part in name.split('/')):
            raise ValueError(f'invalid firmware path: {name}')
        data = path.read_bytes()
        if not 0 < len(data) <= MAX_FILE:
            raise ValueError(f'firmware must contain 1..{MAX_FILE} bytes: {name}')
        offset = 512 + directory_size + len(payload)
        directory += struct.pack('<96sIII20x', name.encode(), offset, len(data), fnv(data))
        payload += data
        payload += bytes((-len(payload)) % 512)
    total = 512 + directory_size + len(payload)
    if total > LIMIT:
        raise ValueError('firmware archive exceeds reserved disk region')
    header = struct.pack('<8sIIII', b'LUNAFW1\0', 1, len(entries), total, fnv(directory))
    return header.ljust(512, b'\0') + directory.ljust(directory_size, b'\0') + payload


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.write_bytes(build_archive(args.directory))
