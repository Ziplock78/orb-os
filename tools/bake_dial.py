#!/usr/bin/env python3
# Bake a PNG dial into an RGB565 C header for the firmware (no Pillow needed).
# Uses macOS `sips` to resize to 466x466 and convert to an uncompressed BMP,
# then emits <name>[466*466] as uint16 RGB565 (native order, matches lv_color_t).
#
#   python3 tools/bake_dial.py <input.png> <out_header.h> DIAL_LOC
import sys, os, struct, subprocess, tempfile

SIZE = 466

def to_bmp(src):
    fd, bmp = tempfile.mkstemp(suffix=".bmp"); os.close(fd)
    subprocess.run(["sips", "-z", str(SIZE), str(SIZE), "-s", "format", "bmp",
                    src, "--out", bmp], check=True, capture_output=True)
    return bmp

def read_bmp(path):
    d = open(path, "rb").read()
    assert d[:2] == b"BM", "not a BMP"
    off = struct.unpack_from("<I", d, 10)[0]
    hdr = struct.unpack_from("<I", d, 14)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    topdown = h < 0
    h = abs(h)
    assert bpp in (24, 32), f"need 24/32-bit BMP, got {bpp}"
    px = bpp // 8
    rowsz = ((w * px + 3) // 4) * 4
    out = [[None] * w for _ in range(h)]
    for r in range(h):
        y = r if topdown else (h - 1 - r)
        base = off + r * rowsz
        for x in range(w):
            b = d[base + x * px]; g = d[base + x * px + 1]; rr = d[base + x * px + 2]
            out[y][x] = (rr, g, b)
    return w, h, out

def rgb565(rgb):
    r, g, b = rgb
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

def main():
    src, outp, name = sys.argv[1], sys.argv[2], sys.argv[3]
    bmp = to_bmp(src)
    try:
        w, h, px = read_bmp(bmp)
    finally:
        os.remove(bmp)
    assert (w, h) == (SIZE, SIZE), f"expected {SIZE}x{SIZE}, got {w}x{h}"
    vals = [rgb565(px[y][x]) for y in range(h) for x in range(w)]
    with open(outp, "w") as f:
        f.write("#pragma once\n")
        f.write(f"// {name}: baked dial {SIZE}x{SIZE} RGB565 (from {os.path.basename(src)}).\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"#define {name}_W {SIZE}\n#define {name}_H {SIZE}\n\n")
        f.write(f"static const uint16_t {name}[{SIZE*SIZE}] = {{\n")
        for i in range(0, len(vals), 16):
            f.write("  " + ",".join(f"0x{v:04X}" for v in vals[i:i+16]) + ",\n")
        f.write("};\n")
    print(f"wrote {outp} ({len(vals)} px)")

if __name__ == "__main__":
    main()
