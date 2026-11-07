#!/usr/bin/env python3
"""Rasterize the unmodified Inter Tight source into embedded grayscale atlases."""

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

sizes = [13, 16, 20, 28, 48]
chars = list(range(32, 127)) + list(range(160, 256))
out = Path("build/font.h")
out.parent.mkdir(exist_ok=True)
with out.open("w") as f:
    f.write(
        "/* Inter Tight, Copyright 2022 The Inter Project Authors. SIL OFL 1.1: assets/fonts/OFL.txt */\n"
    )
    f.write("typedef struct {unsigned int offset; unsigned short w,h,advance;} Glyph;\n")
    f.write("static const unsigned short font_sizes[]={13,16,20,28,48};\n")
    for idx, size in enumerate(sizes):
        font = ImageFont.truetype("assets/fonts/InterTight.ttf", size)
        font.set_variation_by_axes([400])
        data = bytearray()
        glyphs = []
        for code in chars:
            ch = chr(code)
            advance = round(font.getlength(ch))
            w = max(1, advance + 2)
            h = size + 7
            im = Image.new("L", (w, h))
            ImageDraw.Draw(im).text((0, 0), ch, font=font, fill=255)
            glyphs.append((len(data), w, h, advance))
            data.extend(im.tobytes())
        # 4-bit alpha halves storage while preserving 16 levels of antialiasing.
        packed = bytearray((len(data) + 1) // 2)
        for n, a in enumerate(data):
            packed[n // 2] |= (a >> 4) << (4 * (n % 2))
        f.write(
            f"static const unsigned char font_data_{idx}[]={{" + ",".join(map(str, packed)) + "};\n"
        )
        f.write(
            f"static const Glyph font_glyphs_{idx}[]={{"
            + ",".join("{%d,%d,%d,%d}" % g for g in glyphs)
            + "};\n"
        )
    f.write(
        "static const unsigned char *font_data[]={"
        + ",".join("font_data_" + str(i) for i in range(5))
        + "};\n"
    )
    f.write(
        "static const Glyph *font_glyphs[]={"
        + ",".join("font_glyphs_" + str(i) for i in range(5))
        + "};\n"
    )
