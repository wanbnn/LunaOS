#!/usr/bin/env python3
"""Identify the compiled working tree, including uncommitted source changes."""
import hashlib
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parent.parent


def git(*args):
    return subprocess.check_output(['git', '-C', str(root), *args])


try:
    revision = git('rev-parse', '--short=12', 'HEAD').decode().strip()
    changes = git('diff', 'HEAD', '--binary', '--no-ext-diff')
    untracked = sorted(filter(None, git('ls-files', '--others', '--exclude-standard', '-z').split(b'\0')))
    if changes or untracked:
        digest = hashlib.sha256(changes)
        for name in untracked:
            path = root / name.decode()
            if path.is_file():
                data = path.read_bytes()
                digest.update(name + b'\0' + str(len(data)).encode() + b'\0' + data)
        revision += '-local-' + digest.hexdigest()[:10]
except (subprocess.CalledProcessError, FileNotFoundError):
    revision = 'unknown'

header = ('#ifndef LUNA_BUILD_VERSION_H\n#define LUNA_BUILD_VERSION_H\n'
          f'#define LUNA_BUILD_COMMIT "{revision}"\n#endif\n')
output = root / 'build/luna_build_version.h'
output.parent.mkdir(exist_ok=True)
if not output.exists() or output.read_text() != header:
    output.write_text(header)
