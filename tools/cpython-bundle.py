#!/usr/bin/env python3
"""Package the pinned wasm32-wasi CPython distribution for a LunaOS image."""
import hashlib
import io
import importlib.util
import marshal
import struct
import sys
import urllib.request
import zipfile
import zlib
from pathlib import Path

VERSION = "3.13.9"
URL = ("https://github.com/brettcannon/cpython-wasi-build/releases/download/"
       f"v{VERSION}/python-{VERSION}-wasi_sdk-24.zip")
SHA256 = "f974d681668b8a51bf548474c9999630fe83a316155a4e937eaff57ea3ea4f39"
# Imports needed at startup and by common shell scripts. Precompile these
# modules to avoid parsing the standard library under a software-emulated CPU.
PRECOMPILE = {
    "_collections_abc", "_sitebuiltins", "abc", "codecs", "collections",
    "contextlib", "copyreg", "encodings", "enum", "fnmatch", "functools",
    "genericpath", "io", "json", "keyword", "ntpath", "operator", "os",
    "pathlib", "posixpath", "re", "reprlib", "site", "stat", "types", "urllib",
    "warnings",
}


def strip_custom_sections(wasm):
    """Discard debug and name sections while preserving executable sections."""
    result = bytearray(wasm[:8])
    at = 8
    while at < len(wasm):
        start = at
        kind = wasm[at]
        at += 1
        size = shift = 0
        while True:
            byte = wasm[at]
            at += 1
            size |= (byte & 127) << shift
            if byte < 128:
                break
            shift += 7
        at += size
        if kind:
            result += wasm[start:at]
    return result


def main():
    if sys.implementation.name != "cpython" or sys.version_info[:3] != (3, 13, 9):
        raise SystemExit(f"Regenerate the bundle with CPython {VERSION} for matching bytecode")
    output, archive = map(Path, sys.argv[1:])
    archive.parent.mkdir(parents=True, exist_ok=True)
    if archive.exists():
        data = archive.read_bytes()
    else:
        with urllib.request.urlopen(URL, timeout=60) as response:
            data = response.read(32 * 1024 * 1024)
        if hashlib.sha256(data).hexdigest() == SHA256:
            archive.write_bytes(data)
    if hashlib.sha256(data).hexdigest() != SHA256:
        raise SystemExit("CPython WASI archive SHA-256 mismatch")

    stdlib = io.BytesIO()
    with zipfile.ZipFile(io.BytesIO(data)) as source:
        wasm = strip_custom_sections(source.read("python.wasm"))
        with zipfile.ZipFile(stdlib, "w") as dest:
            prefix = "lib/python3.13/"
            for name in sorted(source.namelist()):
                if name.startswith(prefix) and name.endswith(".py"):
                    relative = name[len(prefix):]
                    data = source.read(name)
                    if relative.split("/")[0].removesuffix(".py") in PRECOMPILE:
                        code = compile(data, f"/lib/python3.13/{relative}", "exec", dont_inherit=True)
                        bytecode = importlib.util.MAGIC_NUMBER + bytes(12) + marshal.dumps(code)
                        dest.writestr(zipfile.ZipInfo(relative + "c"), bytecode)
                    else:
                        dest.writestr(zipfile.ZipInfo(relative), data)

    packed = zlib.compress(wasm, 9)
    # The WASI build lacks zlib. Use a stored ZIP for zipimport and
    # decompress the outer payload with the LunaOS decoder.
    library = stdlib.getvalue()
    compressed = zlib.compress(library, 9)
    header = struct.pack("<8sIIII", b"LUNAPY3\0", len(packed), len(wasm),
                         len(compressed), len(library))
    output.write_bytes(header.ljust(512, b"\0") + packed + compressed)


if __name__ == "__main__":
    main()
