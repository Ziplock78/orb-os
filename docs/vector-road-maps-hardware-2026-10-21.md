# Vector road-map hardware observation — 2026-10-21

User-installed firmware: MiltonBanana/2.18.18. Captured COM6 at 115200 baud and polled
`/roads.json` approximately every 0.5 seconds, with periodic `/health` samples.
Firmware and settings were not changed by the monitor.

## Salt Lake City

Serial confirmed the new centre as 40.76070, -111.89101. The location change
included a normal software restart. Times below are seconds relative to the
start of the capture, rather than absolute wall-clock time.

| Capture time | Observation |
| --- | --- |
| 173.266 s | Software restart for the new location |
| 181.172 s | First map-status sample: checking cache, 0/4 tiles |
| 183.000 s | Two cached tiles accepted; next tile downloading, revision 0 |
| 184.016 s | First new tile saved: 3/4, revision 1 |
| 184.532 s | Final tile downloading |
| 185.563 s | Coverage complete: 4/4, revision 2, no error |

The observed map-loading phase completed in approximately **4.4 seconds** from
the first status sample. Including the location-change restart gives approximately
12.3 seconds. Sampling and HTTP response latency limit precision; these are not
firmware-internal stopwatch measurements or a direct measurement of the last
display pixel appearing.

Two of the four tiles were already cached. The other two were newly downloaded,
confirmed by two increments of the cache publication revision:

| New tile | Compressed response | Network | Decode | SD save | Road segments |
| --- | ---: | ---: | ---: | ---: | ---: |
| First | 26,177 bytes | 1,190 ms | 7 ms | 68 ms | 36 |
| Second | 29,456 bytes | 1,225 ms | 11 ms | 69 ms | 40 |

Decode time excludes gzip inflation, which occurs before that timer. Both requests
returned HTTP 200 and gzip data. No retry occurred during this Salt Lake City load.
Coverage remained at 4/4 in subsequent samples. This supports the user's observed
5–10 second experience, while not establishing a completely cold four-tile result
or a matched speedup ratio against the Overpass branch.

## Separate issues observed

- Before the Salt Lake City run, another location stalled at 2/4 with
  `worker stack memory`, then exhausted retries. The current task requests a
  contiguous 24 KiB internal stack; health samples showed only about 16–20 KiB
  in the largest free internal block. This remains an on-device reliability issue.
- Repeated `glyph dsc. not found for U+A9` warnings show that the selected LVGL
  font lacks the copyright symbol used by the map credits. The attribution text
  should use a supported glyph/font to remove the warning flood.
- Aircraft updates occasionally logged approximately 150–160 ms slow snapshots.
  These logs also occurred without road downloads and should not be attributed
  to vector decoding from this capture alone.

Raw artifacts (ignored local files):

- `.pio/vector-tests/slc-serial.log`
- `.pio/vector-tests/slc-status.jsonl`

Next useful validation: repeat with a completely empty road cache, then repeat
location changes after the internal heap has fragmented. Resolve task-stack
allocation reliability before treating every location as consistently fast.

## 100 km range failure and proposed fix

After changing range from 58 km to 100 km on 2.18.18, the device reported
`worker stack memory`, failed at 4/6 tiles, and had approximately 13 KiB free
internal RAM with only a 6–7 KiB largest free block. The most recent successful
tile was HTTP 200, 66,851 compressed bytes, 1,191 ms network, 54 ms decode,
204 ms save and 670 segments. PSRAM still had about 3.5 MiB free. The 24 KiB
per-tile internal stack allocation therefore explains the road download failure.

A further 60-second serial capture showed internal free memory of 13,808 bytes
and largest block 7,156 bytes. It contained no aircraft HTTP request/error lines,
so it does not establish the cause of the reported aircraft feed failure.
Raw files: `.pio/vector-tests/range100-serial.log` and `range100-status.jsonl`.

Firmware 2.18.19 builds with one reusable 24 KiB PSRAM worker stack, internal
task control block, and a queue for sequential requests. It logs stack location,
headroom and per-tile internal memory. It also removes radar attribution labels
in favour of the shared About/startup credits, and uses ASCII weather credits
to avoid the missing copyright glyph warnings. These changes await a hardware
retest; the investigation did not flash the device.

## MiltonBanana/2.18.19 reboot regression

After the user installed 2.18.19, serial capture confirmed repeated panic resets
about every nine seconds. The assertion was:

```text
assert failed: lwip_hook_tcp_isn tcp_isn_default.c:178 (!esp_ptr_external_ram(esp_cpu_get_sp()))
```

Decoding the backtrace against that build identified `lwip_hook_tcp_isn`,
`tcp_next_iss`, `start_ssl_client`, `fetch_one` and `worker_loop`. General SDK
support for external task stacks does not make TCP calls from those stacks safe.
The road worker caused the panic when beginning its connection. Aircraft data
was successfully fetched just before the first captured panic.

Version 2.18.20 corrects this by reserving a single internal 24 KiB worker stack
immediately after boot memory setup, before display, Wi-Fi and other tasks.
It retains the persistent queue/worker, avoiding the original per-tile allocation
failure, while removing the incompatible external stack. This correction is
build-validated and still requires installation and hardware verification.

Capture: `.pio/vector-tests/restart219-serial.log` and `restart219-status.jsonl`.

## MiltonBanana/2.18.20 range-change crash

During a user-triggered 58-to-100 km change, capture `range220-serial.log` recorded
a panic at +14.875 seconds and return to Clock after reboot. Decoding the matching
ELF showed `lock_init_generic`, `__sfp`, `VFSFileImpl`, `SD.open`, `th_open`,
`vector_tile`, `roads_sd::project`, `radar::refreshMap`, `radar::update`,
`host_set_range_km` and `settingsview::onPress`. This identifies an abort while
allocating a newlib file lock during a hidden radar redraw; this is distinct from
the worker's former stack allocation and PSRAM TCP assertion.

Before entering Settings, health showed about 35 KiB internal free. Settings has
an existing measured cost of about 24 KiB internal in small LVGL allocations.
No health sample was obtained at the exact abort; heap values from older boots
in the persistent history should not be treated as measurements of this attempt.

Version 2.18.21 defers range redraw until radar is visible, routes small LVGL
allocations to PSRAM when internal free approaches 16 KiB, and defers road SD work
if internal free is below 8 KiB or the largest block is below 2 KiB. The reusable
internal worker remains unchanged. Host tests exercise allocator routing,
reallocation migration, recovery and fallback. Hardware validation remains pending.
