#!/usr/bin/env python3
"""Cross-check the firmware against Orb Studio.

Every fault this looks for is one that actually shipped, and every one of them was silent:
nothing crashed, nothing logged, a control simply did nothing or a file was simply ignored.
That is the shape of bug this project keeps producing, because the two halves are separate
programs that agree only by convention.

    python3 tools/audit.py [--studio /path/to/buildtheorb/app]

Exit code is the number of findings, so it can gate a release.
"""
import json
import os
import re
import sys

FW = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STUDIO = os.environ.get("ORB_STUDIO_DIR", "/Users/zionbrock/Developer/hf-sites/buildtheorb/app")
for i, a in enumerate(sys.argv):
    if a == "--studio" and i + 1 < len(sys.argv):
        STUDIO = sys.argv[i + 1]

findings = []


def report(kind, msg):
    findings.append((kind, msg))
    print(f"  {kind:<9} {msg}")


def read(path):
    try:
        with open(path, encoding="utf-8", errors="ignore") as f:
            return f.read()
    except OSError:
        return ""


fw_src = {}
for name in os.listdir(os.path.join(FW, "src")):
    if name.endswith((".cpp", ".h")):
        fw_src[name] = read(os.path.join(FW, "src", name))

style_h = fw_src.get("theme_style.h", "")
style_cpp = fw_src.get("theme_style.cpp", "")
# Everything the firmware does OUTSIDE the theme module. A field read only by the parser
# that stores it is a field nothing draws with.
consumers = "".join(v for k, v in fw_src.items() if not k.startswith("theme_style"))

print("\n=== 1. Controls Orb Studio offers that the firmware never reads ===")
print("    The weather map's road colour, its rings and its background colour were all")
print("    like this: set them, install, and the Orb ignored them.\n")

# struct name -> fields, and the variable names each struct gets bound to in the firmware
structs = {}
for m in re.finditer(r"struct\s+(\w+)\s*\{(.*?)\n\};", style_h, re.S):
    name, body = m.group(1), m.group(2)
    # Drop NESTED struct bodies before reading fields. Radar contains a Zone, and counting
    # Zone's x/r/w/rect/invert as Radar's reported five live controls as dead: they are read
    # through a Zone variable, which no amount of looking for "radar.rect" will ever find.
    body = re.sub(r"struct\s+\w+\s*\{.*?\};", "", body, flags=re.S)
    fields = re.findall(r"^\s*(?:bool|int|uint\d+_t|float|double|char)\s+(\w+)\s*(?:\[[^\]]*\])?\s*(?:=|;)",
                        body, re.M)
    if fields:
        structs[name] = fields

# HOW A FIELD GETS READ, and there are two ways, which is what made the first version of
# this check useless: it only knew about the first one and reported fifteen live controls as
# dead. A checker that cries wolf is worse than no checker.
#
#   1. bound to a local reference:  const theme_style::Weather &ws = theme_style::weather();
#                                   ... ws.sweepColor
#   2. called straight through:     theme_style::settings().wheelR
aliases = {}
for m in re.finditer(r"const\s+theme_style::(\w+)\s*&\s*(\w+)\s*=", consumers):
    aliases.setdefault(m.group(1), set()).add(m.group(2))

# struct name -> the accessor that returns it, so form 2 can be matched as well
accessors = {}
for m in re.finditer(r"const\s+(\w+)\s*&\s*(\w+)\(\)", style_h):
    accessors[m.group(1)] = m.group(2)

for sname, fields in sorted(structs.items()):
    names = aliases.get(sname, set())
    if not names and sname not in accessors:
        continue
    for f in fields:
        parsed = f'"{f}"' in style_cpp
        if not parsed:
            continue
        acc = accessors.get(sname)
        used = any(re.search(r"\b%s\s*\.\s*%s\b" % (re.escape(a), re.escape(f)), consumers) for a in names)
        if not used and acc:
            used = bool(re.search(r"theme_style::%s\(\)\s*\.\s*%s\b" % (re.escape(acc), re.escape(f)), consumers))
        if not used:
            # 3. read by a MIGRATION inside the theme module itself. Settings.glow is like
            #    this: superseded by selGlow/itemGlow, kept so a theme saved before the
            #    split still gets its glow. Being read only in there is the point, not a
            #    fault, so a read (rather than the parse that writes it) counts.
            used = bool(re.search(r"=\s*[\w.]*\.%s\s*;" % re.escape(f), style_cpp))
        if not used:
            report("DEAD", f"theme_style::{sname}.{f} is parsed from the theme and never read")

print("\n=== 2. Theme fields Orb Studio sends that the firmware does not parse ===")
print("    A key nobody reads is a control that silently does nothing.\n")

forge = read(os.path.join(STUDIO, "src", "lib", "theme-forge.ts"))
if not forge:
    report("SKIP", f"could not read Orb Studio at {STUDIO}; pass --studio")
else:
    # The style emitters return object literals whose keys are read verbatim by
    # theme_style.cpp. Walk each emitter and check every key it sends.
    for m in re.finditer(r"function (\w+Style)\(t: StudioTheme\) \{(.*?)\n\}", forge, re.S):
        fn, body = m.group(1), m.group(2)
        for km in re.finditer(r"^\s{4}(\w+):", body, re.M):
            key = km.group(1)
            if f'"{key}"' not in style_cpp:
                report("UNREAD", f"{fn}() sends \"{key}\", which theme_style.cpp never parses")

print("\n=== 3. Fixed-size limits with no headroom ===")
print("    MAX_ASSETS was 24. A theme reached 31, the list was cut off, and the firmware")
print("    refused to open files that were on the card.\n")

max_assets = re.search(r"MAX_ASSETS\s*=\s*(\d+)", style_cpp)
if max_assets and forge:
    # Every asset name the forge can emit, counted from the literal file names it builds.
    emitted = set(re.findall(r'name:\s*"([\w./-]+\.(?:png|bin))"', forge))
    emitted |= set(re.findall(r'names:\s*\["([\w./-]+\.bin)"\]', forge))
    emitted |= set(re.findall(r'`font_radar\$\{[^}]+\}\.bin`', forge)) and {"font_radar1.bin", "font_radar2.bin", "font_radar3.bin", "font_radar4.bin"} or set()
    cap = int(max_assets.group(1))
    if len(emitted) > cap:
        report("CAP", f"MAX_ASSETS is {cap} but Orb Studio can emit {len(emitted)} asset names")
    else:
        print(f"    ok: MAX_ASSETS {cap}, Orb Studio can emit at most {len(emitted)}")

    longest = max((len(n) for n in emitted), default=0)
    slot = re.search(r"s_asset\[MAX_ASSETS\]\[(\d+)\]", style_cpp)
    if slot and longest >= int(slot.group(1)):
        report("CAP", f"asset name slot is {slot.group(1)} chars, longest emitted name is {longest}")

print("\n=== 4. Does the simulator stand in for the device? ===")
print("    The simulator registered the weather app with its own entry hook and skipped the")
print("    artwork step, so a plate the device decodes was never decoded there.\n")

main_cpp = fw_src.get("main.cpp", "")
sim_cpp = fw_src.get("sim_main.cpp", "")
main_apps = re.findall(r"app_shell::add\(([^,]+),\s*theme_style::names\(\)\.(\w+)", main_cpp)
sim_apps = re.findall(r"app_shell::add\(([^,]+),\s*(?:theme_style::names\(\)\.)?(\w+)", sim_cpp)
main_order = [a[1] for a in main_apps]
sim_order = [a[1] for a in sim_apps]
if main_order and sim_order and len(main_order) != len(sim_order):
    report("SIM", f"device registers {len(main_order)} apps, simulator registers {len(sim_order)}")
else:
    print(f"    ok: both register {len(main_order)} apps")

# Hooks the device runs on entering a screen that the simulator does not
for hook in ("ui_weather_art_attach", "wx_map_prepare", "clockview::onEnter"):
    in_main = hook in main_cpp
    in_sim = hook in sim_cpp
    if in_main and not in_sim:
        report("SIM", f"the device calls {hook}() on entry and the simulator does not")

print("\n=== 5. Capability levels ===")
print("    A feature the firmware gained without a THEME_CAPS entry installs on an Orb")
print("    that cannot draw it, and is ignored with nothing said.\n")

caps_fw = re.search(r"THEME_CAPS\s*=\s*(\d+)", style_h)
caps_ts = read(os.path.join(STUDIO, "src", "lib", "orb-caps.ts"))
levels = sorted(int(x) for x in re.findall(r"level:\s*(\d+)", caps_ts)) if caps_ts else []
if caps_fw and levels:
    fw_level = int(caps_fw.group(1))
    if max(levels) != fw_level:
        report("CAPS", f"firmware is at THEME_CAPS {fw_level}, Orb Studio's table tops out at {max(levels)}")
    else:
        print(f"    ok: both at level {fw_level}")
    # A GAP IS NOT AUTOMATICALLY A FAULT, so it is said rather than counted. Level 2 has a
    # written justification in orb-caps.ts: the firmware learned something at that level, and
    # Studio arranged for the design to look right on firmware that never heard of it, which
    # is better than making somebody flash. Reporting that forever would train everyone to
    # ignore this tool, and a checker that gets ignored is worse than no checker.
    #
    # The finding that matters is the one above: the two sides disagreeing about the TOP
    # level, which is what happens when a feature ships without its row.
    missing = [n for n in range(2, fw_level + 1) if n not in levels]
    if missing:
        print(f"    note: no CAPS_FEATURES row for level(s) {missing}. Fine when the design")
        print(f"          works on firmware below that level; a fault when it silently does not.")

print("\n=== 7. Curved text that orbits something other than the dial's middle ===")
print("    The weather map passed the line's own x/y as the arc CENTRE, so a line dragged")
print("    to the edge and then curved flew off the screen around a circle centred on")
print("    wherever it had been left.\n")

# draw_arc(dst, font, str, CX, CY, R, arcDeg, ...) — arguments 4 and 5 are the centre the
# text orbits, and on a round 466 px screen there is exactly one right answer for both.
CENTRE_OK = ("SCREEN_W / 2", "SCREEN_H / 2", "W / 2", "H / 2",
             "SCREEN_W/2", "SCREEN_H/2", "W/2", "H/2", "SCREEN_CX", "SCREEN_CY", "233")


def split_args(text):
    """Top-level commas only. A cast like (float)s_cy has parens of its own, and the regex
    this started as chopped straight through them and then reported the wreckage."""
    out, depth, cur = [], 0, ""
    for ch in text:
        if ch in "([": depth += 1
        elif ch in ")]": depth -= 1
        if ch == "," and depth == 0:
            out.append(cur); cur = ""
        else:
            cur += ch
    out.append(cur)
    return [a.strip() for a in out]


def resolve(expr, src):
    """A bare local standing in for the centre is still the centre.

    ticker_view binds `const float cy = SCREEN_H / 2.0f;` and passes `cy`, which the first
    version of this check reported as a fault. One false positive is all it takes for a
    checker to start being ignored, so a plain identifier is looked up once before judging.
    """
    # Cast first, THEN parens. The other order turns "(float)s_cx" into "float)s_cx",
    # which is neither an identifier nor an expression, and the report prints the wreckage.
    e = expr.strip()
    e = re.sub(r"^\(\s*(?:float|int|lv_coord_t)\s*\)\s*", "", e).strip()
    while e.startswith("(") and e.endswith(")"):
        e = e[1:-1].strip()
    if not re.fullmatch(r"[A-Za-z_]\w*", e):
        return e
    # Declared with a type (a local), or as a file-scope static that may be declared
    # alongside its twin: `static lv_coord_t s_cx = SCREEN_CX, s_cy = SCREEN_CY;`
    m = re.search(r"\b(?:static\s+)?(?:const\s+)?(?:float|int|lv_coord_t|auto)\s+[^;]*?\b%s\s*=\s*([^,;]+)[,;]" % re.escape(e), src)
    return m.group(1).strip() if m else e


for name, src in sorted(fw_src.items()):
    if not name.endswith(".cpp"):
        continue
    for m in re.finditer(r"curved_text::draw_arc\(([^;]*?)\);", src, re.S):
        args = split_args(m.group(1))
        if len(args) < 5:
            continue
        cx, cy = resolve(args[3], src), resolve(args[4], src)
        if not any(k in cx for k in CENTRE_OK) or not any(k in cy for k in CENTRE_OK):
            line = src[:m.start()].count("\n") + 1
            report("ARC", f"{name}:{line} curves text about ({cx}, {cy}), not the screen centre")
if not any(k == "ARC" for k, _ in findings):
    print("    ok: every curved line orbits the middle of the dial")

print("\n=== 6. Is the deployed Orb Studio serving the current firmware? ===")
print("    Publishing copies the binary to disk. Studio serves what its BUILD contains.\n")

pub = read(os.path.join(STUDIO, "public", "firmware", "manifest.json"))
dist = read(os.path.join(STUDIO, "dist", "client", "firmware", "manifest.json"))
if pub and dist:
    pv = json.loads(pub).get("version")
    dv = json.loads(dist).get("version")
    if pv != dv:
        report("DEPLOY", f"published {pv} but the deployed build still serves {dv}; run vite build + wrangler deploy")
    else:
        print(f"    ok: both {pv}")

print()
if findings:
    print(f"{len(findings)} finding(s).")
else:
    print("Nothing found.")
sys.exit(len(findings))
