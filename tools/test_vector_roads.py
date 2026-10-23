#!/usr/bin/env python3
"""Build portable decoder tests; optionally test real OpenFreeMap tiles.

python tools/test_vector_roads.py
python tools/test_vector_roads.py --live
python tools/test_vector_roads.py --input tile.pbf --tile 10 194 387 --output roads/v2/tile.bin
Use the exact endpoint template configured on the Orb as --source. SD-only mode
uses the same cache. Without --output the tool uses the device's hashed filename.
"""
import argparse
import gzip
import json
import os
from pathlib import Path
import shutil
import ssl
import subprocess
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / ".pio" / "vector-tests"
DEFAULT = "https://tiles.openfreemap.org/planet/latest/{z}/{x}/{y}.pbf"
LIMIT = 512 * 1024

def build():
    BUILD.mkdir(parents=True, exist_ok=True)
    exe = BUILD / ("vector-test.exe" if os.name == "nt" else "vector-test")
    if os.name == "nt":
        vcvars = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\2017\Professional\VC\Auxiliary\Build\vcvars64.bat")
        if not vcvars.exists():
            raise SystemExit("MSVC vcvars64.bat not found; compile tests/road_vector_test.cpp with a C++17 compiler.")
        batch = BUILD / "build.cmd"
        batch.write_text(f'@echo off\ncall "{vcvars}" >nul\ncl /nologo /EHsc /std:c++17 /W4 /I "{ROOT / "src"}" "{ROOT / "tests/road_vector_test.cpp"}" /Fe:"{exe}" /Fo:"{BUILD / "test.obj"}"\n', encoding="utf-8")
        subprocess.run(["cmd", "/c", str(batch)], check=True, cwd=ROOT)
    else:
        subprocess.run([shutil.which("c++") or "g++", "-std=c++17", "-Wall", "-Wextra", "-pedantic", "-I", str(ROOT / "src"), str(ROOT / "tests/road_vector_test.cpp"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    transport_exe = BUILD / ("transport-test.exe" if os.name == "nt" else "transport-test")
    if os.name == "nt":
        batch = BUILD / "build-transport.cmd"
        batch.write_text(f'@echo off\ncall "{vcvars}" >nul\ncl /nologo /EHsc /std:c++17 /W4 /I "{ROOT / "src"}" "{ROOT / "tests/road_cache_transport_test.cpp"}" /Fe:"{transport_exe}" /Fo:"{BUILD / "transport.obj"}"\n', encoding="utf-8")
        subprocess.run(["cmd", "/c", str(batch)], check=True, cwd=BUILD)
    else:
        subprocess.run([shutil.which("c++") or "g++", "-std=c++17", "-Wall", "-Wextra", "-I", str(ROOT / "src"), str(ROOT / "tests/road_cache_transport_test.cpp"), "-o", str(transport_exe)], check=True)
    subprocess.run([str(transport_exe)], check=True)
    memory_exe = BUILD / ("memory-test.exe" if os.name == "nt" else "memory-test")
    memory_source = ROOT / "tests/lv_memory_headroom_test.cpp"
    memory_includes = [ROOT / "tests/allocator_stubs", ROOT / "include"]
    if os.name == "nt":
        batch = BUILD / "build-memory.cmd"
        includes = " ".join(f'/I "{d}"' for d in memory_includes)
        batch.write_text(f'@echo off\ncall "{vcvars}" >nul\ncl /nologo /EHsc /std:c++17 /W4 {includes} "{memory_source}" /Fe:"{memory_exe}"\n', encoding="utf-8")
        subprocess.run(["cmd", "/c", str(batch)], check=True, cwd=BUILD)
    else:
        subprocess.run([shutil.which("c++") or "g++", "-std=c++17", "-I", str(memory_includes[0]), "-I", str(memory_includes[1]), str(memory_source), "-o", str(memory_exe)], check=True)
    subprocess.run([str(memory_exe)], check=True)
    # Compile the production SD reader/projection with rendering stubbed out.
    cache_exe = BUILD / ("cache-test.exe" if os.name == "nt" else "cache-test")
    lvgl = ROOT / ".pio/libdeps/esp32-s3-amoled-175/lvgl"
    sources = [ROOT / "tests/road_vector_cache_test.cpp", ROOT / "src/roads_sd.cpp", ROOT / "src/coastline.cpp"]
    if os.name == "nt":
        batch = BUILD / "build-cache.cmd"
        includes = " ".join(f'/I "{d}"' for d in [ROOT / "src", ROOT / "include", lvgl])
        files = " ".join(f'"{f}"' for f in sources)
        batch.write_text(f'@echo off\ncall "{vcvars}" >nul\ncl /nologo /EHsc /std:c++17 /D_USE_MATH_DEFINES /DLV_CONF_INCLUDE_SIMPLE {includes} {files} /Fe:"{cache_exe}"\n', encoding="utf-8")
        subprocess.run(["cmd", "/c", str(batch)], check=True, cwd=BUILD)
    else:
        subprocess.run([shutil.which("c++") or "g++", "-std=c++17", "-DLV_CONF_INCLUDE_SIMPLE", "-I", str(ROOT / "src"), "-I", str(ROOT / "include"), "-I", str(lvgl), *map(str, sources), "-o", str(cache_exe)], check=True)
    fixture = BUILD / "cache-fixture"
    (fixture / "roads/v2").mkdir(parents=True, exist_ok=True)
    subprocess.run([str(cache_exe), str(fixture)], check=True)
    return exe

def source_id(s):
    h = 2166136261
    for c in s.encode():
        h = ((h ^ c) * 16777619) & 0xffffffff
    return h

def decode(exe, data, tile, source, output=None):
    if data[:2] == b"\x1f\x8b":
        import io
        with gzip.GzipFile(fileobj=io.BytesIO(data)) as f:
            data = f.read(LIMIT + 1)
    if len(data) > LIMIT:
        raise ValueError("expanded tile exceeds device input budget")
    z, x, y = tile
    raw = BUILD / f"{z}_{x}_{y}.pbf"
    raw.write_bytes(data)
    target = Path(output) if output else BUILD / "sdcard" / "roads" / "v2" / f"{source_id(source):08x}_{z}_{x}_{y}.bin"
    target.parent.mkdir(parents=True, exist_ok=True)
    started = time.perf_counter()
    result = subprocess.run([str(exe), str(raw), str(target), str(z), str(x), str(y), source], text=True, capture_output=True, check=True)
    print(f"z{z}/{x}/{y}: rawBytes={len(data)} {result.stdout.strip()}")
    return {"tile": tile, "rawBytes": len(data), "cacheBytes": target.stat().st_size, "hostProcessMs": round((time.perf_counter()-started)*1000, 2)}

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--live", action="store_true")
    p.add_argument("--tls12", action="store_true", help="restrict live HTTPS probes to the device's TLS 1.2")
    p.add_argument("--input", type=Path)
    p.add_argument("--tile", nargs=3, type=int)
    p.add_argument("--output", type=Path)
    p.add_argument("--source", default=DEFAULT)
    args = p.parse_args()
    exe = build()
    if args.input:
        if not args.tile:
            p.error("--input requires --tile Z X Y")
        decode(exe, args.input.read_bytes(), args.tile, args.source, args.output)
    if args.live:
        measurements = []
        context = ssl.create_default_context()
        if args.tls12:
            context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_2
        # Provo, central London and a rural Utah tile, plus an adjacent Provo tile.
        for tile in [(10, 194, 387), (11, 388, 774), (10, 511, 340), (9, 96, 195), (10, 195, 387)]:
            url = args.source.format(z=tile[0], x=tile[1], y=tile[2])
            start = time.perf_counter()
            req = urllib.request.Request(url, headers={"User-Agent": "OrbOS-vector-road-prototype/1.0 (github.com/Ziplock78/orb-firmware)", "Accept-Encoding": "gzip, identity"})
            with urllib.request.urlopen(req, timeout=30, context=context) as response:
                data = response.read(LIMIT+1)
            if len(data)>LIMIT:
                raise ValueError("compressed tile exceeds device input budget")
            network = round((time.perf_counter()-start)*1000, 2)
            m = decode(exe, data, tile, args.source)
            m.update(wireBytes=len(data), hostNetworkMs=network)
            measurements.append(m)
            time.sleep(1)
        (BUILD / "live-results.json").write_text(json.dumps(measurements, indent=2)+"\n")
        print("Host measurements saved to .pio/vector-tests/live-results.json; these are not device timings.")

if __name__ == "__main__":
    main()
