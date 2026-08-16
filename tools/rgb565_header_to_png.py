#!/usr/bin/env python3
# Re-encode an existing baked RGB565 C header (bake_dial.py's output format) back into a
# PNG, with zero dependencies beyond the stdlib (zlib for the PNG's own deflate stream).
# Exists so an already-baked dial/splash can move to the PNG+PNGdec-at-runtime path (see
# bake_png.py) without needing to track down its original source image.
#
#   python3 tools/rgb565_header_to_png.py <in_header.h> NAME <out.png>
import sys, re, struct, zlib

def main():
    inp, name, outp = sys.argv[1], sys.argv[2], sys.argv[3]
    src = open(inp).read()
    m = re.search(rf"{name}_W\s+(\d+)", src)
    w = int(m.group(1))
    m = re.search(rf"{name}_H\s+(\d+)", src)
    h = int(m.group(1))
    m = re.search(rf"{name}\[\d+\]\s*=\s*\{{(.*?)\}};", src, re.S)
    vals = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{4})", m.group(1))]
    assert len(vals) == w * h, f"expected {w*h} px, found {len(vals)}"

    def rgb565_to_rgb888(v):
        r = (v >> 11) & 0x1F; g = (v >> 5) & 0x3F; b = v & 0x1F
        return (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)

    raw = bytearray()
    idx = 0
    for _y in range(h):
        raw.append(0)   # PNG filter type 0 (none) for this scanline
        for _x in range(w):
            r, g, b = rgb565_to_rgb888(vals[idx]); idx += 1
            raw += bytes((r, g, b))

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)   # 8-bit depth, color type 2 = truecolor
    idat = zlib.compress(bytes(raw), 9)
    png = sig + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")
    open(outp, "wb").write(png)
    print(f"wrote {outp} ({len(png)} bytes)")

if __name__ == "__main__":
    main()
