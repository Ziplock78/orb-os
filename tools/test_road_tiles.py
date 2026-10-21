#!/usr/bin/env python3
"""Build/run portable road-generator tests; --live also tests one real Overpass cell.

Requires PlatformIO's ArduinoJson dependency plus a host C++17 compiler (CXX or
g++, or Visual Studio's C++ tools on Windows). Does not flash a device.
"""
import argparse
import os
from pathlib import Path
import shutil
import ssl
import re
import json
import time
import urllib.error
import subprocess
import urllib.request
from collections import Counter


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--live", action="store_true")
    parser.add_argument("--url", help="Override the firmware's ROAD_OVERPASS_URL for the live test")
    parser.add_argument("--tls12", action="store_true", help="Restrict live HTTPS to the firmware SDK's TLS 1.2")
    parser.add_argument("--insecure", action="store_true", help="Match the firmware's existing setInsecure certificate policy")
    parser.add_argument("--tile", type=int, nargs=2, default=(37, -113), metavar=("LAT", "LON"))
    parser.add_argument("--quarter-cell", type=int, nargs=2, metavar=("LAT_INDEX", "LON_INDEX"),
                        help="Test the new quarter-degree cell (indices are floor(coordinate * 4))")
    parser.add_argument("--road-class", choices=("motorway", "trunk"))
    parser.add_argument("--server-memory-mb", type=int, default=128)
    parser.add_argument("--compact-output", action="store_true", help="Benchmark skel geom noids qt and compare geometry with a saved standard response")
    parser.add_argument("--fixture", type=Path, action="append", default=[], help="Check a saved response without making a network request")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    os.chdir(root)
    build = root / ".pio" / "road-tests"
    build.mkdir(parents=True, exist_ok=True)
    include = root / ".pio/libdeps/esp32-s3-amoled-175/ArduinoJson/src"
    source = root / "tests/road_tile_builder_test.cpp"
    exe = build / ("test.exe" if os.name == "nt" else "test")
    cache_exe = build / ("cache-test.exe" if os.name == "nt" else "cache-test")
    lvgl = root / ".pio/libdeps/esp32-s3-amoled-175/lvgl"
    cache_sources = [root / p for p in ("tests/road_cache_test.cpp", "src/roads_sd.cpp", "src/coastline.cpp")]
    compiler = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
    if compiler:
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-I", str(root / "src"),
                        "-I", str(include), str(source), "-o", str(exe)], check=True)
        subprocess.run([compiler, "-std=c++17", "-D_USE_MATH_DEFINES", "-DLV_CONF_INCLUDE_SIMPLE",
                        "-I", str(root / "src"), "-I", str(root / "include"), "-I", str(lvgl),
                        *map(str, cache_sources), "-o", str(cache_exe)], check=True)
    elif os.name == "nt":
        vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
        installation = subprocess.check_output([str(vswhere), "-latest", "-products", "*",
                                                "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                                "-property", "installationPath"], text=True).strip()
        if not installation:
            raise SystemExit("No host C++ compiler found; set CXX or install Visual Studio C++ tools")
        vcvars = Path(installation) / "VC/Auxiliary/Build/vcvars64.bat"
        script = build / "build.cmd"
        script.write_text(f'@echo off\ncall "{vcvars}" >nul\n'
                          f'cl /nologo /EHsc /std:c++17 /W4 /I "{root / "src"}" /I "{include}" '
                          f'"{source}" /Fe:"{exe}" /Fo:"{build / "test.obj"}"\n', encoding="utf-8")
        subprocess.run(["cmd", "/c", str(script)], check=True)
        cache_script = build / "build-cache.cmd"
        cache_script.write_text(f'@echo off\ncall "{vcvars}" >nul\n'
                                f'cd /d "{build}"\n'
                                f'cl /nologo /EHsc /std:c++17 /D_USE_MATH_DEFINES /DLV_CONF_INCLUDE_SIMPLE '
                                f'/I "{root / "src"}" /I "{root / "include"}" /I "{lvgl}" '
                                + ' '.join(f'"{s}"' for s in cache_sources) + f' /Fe:"{cache_exe}"\n', encoding="utf-8")
        subprocess.run(["cmd", "/c", str(cache_script)], check=True)
    else:
        raise SystemExit("No host C++ compiler found; set CXX")
    subprocess.run([str(exe)], check=True)
    cache_root = build / "cache-fixture"
    (cache_root / "roads").mkdir(parents=True, exist_ok=True)
    subprocess.run([str(cache_exe), str(cache_root)], check=True)
    for fixture in args.fixture:
        subprocess.run([str(exe), str(fixture.resolve())], check=True)
    if args.live:
        lat, lon = args.tile
        step = 1
        if args.quarter_cell:
            lat, lon = (v / 4 for v in args.quarter_cell)
            step = 0.25
        if not (-90 <= lat < 90 and -180 <= lon < 180):
            raise SystemExit("Tile coordinates out of bounds")
        classes = (args.road_class,) if args.road_class else ("motorway", "trunk")
        for road_class in classes:
            selector = f'[highway="{road_class}"]'
            if not 1 <= args.server_memory_mb <= 512:
                raise SystemExit("Server memory budget must be between 1 and 512 MB")
            bbox = f'({lat},{lon},{lat+step},{lon+step})'
            query = (f'[out:json][timeout:45][maxsize:{args.server_memory_mb * 1024 * 1024}];way' + selector +
                     bbox + ';out skel geom' +
                     (' noids qt' if args.compact_output else '') + ';')
            url = args.url or re.search(r'#define ROAD_OVERPASS_URL "([^"]+)"', (root / "src/config.h").read_text())[1]
            context = ssl.create_default_context()
            if args.insecure:
                context.check_hostname = False
                context.verify_mode = ssl.CERT_NONE
            if args.tls12:
                context.minimum_version = ssl.TLSVersion.TLSv1_2
                context.maximum_version = ssl.TLSVersion.TLSv1_2
            request = urllib.request.Request(url, data=query.encode(),
                                             headers={"User-Agent": "TheOrbOS-road-generator-test/1.0", "Content-Type": "text/plain"})
            print(f"Live query: {url}, bounds=({lat},{lon},{lat+step},{lon+step}) {road_class}, TLS 1.2 only={args.tls12}", flush=True)
            last_query = build / "last-live-query"
            if last_query.exists():
                time.sleep(max(0, 10 - (time.time() - float(last_query.read_text()))))
            try:
                started = time.monotonic()
                try:
                    response = urllib.request.urlopen(request, timeout=65, context=context)
                except urllib.error.HTTPError as exc:
                    with exc:
                        message = re.sub(r"<[^>]*>", " ", exc.read(4096).decode("utf-8", "replace"))
                    raise SystemExit(f"Overpass HTTP {exc.code}: {' '.join(message.split())[:600]}") from None
                with response:
                    headers_s = time.monotonic() - started
                    data = response.read(8 * 1024 * 1024 + 1)
            finally:
                last_query.write_text(str(time.time()))
            if len(data) > 8 * 1024 * 1024:
                raise SystemExit("Live response exceeded firmware's 8 MB limit")
            baseline = build / f"overpass-{lat}_{lon}-{road_class}.json"
            suffix = '-noids' if args.compact_output else ''
            fixture = baseline.with_name(baseline.stem + suffix + '.json')
            fixture.write_bytes(data)
            print(f"Downloaded {len(data)} bytes; headers={headers_s:.2f}s total={time.monotonic()-started:.2f}s", flush=True)
            response_doc = json.loads(data)
            if "remark" in response_doc:
                raise SystemExit(f"Overpass remark: {response_doc['remark']}")
            if args.compact_output and baseline.exists():
                def geometry(doc):
                    return Counter(tuple((p['lat'], p['lon']) for p in way['geometry']) for way in doc['elements'])
                if geometry(response_doc) != geometry(json.loads(baseline.read_bytes())):
                    raise SystemExit("Compact response geometry differs from saved baseline (possibly changed OSM data)")
                print(f"Geometry matches baseline exactly; response reduced by {100*(1-len(data)/baseline.stat().st_size):.1f}%", flush=True)
            subprocess.run([str(exe), str(fixture)] + ([str(v) for v in args.quarter_cell] if args.quarter_cell else []), check=True)


if __name__ == "__main__":
    main()
