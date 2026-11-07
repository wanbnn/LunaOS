#!/usr/bin/env python3
"""Compile vendored Lucide SVGs to four-bit alpha masks. No SVG runtime in kernel."""

import hashlib
import io
import json
from pathlib import Path

import cairosvg
from PIL import Image

root = Path("assets/icons/lucide")
manifest = json.loads((root / "manifest.json").read_text())
# This is also the stable native Icon enum order.
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
sizes = [16, 20, 24, 26, 28, 29, 30, 32, 34, 36, 76]
for name, digest in manifest["files"].items():
    assert hashlib.sha256((root / name).read_bytes()).hexdigest() == digest, (
        f"Vendored asset changed: {name}"
    )
Path("build").mkdir(exist_ok=True)
with Path("build/icons.h").open("w") as f:
    f.write("/* Official Lucide Icons: assets/icons/lucide/LICENSE and manifest.json */\n")
    f.write("static const unsigned short icon_sizes[]={" + ",".join(map(str, sizes)) + "};\n")
    for size in sizes:
        data = bytearray()
        for name in names:
            svg = (root / (name + ".svg")).read_bytes()
            png = cairosvg.svg2png(bytestring=svg, output_width=size * 3, output_height=size * 3)
            im = (
                Image.open(io.BytesIO(png))
                .convert("RGBA")
                .getchannel("A")
                .resize((size, size), Image.Resampling.LANCZOS)
            )
            alpha = im.tobytes()
            packed = bytearray((len(alpha) + 1) // 2)
            for i, a in enumerate(alpha):
                packed[i // 2] |= (a >> 4) << (4 * (i % 2))
            data.extend(packed)
        f.write(
            f"static const unsigned char icon_data_{size}[]={{" + ",".join(map(str, data)) + "};\n"
        )
    f.write(
        "static const unsigned char *icon_atlases[]={"
        + ",".join(f"icon_data_{s}" for s in sizes)
        + "};\n"
    )
print(f"Compiled {len(names)} Lucide icons in {len(sizes)} native sizes")
