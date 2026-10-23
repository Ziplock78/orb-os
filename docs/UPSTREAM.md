# Updating this fork

`origin` is `https://github.com/Lerxtwood/orb-os.git`.
`upstream` is `https://github.com/Ziplock78/orb-os.git` (branch `main`).

The upstream remote does not automatically fetch tags. This keeps upstream
release tags separate from this fork's `v*-companion` releases.

## Review and integrate

Start with a clean working tree and committed local work. Fetching does not
change the checked-out files:

```powershell
git fetch upstream --prune --no-tags
git log --oneline main..upstream/main
git diff --stat main...upstream/main
git switch main
git switch -c integration/upstream-YYYY-MM-DD
git merge --no-ff --no-commit upstream/main
```

Inspect the full upstream diff, including changes that merge without conflicts.
Resolve overlaps by retaining the intended behavior of this fork, then run:

```powershell
.\.pio\build-venv\Scripts\python.exe -m platformio run -e esp32-s3-amoled-175-companion
git diff --check
git add <resolved-files>
git commit
```

Run the relevant regression checks as well. Route-cache changes are covered by
`tools/companion/test_route_memory.cpp` (compile with `src/route.cpp`, `-Isrc`,
and assertions enabled). The companion CI workflows run this test.

After reviewing and testing the integration branch, promote it explicitly:

```powershell
git switch main
git merge --ff-only integration/upstream-YYYY-MM-DD
git push origin main
```

If `main` has advanced in the meantime, bring those changes into the integration
branch and retest before promotion. Use merges rather than rebasing published
fork history. An integration push does not publish firmware; release tags do.
Create a new companion release only when its firmware is ready to distribute.

## Features to preserve during review

- Companion partition layout, separate PrintSphere settings, and app-only updates.
- Orb web configuration, theme uploads, retained theme art, and USB protocol.
- Location labels and comma search.
- Direct FlightAware lookup, PSRAM-backed TLS, route expiry and immediate RAM-cache
  revisits, and the info-card wait/read timer.
- PrintSphere source pin and reproducible companion adaptations.

## First integration

Branch `integration/upstream-2.16.26` incorporates upstream commit `4df2d82`.
It adopts the shared clock-read helper (including the header/chime fix), safer
geocoding query encoding, and upstream's comma-key support. It also follows upstream
in removing the railway-stop option: sweeping seconds now advance continuously,
and legacy secondRailway theme settings are ignored. The clock-read fallback remains
initialized if conversion fails. No partition or PrintSphere changes are involved.


Flash command for combined firmware:

.\.pio\build-venv\Scripts\python.exe tools/companion/flash.py update-orb --port COM5