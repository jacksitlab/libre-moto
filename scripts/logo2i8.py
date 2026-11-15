#!/usr/bin/env python3
"""Convert the splash logo (docs/logo.svg) to an LVGL 9 I8-indexed C array.

The logo is mostly 3 colors (black->transparent, white, orange) plus
antialiasing edge tones — 914 unique RGBA values quantize to 256 with no
visible difference. I8 = 256-entry ARGB8888 palette + 1 byte per pixel:
360 KB ARGB8888 -> ~91 KB (75% flash saving for the biggest asset).

LVGL 9 indexed layout (lv_bin_decoder, LV_IMAGE_SRC_VARIABLE):
  data[0..palette_size*4] = palette as lv_color32_t (BGRA byte order:
  B,G,R,A in memory — lv_color32_t is little-endian ARGB)
  then w*h bytes of palette indices (row-major, stride = w)

Usage: python3 scripts/logo2i8.py
"""
import io, os, sys

import cairosvg
from PIL import Image

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SVG = os.path.join(REPO, "docs", "logo.svg")
OUT_C = os.path.join(REPO, "firmware", "src", "libre-moto", "libremoto_logo.c")
OUT_H = os.path.join(REPO, "firmware", "src", "libre-moto", "libremoto_logo.h")

W, H = 480, 188
PALETTE = 256


def main():
    png = cairosvg.svg2png(url=SVG, output_width=W, output_height=H)
    img = Image.open(io.BytesIO(png)).convert("RGBA")
    px = img.load()

    # quantize to <=256 colors: PIL's adaptive palette per full image
    quant = img.quantize(
        colors=PALETTE, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE)
    # keep alpha: quantize() loses it — blend palette entries carry their own
    # alpha via the RGBA->RGB trick is wrong; instead build the palette from
    # the quantized RGB + take alpha from the original per palette entry.
    qpx = quant.load()
    pal_rgb = quant.getpalette()  # flat r,g,b triplets

    # find alpha per palette index: sample the original pixels per index
    alpha_sum = [0] * PALETTE
    alpha_cnt = [0] * PALETTE
    for y in range(H):
        for x in range(W):
            i = qpx[x, y]
            alpha_sum[i] += px[x, y][3]
            alpha_cnt[i] += 1

    # emit: palette (BGRA per entry) + index bytes
    out = bytearray()
    for i in range(PALETTE):
        r = pal_rgb[i * 3] if i * 3 < len(pal_rgb) else 0
        g = pal_rgb[i * 3 + 1] if i * 3 + 1 < len(pal_rgb) else 0
        b = pal_rgb[i * 3 + 2] if i * 3 + 2 < len(pal_rgb) else 0
        a = (alpha_sum[i] // alpha_cnt[i]) if alpha_cnt[i] else 0
        # lv_color32_t in memory (little-endian ARGB): B,G,R,A
        out += bytes((b, g, r, a))
    for y in range(H):
        for x in range(W):
            out.append(qpx[x, y])

    with open(OUT_C, "w") as f:
        f.write(f"/* LibreMoto logo (wordmark) — GENERATED from docs/logo.svg\n")
        f.write(f" * by scripts/logo2i8.py. I8 indexed: {PALETTE}-entry palette + 1 B/px.\n")
        f.write(f" * {len(out)} B total (ARGB8888 would be {W*H*4} B). Do not edit. */\n")
        f.write('#include "libremoto_logo.h"\n\n')
        f.write("const uint8_t libremoto_logo_map[] = {\n")
        for i in range(0, len(out), 12):
            f.write("  " + ", ".join(f"0x{b:02x}" for b in out[i:i+12]) + ",\n")
        f.write("};\n")

    with open(OUT_H, "w") as f:
        f.write("/* LibreMoto logo (wordmark) — GENERATED from docs/logo.svg. */\n")
        f.write("#ifndef LIBREMOTO_LOGO_H\n#define LIBREMOTO_LOGO_H\n")
        f.write("#include <stdint.h>\n")
        f.write("#ifdef __cplusplus\nextern \"C\" {\n#endif\n")
        f.write("extern const uint8_t libremoto_logo_map[];\n")
        f.write("#ifdef __cplusplus\n}\n#endif\n#endif\n")

    print(f"logo: I8 {len(out)} B (was {W*H*4} B ARGB8888) — saved {100 - len(out)*100//(W*H*4)}%")


if __name__ == "__main__":
    main()
