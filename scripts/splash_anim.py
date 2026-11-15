#!/usr/bin/env python3
"""Splash animation assets — GENERATED from docs/logo.svg, website variant A
("assembly": letters stagger in, speed trail streaks, swooshes fly in,
the needle settles).

One A8 alpha mask per animated element, rendered at the splash size
(480x188, same placement as libremoto_logo.c). Each mask is cropped to
its own bbox and placed by (x, y) at runtime — the firmware blits the
masks with per-element color, offset and rotation instead of shipping
16 full frames (16 x 90 KB = 1.4 MB) or an LVGL animation.

Colors are flat: white (#ffffff) for the Libre letters + main/left
swooshes, brand orange (#fe6b0b) for the Moto letters + needle + mid
swoosh + speedline (same palette as the website CSS).

Usage: python3 scripts/splash_anim.py
"""
import io, os, re
import xml.etree.ElementTree as ET

import cairosvg
from PIL import Image

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SVG = os.path.join(REPO, "docs", "logo.svg")
OUT_C = os.path.join(REPO, "firmware", "src", "libre-moto", "splash_anim.c")
OUT_H = os.path.join(REPO, "firmware", "src", "libre-moto", "splash_anim.h")

W, H = 480, 188          # splash placement = libremoto_logo.c
SVG_W, SVG_H = 255, 100  # logo.svg viewBox

NS = {"svg": "http://www.w3.org/2000/svg"}

# element id -> (website class, color, animation, delay_s, duration_s)
# Timings mirror website variant A exactly (index.html, .stage.play.a):
#   letters   chIn .5s, stagger .06 .14 .22 .30 .38 | .52 .60 .68 .76
#   dot       dotIn .5s @ .5s   (fall + bounce)
#   speedline streakIn .45s @ .85s (from right)
#   needle    settle .75s @ 1.25s (fade in rotated -12deg -> 0)
#   swooshes  fromLeft .6s @ .85/.95/1.05s, fromRight .6s @ 1.15s
ELEMENTS = [
    ("path11", "l-l",    "ffffff", "chIn",     0.06, 0.50),
    ("path1",  "l-i",    "ffffff", "chIn",     0.14, 0.50),
    ("path4",  "l-b",    "ffffff", "chIn",     0.22, 0.50),
    ("path5",  "l-r",    "ffffff", "chIn",     0.30, 0.50),
    ("path6",  "l-e",    "ffffff", "chIn",     0.38, 0.50),
    ("path2",  "l-dot",  "ffffff", "dotIn",    0.50, 0.50),
    ("path7",  "m-m",    "fe6b0b", "chIn",     0.52, 0.50),
    ("path8",  "m-o1",   "fe6b0b", "chIn",     0.60, 0.50),
    ("path9",  "m-t",    "fe6b0b", "chIn",     0.68, 0.50),
    ("path8-3","m-o2",   "fe6b0b", "chIn",     0.76, 0.50),
    ("path15", "speed",  "fe6b0b", "streakIn", 0.85, 0.45),
    ("path13", "sw-left","ffffff", "fromLeft", 0.85, 0.60),
    ("path14", "sw-lf2", "fe6b0b", "fromLeft", 0.95, 0.60),
    ("path12", "sw-main","ffffff", "fromLeft", 1.05, 0.75),
    ("path3",  "sw-mid", "fe6b0b", "fromRight",1.15, 0.60),
    ("path10", "needle", "fe6b0b", "settle",   1.25, 0.75),
]

TOTAL_MS = 2400  # website intro ~2.2 s + settle tail


def render_one(tree, pid, color):
    """Render ONE path of logo.svg at splash size, cropped to its bbox."""
    # build a minimal svg containing only this path (keep viewBox)
    svgns = "http://www.w3.org/2000/svg"
    path = tree.find(f".//{{{svgns}}}path[@id='{pid}']")
    p_xml = ET.tostring(path, encoding="unicode")
    one = (f'<svg xmlns="http://www.w3.org/2000/svg" '
           f'viewBox="0 0 {SVG_W} {SVG_H}" width="{SVG_W}" height="{SVG_H}">'
           f'{p_xml}</svg>')
    png = cairosvg.svg2png(bytestring=one.encode(), output_width=W, output_height=H)
    img = Image.open(io.BytesIO(png)).convert("RGBA")
    # crop to content bbox (alpha > 0) with 1 px pad
    alpha = img.split()[3]
    bbox = alpha.getbbox()
    if bbox:
        bbox = (max(0, bbox[0]-1), max(0, bbox[1]-1),
                min(W, bbox[2]+1), min(H, bbox[3]+1))
    else:
        bbox = (0, 0, 1, 1)
    crop = img.crop(bbox)
    return crop, bbox


def main():
    tree = ET.parse(SVG)

    entries = []   # (name, color, anim, delay_ms, dur_ms, x, y, w, h, a8 bytes)
    for pid, name, color, anim, delay, dur in ELEMENTS:
        crop, bbox = render_one(tree, pid, color)
        a8 = crop.split()[3]  # alpha channel as the mask
        x, y = bbox[0], bbox[1]
        w, h = crop.size
        # quantize alpha to 16 steps (A4) — plenty for AA edges, half flash
        px = a8.load()
        for yy in range(h):
            for xx in range(w):
                px[xx, yy] = (px[xx, yy] // 16) * 16
        entries.append((name, color, anim, int(delay*1000), int(dur*1000),
                        x, y, w, h, list(a8.tobytes())))

    with open(OUT_C, "w") as f:
        f.write("/* LibreMoto splash animation frames — GENERATED from\n")
        f.write(" * docs/logo.svg by scripts/splash_anim.py (website variant A\n")
        f.write(" * 'assembly' timings). A8 masks cropped per element. Do not edit. */\n")
        f.write('#include "splash_anim.h"\n\n')
        for name, color, anim, delay, dur, x, y, w, h, data in entries:
            cname = name.replace("-", "_")
            f.write(f"static const uint8_t mask_{cname}[] = {{\n")
            for i in range(0, len(data), 16):
                f.write("  " + ", ".join(f"0x{b:02x}" for b in data[i:i+16]) + ",\n")
            f.write("};\n\n")
        f.write("/* element table: color RGB565, anim type, delay/duration ms,\n")
        f.write("   placement at splash size (480x188, origin top-left) */\n")
        f.write("const splash_elem_t splash_elems[] = {\n")
        for name, color, anim, delay, dur, x, y, w, h, data in entries:
            cname = name.replace("-", "_")
            r, g, b = int(color[0:2], 16), int(color[2:4], 16), int(color[4:6], 16)
            rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            anim_id = {"chIn": 0, "dotIn": 1, "streakIn": 2,
                       "fromLeft": 3, "fromRight": 4, "settle": 5}[anim]
            f.write(f"  {{ mask_{cname}, 0x{rgb565:04x}, {anim_id}, {delay}, {dur}, "
                    f"{x}, {y}, {w}, {h} }},\n")
        f.write("};\n")
        f.write(f"const int splash_elem_count = {len(entries)};\n")
        f.write(f"const int splash_anim_total_ms = {TOTAL_MS};\n")

    with open(OUT_H, "w") as f:
        f.write("/* LibreMoto splash animation — GENERATED from docs/logo.svg. */\n")
        f.write("#ifndef SPLASH_ANIM_H\n#define SPLASH_ANIM_H\n")
        f.write("#include <stdint.h>\n\n")
        f.write("typedef struct {\n")
        f.write("    const uint8_t *mask;   /* A8 alpha, w*h bytes */\n")
        f.write("    uint16_t rgb565;       /* flat element color */\n")
        f.write("    uint8_t  anim;         /* 0 chIn 1 dotIn 2 streakIn 3 fromLeft 4 fromRight 5 settle */\n")
        f.write("    uint16_t delay_ms, dur_ms;\n")
        f.write("    int16_t  x, y;         /* placement in the 480x188 splash area */\n")
        f.write("    uint16_t w, h;\n")
        f.write("} splash_elem_t;\n\n")
        f.write("extern const splash_elem_t splash_elems[];\n")
        f.write("extern const int splash_elem_count;\n")
        f.write("extern const int splash_anim_total_ms;\n")
        f.write("#endif\n")

    total = sum(len(d) for *_, d in entries)
    print(f"splash_anim: {len(entries)} elements, {total} B masks total")


if __name__ == "__main__":
    main()
