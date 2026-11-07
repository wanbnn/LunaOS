#!/usr/bin/env python3
import json
from pathlib import Path

pairs = {"font_license": "assets/fonts/OFL.txt", "icon_license": "assets/icons/lucide/LICENSE"}
extra = {
    "QuickJS.txt": "third_party/quickjs/LICENSE",
    "BearSSL.txt": "third_party/bearssl/LICENSE.txt",
    "Musl.txt": "third_party/musl/COPYRIGHT",
    "GCC-Runtime-Exception.txt": "assets/licenses/GCC-Runtime-Exception.txt",
    "GCC-GPL3.txt": "assets/licenses/GPL-3.txt",
}
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
