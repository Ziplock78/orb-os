# Memory

Read this before adding a screen. Nearly every hang and every "allocation failed" in this
project's history has been a memory question wearing a different hat, and the ones that were
not were a threading question about the same buffers.

## What there is

| | |
|---|---|
| PSRAM | 8 MB. Everything large lives here. |
| Internal RAM | ~320 KB. Task stacks, the LVGL draw buffer, DMA. Contended and small. |
| Flash | 16 MB, of which `themeart` holds pre-baked theme artwork. |

`heap_caps_malloc_extmem_enable(4096)` is set at boot, so any allocation over 4 KB goes to
PSRAM without asking. That is why the internal heap survives at all.

**Internal RAM is the scarce one, not PSRAM.** The Orb cannot do TLS because it cannot raise
two contiguous ~16 KB internal blocks, and it never will while the display and WiFi are up.
Anything you write that wants a big buffer must say `MALLOC_CAP_SPIRAM` explicitly or be
over 4 KB. A 3 KB buffer without the flag comes out of the 320 KB.

## The unit costs

Everything below is one of these. Learn the four numbers and you can size a screen in your
head before writing it.

| Thing | Size |
|---|---|
| Full-screen opaque plate, 466x466 RGB565 | **424 KB** |
| Full-screen glass or overlay, 466x466 RGB565+alpha | **636 KB** |
| One weather frame, 360x360 RGB565 | **253 KB** |
| One 1-bit full-map mask, 360x360 | **16 KB** |

## The contract

**Take memory when your screen is entered. Give it back when it is left.**

Every screen registers `onEnter` and `onExit` with `app_shell::add()`. They fire on a real
app change only, never per switcher detent. What a screen holds while it is not on the dial
should be close to nothing.

This is not a style preference. Before it existed the weather map could not get its frame
buffers at all: it was last in the queue and everything ahead of it had taken and kept what
it needed at boot. The log said `begin: 1/5 frame buffers allocated` and the animation
silently ran on a single frame for weeks.

```
clock       382 KB free  ->  938 KB free      (releasing the clock's canvas on exit)
weather     444 KB free  -> 1438 KB free      (all five frames, first time on hardware)
```

## What each app holds while it is up

| App | Holds | Roughly |
|---|---|---|
| **Clock** | canvas + rotation cache | 848 KB |
| **Flight tracker** | plate + glass + road cache + coastline + text canvas | ~1.4 MB |
| **Weather radar** | 5 frames + back buffer, PNG decoder, plate crop | ~1.8 MB |
| **News** | plate + glass | ~1.1 MB |
| **Stock ticker** | plate, and the strip canvas only when the tape is curved | 424 KB, or ~1.1 MB curved |
| **Settings** | plate + text canvas | ~700 KB |
| **Surveillance** | camera view | not yet on the contract |

Always resident, on top of whatever app is up:

- LVGL partial draw buffer, 466 x 40 lines, in **internal** RAM: 36 KB
- Rotation scratch + logical framebuffer, PSRAM: 36 KB + 424 KB
- Decoded theme fonts, a few tens of KB each

## The three things that are deliberately never freed

Not oversights. Each one is a thread question answered by refusing to have one.

- **The weather map's road and coastline masks**, 16 KB each. Written by the UI thread,
  read by the network task while it builds a frame. Never freeing them means there is no
  moment where one is reading while the other takes it away.
- **The weather map's background crop**, 253 KB. Same two threads, same reason.
- **`roads_sd`'s projection cache**, ~154 KB. Reserved at boot on purpose, while PSRAM is
  still unfragmented: by the time the Flight Tracker first runs, a mid-session request for
  a contiguous block this size can fail even with megabytes free.

Fragmentation is why "free PSRAM" is the less useful number. `?orb mem` reports the largest
free block as well, and that is the one that decides whether an allocation succeeds.

## Threads

Two, and they do not share buffers safely:

- **UI thread (core 1)** — LVGL, every screen's drawing, the web server, the USB command
  handler. **Reads the SD card.**
- **Network task (core 0)** — aircraft, weather tiles, headlines, quotes.

Three rules learned the hard way, each of which cost a wedged device:

1. **Only the UI thread touches the SD card.** The Arduino SD driver is not safe for two
   tasks, and the UI thread reads theme art on every app switch. Reading road tiles from the
   network task survived a slow walk through the apps and died within seconds of a fast one.
2. **Free a buffer from the task that writes it.** The UI thread freeing weather frames
   while core 0 was mid-decode hung the device twice. The UI now sets a flag and the network
   task frees at the top of its loop, where nothing is part-way through.
3. **A wedged UI thread looks like a healthy device.** Core 0 keeps logging aircraft while
   the screen, the web server and the USB handler all stop together. If `?orb` goes quiet
   but `[adsb]` keeps printing, something on the display thread died without saying so.

## Adding a screen

- Take art in `onEnter`, release in `onExit`. Check with `?orb mem` on the way in and out;
  the number should come back.
- Ship a plate only when the design uses a picture. 424 KB of `themeart` to say what one
  JSON field already says is not a saving anywhere.
- Allocate anything large in PSRAM explicitly.
- If two threads can see a buffer, either give it a single owner or never free it, and write
  down which you chose and why.
- Delete an LVGL canvas object on exit rather than handing it a null buffer. LVGL does not
  accept the latter and wedges the UI thread on the next app switch.
