#!/usr/bin/env python3
# Embed an arbitrary file's raw bytes as a flash-resident C array. Shared by bake_png.py
# (which resizes first) and used directly when the source is already the right shape.
#
#   python3 tools/embed_bytes.py <infile> <out_header.h> NAME
import sys

def main():
    inp, outp, name = sys.argv[1], sys.argv[2], sys.argv[3]
    data = open(inp, "rb").read()
    with open(outp, "w") as f:
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write(f"static const uint32_t {name}_LEN = {len(data)};\n")
        f.write(f"static const uint8_t {name}[{len(data)}] = {{\n")
        for i in range(0, len(data), 20):
            f.write("  " + ",".join(f"0x{b:02X}" for b in data[i:i+20]) + ",\n")
        f.write("};\n")
    print(f"wrote {outp} ({len(data)} bytes)")

if __name__ == "__main__":
    main()
