# Adding a screen

A screen is not a firmware feature. It is a firmware feature **plus** a design surface in
Orb Studio, and it is not finished until both halves agree. This is the checklist for
building one so it arrives complete instead of being repaired in public over several days.

## Why this file exists

The Headlines (Intel) screen was built a piece at a time in August 2026, and every piece
that got missed had already existed on every other screen for months. In order, the owner
had to ask for: a background image, the glass and CRT layer, a typeface for each text
element, top and bottom margins, and version saving. None of these were new ideas. They
were the standard equipment of a screen, and shipping without them was not a smaller
version of the feature, it was a broken one.

Two of the misses failed in ways worth remembering, because they are the shape most of
these take:

- **Version saving did not degrade, it threw.** Studio keeps a map of which slice of a
  theme each screen owns (`SCREEN_KEYS` in `theme-store.ts`). Intel was absent, so the
  lookup returned `undefined`, and the deep copy every save makes is
  `JSON.parse(JSON.stringify(undefined))` — a `SyntaxError`. The row appeared and then
  vanished on the next read. A missing registration is rarely a missing feature; it is
  usually a crash somewhere downstream.
- **A new font size with no line height overlapped its own text.** Studio's preview reads
  line heights from a table. New sizes were added to the size ladder and not to the table,
  so the fallback was used and 22 px text was laid out with 18 px of leading. Two lines
  drew on top of each other. Every lookup table that is keyed by an option needs an entry
  for every value of that option, and a test that says so.

The rule this file encodes: **anywhere a screen has text, a picture, or a background, it
gets the same controls every other screen has for that thing.** Screens differ in what
they show. They do not differ in how their text is set.

## The standard parts

### Every screen

| Part | Firmware | Studio |
|---|---|---|
| Background | colour, plus `<screen>_plate.png` decoded flash-first then SD | `BackgroundCard` (colour **or** image, zoom, dim), pinned to the back |
| Glass / CRT | `<screen>_overlay.png`, composited over everything | `GlassCrtCard` on the shared `clock.overlay` object |
| Version history | — | an entry in `SCREEN_KEYS`, or saving throws |
| Capability level | a `THEME_CAPS` bump **and** a ledger entry per feature | a matching row in `CAPS_FEATURES` |
| Memory | attach art on enter, release on exit | — |
| Preview parity | — | the preview runs the firmware's layout arithmetic, not an approximation |

Ship the plate only when the design actually uses a picture. Baked art is raw RGB565, so a
466x466 plate of flat colour costs 424 KB of the `themeart` partition to say what one JSON
field already says. The glass is already skipped when it is switched off.

### Every text element

This is the part that kept getting shortchanged. A text element means **all** of these,
not a subset:

- **Words** — for static text, or a format string with a token for anything live
  (`{t}`, `{callsign}`). Any text that can come from outside must be ASCII-folded before it
  reaches the device: the font has no fallback, so a curly apostrophe draws an empty box.
- **Typeface and weight** — shipped as a `font_<screen>_<slot>.bin` theme font, with a slot
  in `theme_font.cpp`. A converted face is baked at one size by `lv_font_conv`, so the size
  control is what gets baked rather than something the device varies afterwards. Give the
  screen an `intel_has_font`-style predicate if it needs to know which it got.
- **Size** — from the compiled ladder only (`lv_conf.h`). LVGL fonts are glyph bitmaps, not
  outlines: a size the binary was not built with cannot be drawn at any quality. Offer a
  slider over ladder positions, never a free pixel value, and snap unknown values to the
  default rather than to the nearest — nearest silently redesigns the theme.
- **Colour**, and a **stale/alternate colour** if the element can go out of date.
- **Glow** and glow colour.
- **Across / Down** — absolute screen px, hidden when the element is curved.
- **Curve** — on, radius, angle, via `curved_text::draw_arc`. Do not write a fourth copy of
  the glyph-rotation maths; it lives in `curved_text.cpp` and both the clock and the scope
  already call it.
- **Show / hide.**

### Every image element

Source, on/off, zoom, dim, position. Decoded flash-first then SD, released on exit, and
declared in the theme's asset list so a file left behind by an older push is not drawn.

### Layout

Left, right, **top and bottom** margins. Top and bottom were the ones missed on Intel: the
band was worked out from whatever sat above and below, which is a fine default and a poor
ceiling. Default them to 0 meaning "work it out", and let a stated value win.

Where a screen stacks layers, it gets the same layer ordering the Flight Tracker has, with
the background pinned at the bottom and the glass pinned at the top.

**The controls column is the stack, read downwards.** Top of the column is the layer
nearest the glass; bottom of the column is the background. Bracket it with the same
`StackEnd` markers the Flight Tracker uses so the order explains itself. Intel shipped with
its glass card sitting second from the bottom, directly contradicting the comment attached
to it, and it read as the glass having stopped being the top layer.

### Screens fed by live data

- Poll interval as theme data, not a `#define`.
- An age or freshness readout, and a colour that changes when it stops being current.
- Honest empty states that say **which** thing is unwell: no WiFi, not asked yet, and the
  service not answering are three different sentences.
- If more can arrive than fits, the knob scrolls: press takes the knob, turn moves, press
  again or an idle timeout releases. Use the Flight Tracker's grammar rather than a new one.
- Separate **how many are fetched** from **how many are shown**. They are different
  questions and welding them together caps the design to the dial.
- Hold the data in one place and read narrow windows out of it. At twenty items an
  `IntelSnapshot` is over 2 KB, and copying that onto a task stack is not something this
  device's internal RAM can absorb.
- Build widgets for what can be **seen**, not for what can be **held**.

## Before calling it done

- [ ] Both firmware targets build (`native` and `esp32-s3-amoled-175`).
- [ ] `THEME_CAPS` bumped, ledger entry written, `CAPS_FEATURES` row added.
- [ ] `SCREEN_KEYS` entry added, and a version saves and survives a reload.
- [ ] Studio defaults reproduce the firmware's compiled defaults **exactly** — a brand new
      theme must look identical to what the screen drew before it was themeable. There is a
      test for this; add one for the new screen.
- [ ] Every clamp in Studio matches the clamp in `theme_style.cpp`. Both directions.
- [ ] Every option that indexes a lookup table has an entry in it, with a test that walks
      the option's full range.
- [ ] The preview was checked against a simulator screenshot of the same settings, not
      against expectation.
- [ ] Nothing new is drawn on a resting screen that a person did not ask for. An indicator
      that sits inside a line of text becomes punctuation.
- [ ] PSRAM taken on enter is given back on exit.

## Where things live

| | |
|---|---|
| Screen view | `src/<screen>_view.cpp` |
| Its art loader | `src/<screen>_sprite.cpp` (model on `intel_sprite.cpp`, the smallest) |
| Theme data | `src/theme_style.{h,cpp}` |
| Font slots | `src/theme_font.{h,cpp}` |
| Curved text | `src/curved_text.cpp` — shared, do not copy |
| Studio model, defaults, bake | `buildtheorb/app/src/lib/theme-forge.ts` |
| Studio capability table | `buildtheorb/app/src/lib/orb-caps.ts` |
| Studio version keys | `buildtheorb/app/src/lib/theme-store.ts` |
| Studio controls and preview | `buildtheorb/app/src/routes/studio.tsx` |
| Tests | `buildtheorb/app/src/lib/theme-screens.test.ts` |
