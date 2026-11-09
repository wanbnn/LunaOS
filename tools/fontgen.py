#!/usr/bin/env python3
"""Rasterize the source fonts into embedded grayscale atlases.

Three text families are offered to the user and a symbol family supplies the
glyphs the text families do not carry. Every family uses the same codepoint
table so a missing glyph can be replaced by the symbol family at run time.
"""

import json
import struct
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

SIZES = [13, 16, 20, 28, 48]

# Text families the user can pick as the default.
FAMILIES = [
    ("Inter Tight", "assets/fonts/InterTight.ttf"),
    ("Noto Sans", "assets/fonts/NotoSans.ttf"),
    ("Montserrat", "assets/fonts/Montserrat.ttf"),
]
# The symbol family is never selected as text; it only fills the gaps.
SYMBOL_FAMILY = ("Noto Sans Symbols 2", "assets/fonts/NotoSansSymbols2.ttf")

# Characters every text family is expected to carry.
TEXT_RANGE = [(32, 126), (160, 255), (0x100, 0x17F)]
# Symbols and punctuation that pages rely on and the text families may lack.
SYMBOLS = [
    0x2010, 0x2011, 0x2012, 0x2013, 0x2014, 0x2015, 0x2018, 0x2019, 0x201A,
    0x201B, 0x201C, 0x201D, 0x201E, 0x201F, 0x2020, 0x2021, 0x2022, 0x2026,
    0x2030, 0x2039, 0x203A, 0x2044, 0x20AC, 0x2122, 0x2190, 0x2191, 0x2192,
    0x2193, 0x2194, 0x2195, 0x21D2, 0x21D4, 0x2202, 0x2205, 0x2208, 0x2212,
    0x2215, 0x2219, 0x221A, 0x221E, 0x222B, 0x2248, 0x2260, 0x2264, 0x2265,
    0x25A0, 0x25AA, 0x25B2, 0x25B4, 0x25B6, 0x25B8, 0x25BC, 0x25BD, 0x25BE,
    0x25C0, 0x25C6, 0x25CB, 0x25CF, 0x25D0, 0x25E6, 0x2605, 0x2606, 0x2610,
    0x2611, 0x263A, 0x2640, 0x2642, 0x2660, 0x2663, 0x2665, 0x2666, 0x266A,
    0x266B, 0x2713, 0x2714, 0x2717, 0x2718, 0x2726, 0x2727, 0x274F, 0x2753,
    0x2764, 0x25CF, 0x2219, 0x00B7, 0x00D7, 0x2022, 0x2709, 0x2611,
    0x2612, 0x25B3, 0x25B5, 0x25B7, 0x25C8, 0x25D8, 0x25D9, 0x25A1,
    0x25EF, 0x2716, 0x2731, 0x2B1B, 0x2B1C, 0x00B6,
    0x2032, 0x2033, 0x21A9, 0x21B5, 0x2299, 0x2317, 0x2318, 0x2630, 0x26A0,
]


def codepoints():
    chars = set()
    for lo, hi in TEXT_RANGE:
        chars.update(range(lo, hi + 1))
    chars.update(SYMBOLS)
    return sorted(chars)


def load(path, size):
    font = ImageFont.truetype(path, size)
    try:
        axes = font.get_variation_axes()
    except (AttributeError, OSError):
        return font
    values = [axis["default"] for axis in axes]
    for index, axis in enumerate(axes):
        if axis["name"] == b"Weight":
            values[index] = 400
    try:
        font.set_variation_by_axes(values)
    except OSError:
        pass
    return font


def cmap_codepoints(path):
    """The codepoints a font carries, read from its own character map.

    Comparing renderings would depend on the size and could mistake a blank
    space for a missing glyph, which would shift every glyph of the atlas.
    """
    data = Path(path).read_bytes()
    tables = {}
    for i in range(struct.unpack(">H", data[4:6])[0]):
        entry = 12 + i * 16
        tag = data[entry : entry + 4].decode("latin-1")
        tables[tag] = struct.unpack(">II", data[entry + 8 : entry + 16])
    if "cmap" not in tables:
        return set()
    base = tables["cmap"][0]
    chosen = None
    for i in range(struct.unpack(">H", data[base + 2 : base + 4])[0]):
        record = base + 4 + i * 8
        platform, encoding, offset = struct.unpack(">HHI", data[record : record + 8])
        sub = base + offset
        fmt = struct.unpack(">H", data[sub : sub + 2])[0]
        if fmt not in (4, 12):
            continue
        if fmt == 12:
            chosen = sub
            break
        if chosen is None:
            chosen = sub
    if chosen is None:
        return set()
    codes = set()
    fmt = struct.unpack(">H", data[chosen : chosen + 2])[0]
    if fmt == 4:
        segs = struct.unpack(">H", data[chosen + 6 : chosen + 8])[0] // 2
        ends = struct.unpack(">%dH" % segs, data[chosen + 14 : chosen + 14 + segs * 2])
        starts = struct.unpack(
            ">%dH" % segs, data[chosen + 16 + segs * 2 : chosen + 16 + segs * 4]
        )
        for start, end in zip(starts, ends):
            if start == 0xFFFF:
                continue
            codes.update(range(start, end + 1))
    else:
        groups = struct.unpack(">I", data[chosen + 12 : chosen + 16])[0]
        for i in range(groups):
            record = chosen + 16 + i * 12
            start, end = struct.unpack(">II", data[record : record + 8])
            codes.update(range(start, end + 1))
    return codes



def atlas(path, size, chars, allowed=None, covered=None):
    """Rasterizes the characters the family carries and returns the atlas."""
    font = load(path, size)
    if covered is None:
        covered = cmap_codepoints(path)
    data = bytearray()
    glyphs = []
    slots = []
    for code in chars:
        if allowed is not None and code not in allowed:
            slots.append(0xFFFF)
            continue
        if code not in covered:
            slots.append(0xFFFF)
            continue
        ch = chr(code)
        advance = round(font.getlength(ch))
        w = max(1, advance + 2)
        h = size + 7
        image = Image.new("L", (w, h))
        ImageDraw.Draw(image).text((0, 0), ch, font=font, fill=255)
        slots.append(len(glyphs))
        glyphs.append((len(data), w, h, advance))
        data.extend(image.tobytes())
    # 4-bit alpha halves storage while preserving 16 levels of antialiasing.
    packed = bytearray((len(data) + 1) // 2)
    for n, alpha in enumerate(data):
        packed[n // 2] |= (alpha >> 4) << (4 * (n % 2))
    return bytes(packed), glyphs, slots


def main():
    chars = codepoints()
    out = Path("build/font.h")
    out.parent.mkdir(exist_ok=True)
    families = FAMILIES + [SYMBOL_FAMILY]
    names = []
    with out.open("w") as f:
        f.write(
            "/* Inter Tight, Montserrat and Noto Sans, Copyright their authors."
            " SIL OFL 1.1: assets/fonts/OFL.txt */\n"
            "/* Noto Sans Symbols 2, Copyright Google. SIL OFL 1.1:"
            " assets/fonts/NotoSansSymbols2-OFL.txt */\n"
        )
        f.write(
            "typedef struct {unsigned int offset; unsigned short w,h,advance;} Glyph;\n"
            "static const unsigned short font_sizes[]={%s};\n"
            % ",".join(str(s) for s in SIZES)
        )
        f.write("/* Codepoints shared by every family; a family that lacks one\n"
                " * points to the symbol family instead. */\n")
        f.write("static const unsigned int font_codepoints[]={%s};\n"
                % ",".join(str(c) for c in chars))
        f.write("#define FONT_CODEPOINTS %d\n" % len(chars))
        for family, (label, path) in enumerate(families):
            # The symbol family only fills the gaps; it never repeats the text.
            allowed = None if family < len(FAMILIES) else set(SYMBOLS)
            coverage = cmap_codepoints(path)
            for index, size in enumerate(SIZES):
                packed, glyphs, slots = atlas(path, size, chars, allowed, coverage)
                f.write("static const unsigned char font_data_%d_%d[]={%s};\n"
                        % (family, index, ",".join(map(str, packed))))
                f.write("static const Glyph font_glyphs_%d_%d[]={%s};\n"
                        % (family, index, ",".join("{%d,%d,%d,%d}" % g for g in glyphs)))
                if index == 0:
                    names.append(family)
                    f.write("/* %s */\n" % label)
                    f.write("static const unsigned short font_slots_%d[]={%s};\n"
                            % (family, ",".join(str(s) for s in slots)))
        f.write("static const unsigned char *font_data[%d][%d]={"
                % (len(families), len(SIZES)))
        for family in range(len(families)):
            f.write("{" + ",".join("font_data_%d_%d" % (family, i) for i in range(len(SIZES))) + "},")
        f.write("};\n")
        f.write("static const Glyph *font_glyphs[%d][%d]={"
                % (len(families), len(SIZES)))
        for family in range(len(families)):
            f.write("{" + ",".join("font_glyphs_%d_%d" % (family, i) for i in range(len(SIZES))) + "},")
        f.write("};\n")
        f.write("static const unsigned short *font_slots[%d]={%s};\n"
                % (len(families), ",".join("font_slots_%d" % f for f in range(len(families)))))
        f.write("static const char *font_family_names[%d]={%s};\n"
                % (len(families), ",".join(json.dumps(label) for label, _ in families)))
        f.write("#define FONT_FAMILY_COUNT %d\n" % len(families))
        f.write("#define FONT_TEXT_FAMILIES %d\n" % len(FAMILIES))
        f.write("#define FONT_SYMBOL_FAMILY %d\n" % len(FAMILIES))
    print("font.h: %d codepoints, %d families" % (len(chars), len(families)))


if __name__ == "__main__":
    main()
