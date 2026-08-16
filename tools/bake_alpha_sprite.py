#!/usr/bin/env python3
# Crop+resize an alpha PNG for use as a rotating LVGL sprite, saved as a compressed PNG
# (embed with embed_bytes.py) plus a small metadata header (W/H/pivot). Decoded once at
# boot into a PSRAM LV_IMG_CF_TRUE_COLOR_ALPHA buffer (see splash_art.cpp's pattern) —
# NOT baked as a raw RGB565+alpha array directly, which for this sprite's size would be
# ~380KB of flash for one asset. PNG-in-flash + decode-to-PSRAM-once is the same fix
# that got the boot splash under budget; same reasoning applies here. Needs Pillow
# (tools/hands/venv has it) — sips can't do precise numeric alpha-bbox cropping.
#
#   tools/hands/venv/bin/python3 tools/bake_alpha_sprite.py <input.png> <out_dir> NAME \
#       --scale 0.455078125 --dotx 523.8 --doty 511.2 --bbox 130 132 897 909 \
#       --baseline-deg 62.35
#
# Writes <out_dir>/<name_lower>.png (crop, feed to embed_bytes.py separately) and
# <out_dir>/<name_lower>_meta.h (W/H/PIVOT_X/PIVOT_Y/BASELINE_DEG_X10 defines).
import argparse
import os
from PIL import Image

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src"); ap.add_argument("out_dir"); ap.add_argument("name")
    ap.add_argument("--scale", type=float, required=True)
    ap.add_argument("--dotx", type=float, required=True)
    ap.add_argument("--doty", type=float, required=True)
    ap.add_argument("--bbox", type=float, nargs=4, required=True, metavar=("X0", "Y0", "X1", "Y1"))
    ap.add_argument("--baseline-deg", type=float, required=True,
                    help="Clock-angle (0=12 o'clock, clockwise) the hand points at in the source art")
    ap.add_argument("--pad", type=int, default=3)
    args = ap.parse_args()

    img = Image.open(args.src).convert("RGBA")
    sw, sh = round(img.width * args.scale), round(img.height * args.scale)
    img = img.resize((sw, sh), Image.LANCZOS)

    x0, y0, x1, y1 = [v * args.scale for v in args.bbox]
    x0 = max(0, int(x0) - args.pad); y0 = max(0, int(y0) - args.pad)
    x1 = min(sw, int(x1) + args.pad); y1 = min(sh, int(y1) + args.pad)
    crop = img.crop((x0, y0, x1, y1))
    w, h = crop.size

    pivot_x = round(args.dotx * args.scale) - x0
    pivot_y = round(args.doty * args.scale) - y0

    png_path = os.path.join(args.out_dir, f"{args.name.lower()}.png")
    crop.save(png_path)

    meta_path = os.path.join(args.out_dir, f"{args.name.lower()}_meta.h")
    with open(meta_path, "w") as f:
        f.write("#pragma once\n")
        f.write(f"// {args.name}: metadata for the baked sprite in {args.name.lower()}.png\n")
        f.write(f"// (crop of {os.path.basename(args.src)}). Decoded at runtime — see splash_art.cpp's pattern.\n")
        f.write(f"#define {args.name}_W {w}\n#define {args.name}_H {h}\n")
        f.write(f"#define {args.name}_PIVOT_X {pivot_x}\n#define {args.name}_PIVOT_Y {pivot_y}\n")
        f.write(f"#define {args.name}_BASELINE_DEG_X10 {round(args.baseline_deg * 10)}   // lv_img_set_angle() units\n")
    print(f"wrote {png_path} ({w}x{h}) and {meta_path}, pivot ({pivot_x},{pivot_y})")

if __name__ == "__main__":
    main()
