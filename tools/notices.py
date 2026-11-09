#!/usr/bin/env python3
import json
from pathlib import Path

pairs = {"font_license": "assets/fonts/OFL.txt", "icon_license": "assets/icons/lucide/LICENSE"}
extra = {
    "NotoSans-OFL.txt": "assets/fonts/NotoSans-OFL.txt",
    "Montserrat-OFL.txt": "assets/fonts/Montserrat-OFL.txt",
    "NotoSansSymbols2-OFL.txt": "assets/fonts/NotoSansSymbols2-OFL.txt",
    "uACPI.txt": "third_party/uacpi/LICENSE",
    "stb.txt": "third_party/stb/LICENSE",
    "Realtek-rtw88-BSD.txt": "assets/licenses/Realtek-rtw88-BSD.txt",
    "Intel-WiFi-Firmware.txt": "assets/licenses/Intel-WiFi-Firmware.txt",
    "Realtek-Firmware.txt": "assets/licenses/Realtek-Firmware.txt",
    "MicroPython.txt": "third_party/micropython/LICENSE",
    "CPython.txt": "assets/licenses/CPython.txt",
    "wasm3.txt": "third_party/wasm3/LICENSE",
    "QuickJS.txt": "third_party/quickjs/LICENSE",
    "BearSSL.txt": "third_party/bearssl/LICENSE.txt",
    "Musl.txt": "third_party/musl/COPYRIGHT",
    "GCC-Runtime-Exception.txt": "assets/licenses/GCC-Runtime-Exception.txt",
    "GCC-GPL3.txt": "assets/licenses/GPL-3.txt",
}
for filename, path in {
    "Lexbor-Apache-2.0.txt": "third_party/lexbor/LICENSE",
    "litehtml-BSD.txt": "third_party/litehtml/LICENSE",
    "Gumbo-Apache-2.0.txt": "third_party/litehtml/src/gumbo/LICENSE",
}.items():
    if Path(path).is_file():
        extra[filename] = path

with Path("build/notices.h").open("w") as f:
    for name, path in pairs.items():
        f.write(
            f"static const char {name}[] = "
            + json.dumps(Path(path).read_text(), ensure_ascii=True)
            + ";\n"
        )
    records = []
    for filename, path in extra.items():
        data = Path(path).read_text()
        for part, offset in enumerate(range(0, len(data), 7000)):
            text = data[offset : offset + 7000]
            name = f"extra_notice_{len(records)}"
            f.write(f"static const char {name}[] = " + json.dumps(text, ensure_ascii=True) + ";\n")
            dest = filename if len(data) <= 7000 else filename + f".part{part + 1}"
            records.append((dest, name))
    f.write(
        "static const struct {const char *name,*text;} extra_notices[]={"
        + ",".join(
            "{" + json.dumps("/etc/licenses/" + dest) + "," + name + "}" for dest, name in records
        )
        + "};\n"
    )
