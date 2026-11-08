#!/usr/bin/env python3
"""Convert maneuver arrow SVGs to a single LVGL 9 C array for the firmware.

Usage:  python3 scripts/svg2maneuvers.py

Sources : docs/assets/maneuvers/<maneuver>.svg   (48x48 viewBox, white)
          docs/assets/vehicle.svg               (64x64 viewBox)
Output  : firmware/src/libre-moto/maneuver_icons.c/.h
          firmware/src/libre-moto/vehicle_icon.c/.h

Expected file names (= wire keywords of protocol.md §2):
  left.svg  right.svg  straight.svg  uturn.svg
  merge.svg slight_left.svg slight_right.svg

Each SVG is rendered at 48x48 into ARGB8888 (LVGL 9 lv_image_dsc_t,
byte order B,G,R,A). Unknown names are skipped with a warning; missing
names fall back to straight.svg at runtime (firmware logic).
"""
import io, os, sys

try:
    import cairosvg
except ImportError:
    sys.exit("pip install cairosvg")

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.join(REPO, "docs", "assets", "maneuvers")
OUT_C = os.path.join(REPO, "firmware", "src", "libre-moto", "maneuver_icons.c")
OUT_H = os.path.join(REPO, "firmware", "src", "libre-moto", "maneuver_icons.h")

SIZE = 48
MANEUVERS = ["left", "right", "straight", "uturn",
             "merge", "slight_left", "slight_right"]


def render_argb8888(svg_path):
    png = cairosvg.svg2png(url=svg_path, output_width=SIZE, output_height=SIZE)
    from PIL import Image
    img = Image.open(io.BytesIO(png)).convert("RGBA")
    if img.size != (SIZE, SIZE):
        img = img.resize((SIZE, SIZE))
    px = img.load()
    out = bytearray()
    for y in range(SIZE):
        for x in range(SIZE):
            r, g, b, a = px[x, y]
            out += bytes((b, g, r, a))   # LVGL XRGB8888 little-endian
    return bytes(out), img


def main():
    if not os.path.isdir(SRC_DIR):
        sys.exit(f"missing {SRC_DIR}")
    maps = {}
    for name in MANEUVERS:
        path = os.path.join(SRC_DIR, f"{name}.svg")
        if not os.path.isfile(path):
            print(f"WARN: {path} missing — firmware will fall back to straight")
            continue
        data, img = render_argb8888(path)
        maps[name] = data
        img.save(os.path.join(REPO, "docs", "assets", "maneuvers", f"{name}.preview.png"))
        print(f"{name}: rendered {len(data)} B")

    if not maps:
        sys.exit("no SVGs found — nothing to do")

    with open(OUT_C, "w") as f:
        f.write("/* Maneuver arrow icons — GENERATED from docs/assets/maneuvers/*.svg\n")
        f.write(" * by scripts/svg2maneuvers.py. Do not edit by hand. */\n")
        f.write('#include "maneuver_icons.h"\n\n')
        for name, data in maps.items():
            f.write(f"const uint8_t maneuver_{name}_map[] = {{\n")
            for i in range(0, len(data), 12):
                f.write("  " + ", ".join(f"0x{b:02x}" for b in data[i:i+12]) + ",\n")
            f.write("};\n\n")

    with open(OUT_H, "w") as f:
        f.write("/* Maneuver arrow icons — GENERATED from docs/assets/maneuvers/*.svg. */\n")
        f.write("#ifndef MANEUVER_ICONS_H\n#define MANEUVER_ICONS_H\n")
        f.write("#include <stdint.h>\n")
        f.write("#ifdef __cplusplus\nextern \"C\" {\n#endif\n")
        for name in maps:
            f.write(f"extern const uint8_t maneuver_{name}_map[];\n")
        f.write("#ifdef __cplusplus\n}\n#endif\n#endif\n")

    # ---- vehicle marker icon (64x64) --------------------------------
    veh_svg = os.path.join(REPO, "docs", "assets", "vehicle.svg")
    out_veh_c = os.path.join(REPO, "firmware", "src", "libre-moto", "vehicle_icon.c")
    out_veh_h = os.path.join(REPO, "firmware", "src", "libre-moto", "vehicle_icon.h")
    if os.path.isfile(veh_svg):
        global SIZE
        SIZE = 64
        data, img = render_argb8888(veh_svg)
        with open(out_veh_c, "w") as f:
            f.write("/* Vehicle marker icon — GENERATED from docs/assets/vehicle.svg\n")
            f.write(" * by scripts/svg2maneuvers.py. Do not edit by hand. */\n")
            f.write('#include "vehicle_icon.h"\n\n')
            f.write("const uint8_t vehicle_icon_map[] = {\n")
            for i in range(0, len(data), 12):
                f.write("  " + ", ".join(f"0x{b:02x}" for b in data[i:i+12]) + ",\n")
            f.write("};\n")
        with open(out_veh_h, "w") as f:
            f.write("/* Vehicle marker icon — GENERATED from docs/assets/vehicle.svg. */\n")
            f.write("#ifndef VEHICLE_ICON_H\n#define VEHICLE_ICON_H\n")
            f.write("#include <stdint.h>\n")
            f.write("#ifdef __cplusplus\nextern \"C\" {\n#endif\n")
            f.write("extern const uint8_t vehicle_icon_map[];\n")
            f.write("#ifdef __cplusplus\n}\n#endif\n#endif\n")
        img.save(os.path.join(REPO, "docs", "assets", "vehicle.preview.png"))
        print(f"vehicle: rendered {len(data)} B")
    else:
        print("WARN: docs/assets/vehicle.svg missing — firmware keeps the diamond")

    print(f"written: {OUT_C}\n         {OUT_H}")


if __name__ == "__main__":
    main()
