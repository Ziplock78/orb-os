#!/usr/bin/env python3
"""List what is inside a .orb theme file.

The bundle is what Orb Studio would send over the cable, packed instead of streamed, so it
answers "is this file actually in the theme?" without a device, a cable, or a two-minute
transfer. Written while chasing a background picture that was on the card and not in the
theme's own manifest, where knowing which of the two halves was wrong was the whole problem.

    python3 tools/read-orb-bundle.py "~/Downloads/Steam Punk.orb"
"""
import json
import os
import struct
import sys

MAGIC = b"ORBTHM01"


def unpack(path):
    with open(path, "rb") as f:
        d = f.read()
    if d[:8] != MAGIC:
        raise SystemExit(f"not an Orb theme file (magic is {d[:8]!r})")
    at = 8
    (slug_len,) = struct.unpack_from("<H", d, at); at += 2
    slug = d[at:at + slug_len].decode(); at += slug_len
    (count,) = struct.unpack_from("<H", d, at); at += 2
    files = []
    for _ in range(count):
        (n,) = struct.unpack_from("<H", d, at); at += 2
        name = d[at:at + n].decode(); at += n
        (size,) = struct.unpack_from("<I", d, at); at += 4
        files.append((name, d[at:at + size])); at += size
    return slug, files


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    path = os.path.expanduser(sys.argv[1])
    slug, files = unpack(path)
    print(f"\n  {os.path.basename(path)}")
    print(f"  slug: {slug}   files: {len(files)}   {os.path.getsize(path) / 1024:.0f} KB\n")

    for name, data in sorted(files, key=lambda f: -len(f[1]))[:60]:
        print(f"    {len(data) / 1024:8.1f} KB  {name}")

    names = {n for n, _ in files}
    manifest = next((d for n, d in files if n == "theme.json"), None)
    if manifest:
        j = json.loads(manifest.decode("utf-8", "replace"))
        assets = j.get("assets", [])
        print(f"\n  theme.json declares {len(assets)} asset(s)")
        # The disagreement that costs an evening: the file is in the bundle and the theme's
        # own list does not mention it, so the firmware refuses to open something it has.
        orphan = sorted(n for n in names
                        if (n.endswith(".png") or n.endswith(".bin")) and n not in assets)
        if orphan:
            print("  IN THE BUNDLE BUT NOT IN ITS OWN ASSET LIST:")
            for n in orphan:
                print(f"    {n}")
        missing = sorted(a for a in assets if a not in names)
        if missing:
            print("  LISTED AS AN ASSET BUT NOT IN THE BUNDLE:")
            for a in missing:
                print(f"    {a}")

    for want in ("weather_plate.png", "ticker_plate.png", "intel_plate.png"):
        print(f"  {want:<20} {'present' if want in names else 'ABSENT'}")
    print()


main()
