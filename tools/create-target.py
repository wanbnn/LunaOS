#!/usr/bin/env python3
"""Create a new disposable raw IDE disk; refuses to overwrite existing paths."""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('path', type=Path)
parser.add_argument('mib', type=int, nargs='?', default=128)
args = parser.parse_args()
if not 66 <= args.mib <= 131072:
    parser.error('size must be 66..131072 MiB for the current installer')
try:
    with args.path.open('xb') as disk:
        disk.truncate(args.mib * 1024 * 1024)
except FileExistsError:
    parser.error(f'path already exists; preserved without changes: {args.path}')
print(f'Created {args.path}: {args.mib} MiB raw disk')
