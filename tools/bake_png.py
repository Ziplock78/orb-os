#!/usr/bin/env python3
# Bake a PNG into a flash-resident byte array for runtime PNGdec decoding (see
# radar_png_line()-style callbacks in wx_radar_client.cpp) instead of a raw RGB565
# array like bake_dial.py produces. A raw 466x466 RGB565 dial is ~217KB of flash no
# matter what it shows; a PNG of mostly-flat art (like the Office splash) compresses
# to a few tens of KB, decoded once at boot/About-page time — cheap either way, but
# only one of these fits the app partition twice over.
#
#   python3 tools/bake_png.py <input.png> <out_header.h> NAME
import sys, os, subprocess, tempfile

SIZE = 466

def dims(path):
    out = subprocess.run(["sips", "-g", "pixelWidth", "-g", "pixelHeight", path],
                         capture_output=True, text=True).stdout
    w = h = 0
    for line in out.splitlines():
        if "pixelWidth" in line:  w = int(line.split(":")[1])
        if "pixelHeight" in line: h = int(line.split(":")[1])
    return w, h

def to_png(src):
    # Same center-crop-to-square-then-resize as bake_dial.py, so art lines up identically
    # whichever form (raw RGB565 vs PNG-decoded) a given asset ends up baked as.
    w, h = dims(src)
    side = min(w, h)
    fd, sq = tempfile.mkstemp(suffix=".png"); os.close(fd)
    subprocess.run(["sips", "-c", str(side), str(side), src, "--out", sq],
                   check=True, capture_output=True)
    fd, out = tempfile.mkstemp(suffix=".png"); os.close(fd)
    subprocess.run(["sips", "-z", str(SIZE), str(SIZE), "-s", "format", "png",
                    sq, "--out", out], check=True, capture_output=True)
    os.remove(sq)
    return out

def main():
    src, outp, name = sys.argv[1], sys.argv[2], sys.argv[3]
    png = to_png(src)
    try:
        data = open(png, "rb").read()
    finally:
        os.remove(png)
    with open(outp, "w") as f:
        f.write("#pragma once\n")
        f.write(f"// {name}: {SIZE}x{SIZE} PNG (from {os.path.basename(src)}), decoded at runtime via\n")
        f.write("// PNGdec (see ui_splash_show() / settings_view.cpp's About page) — not a raw\n")
        f.write("// RGB565 array, which would cost ~217KB of flash regardless of content.\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"static const uint32_t {name}_LEN = {len(data)};\n")
        f.write(f"static const uint8_t {name}[{len(data)}] = {{\n")
        for i in range(0, len(data), 20):
            f.write("  " + ",".join(f"0x{b:02X}" for b in data[i:i+20]) + ",\n")
        f.write("};\n")
    print(f"wrote {outp} ({len(data)} bytes, was {SIZE*SIZE*2} as raw RGB565)")

if __name__ == "__main__":
    main()
