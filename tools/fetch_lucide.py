#!/usr/bin/env python3
"""Vendor official Lucide SVG sources at one recorded upstream commit."""

import hashlib
import json
import urllib.request
from pathlib import Path

root = Path("assets/icons/lucide")
root.mkdir(parents=True, exist_ok=True)
names = [
    "moon",
    "terminal",
    "folder",
    "globe",
    "sliders-horizontal",
    "wifi",
    "bluetooth",
    "volume-2",
    "sun",
    "ethernet-port",
    "file-text",
    "x",
    "minus",
    "maximize-2",
    "chevron-right",
    "chevron-up",
    "play",
    "house",
    "hard-drive",
    "save",
    "circle-exclamation-point",
    "arrow-left",
    "mouse-pointer-2",
    "search",
    "download",
    "settings",
    "memory-stick",
    "command",
]
req = urllib.request.Request(
    "https://api.github.com/repos/lucide-icons/lucide/commits/main",
    headers={"User-Agent": "LunaOS asset vendor"},
)
revision = json.load(urllib.request.urlopen(req))["sha"]
base = f"https://raw.githubusercontent.com/lucide-icons/lucide/{revision}"
files = {}
for name in ["LICENSE"] + [f"icons/{n}.svg" for n in names]:
    data = urllib.request.urlopen(base + "/" + name).read()
    dest = root / Path(name).name
    dest.write_bytes(data)
    files[dest.name] = hashlib.sha256(data).hexdigest()
manifest = {
    "upstream": "https://github.com/lucide-icons/lucide",
    "revision": revision,
    "license": "ISC; Feather-derived icons MIT (see LICENSE)",
    "files": files,
}
(root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(f"Vendored {len(names)} official Lucide icons at {revision}")
