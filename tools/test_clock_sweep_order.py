"""Exercise the production sweep's upper-layer loop with overlapping opaque pixels.

Run with --vcvars pointing to vcvars64.bat on Windows, or --cxx c++ elsewhere.
The snippets come from clock_view.cpp so this catches unconditional cache blits too.
"""
import argparse
from pathlib import Path
import subprocess


def block(source, start):
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vcvars")
    parser.add_argument("--cxx", default="c++")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    source = (root / "src/clock_view.cpp").read_text(encoding="utf-8")
    helper = block(source, source.index("static bool sweep_layers_usable("))
    sweep = source.index("static void sweep_frame(")
    start = source.index("            const bool layered =", sweep)
    loop = source.index("            for (int i =", start)
    upper = source[start:loop] + block(source, loop)
    harness = r'''
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>
namespace theme_style {
struct Hand { bool show = true; int blend = 0, pivotX = 0, pivotY = 0;
              int centerX = 0, centerY = 0; };
struct Clock { Hand hand[5]; int order[5] = {0,1,2,3,4}; int orderN = 5;
               bool shadowOn = false; int shadowDX = 0, shadowDY = 0; };
}
struct CustomSprite { const unsigned char *data; int w = 1, h = 1; };
static unsigned char ids[5] = {0,1,2,3,4}, pair = 10, shadow = 11;
static const unsigned char *s_layHand = &pair, *s_layShadow = &shadow;
static std::vector<int> painted;
static const theme_style::Clock *active;
static bool available;
static int requests;
static bool layers_tick(float, float, bool) { ++requests; return available; }
static CustomSprite custom_hand(int k) { return {&ids[k]}; }
static CustomSprite custom_shadow(int k) { return {&ids[k]}; }
static void blend_shadow(const unsigned char *, int, int, int, int, float, float, float) {}
static void blend_custom_hand(const unsigned char *p, int, int, int, int, float, float, float, int) {
    painted.push_back(*p);
}
static void lay_blit(const unsigned char *p) {
    if (p != s_layHand) return;
    for (int i = 0; i < active->orderN; ++i) {
        int k = active->order[i];
        if ((k == 0 || k == 1) && active->hand[k].show) painted.push_back(k);
    }
}
''' + helper + r'''
static void upper_layers(const theme_style::Clock &cs) {
    const float above[5] = {};
''' + upper + r'''
}
int main() {
    theme_style::Clock cs;
    active = &cs;
    int cases = 0;
    do {
        for (int cached = 0; cached < 2; ++cached)
        for (int visible = 0; visible < 4; ++visible)
        for (int blend = 0; blend < 2; ++blend)
        for (int shadows = 0; shadows < 2; ++shadows) {
            available = cached != 0;
            cs.hand[0].show = (visible & 1) != 0;
            cs.hand[1].show = (visible & 2) != 0;
            cs.hand[0].blend = blend;
            cs.shadowOn = shadows != 0;
            painted.clear(); requests = 0;
            // Restored lower layers, followed by the newly painted second hand.
            for (int i = 0; cs.order[i] != 2; ++i)
                if (cs.hand[cs.order[i]].show) painted.push_back(cs.order[i]);
            painted.push_back(2);
            upper_layers(cs);
            std::vector<int> expected;
            for (int k : cs.order) if (cs.hand[k].show) expected.push_back(k);
            // All sprites overlap: sequence equality ensures the correct top pixel
            // and catches duplicated lower hands even when a static hides the error.
            assert(painted == expected);
            int h = -1, m = -1, s = -1;
            for (int i = 0; i < 5; ++i) {
                if (cs.order[i] == 0) h = i;
                if (cs.order[i] == 1) m = i;
                if (cs.order[i] == 2) s = i;
            }
            if (h < s || m < s || blend) assert(requests == 0);
            if (cached && !blend && h > s && m > s && (h == m+1 || m == h+1))
                assert(requests == 1);
            ++cases;
        }
    } while (std::next_permutation(cs.order, cs.order + 5));
    printf("PASS: %d sweep layer-order cases (cache, visibility, blend, shadows)\n", cases);
}
'''
    output = root / ".pio" / "clock-sweep-order-test"
    output.mkdir(parents=True, exist_ok=True)
    cpp = output / "test.cpp"
    cpp.write_text(harness, encoding="utf-8")
    exe = output / "test.exe"
    if args.vcvars:
        command = f'call "{args.vcvars}" >nul && cl /nologo /EHsc /std:c++17 /Fe:"{exe}" /Fo:"{output / "test.obj"}" "{cpp}"'
        subprocess.run(command, shell=True, check=True, cwd=output)
    else:
        subprocess.run([args.cxx, "-std=c++17", str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
