# Manual build, flash, commit, and release

Reference for **Orb OS + PrintSphere** on the 16 MB ESP32-S3 AMOLED 1.75, using Windows PowerShell.

- Repository: https://github.com/Lerxtwood/orb-os
- Web installer: https://lerxtwood.github.io/orb-os/
- Working directory: `D:\git\Arduino\orb-os`
- Example device port: `COM5`

Run commands section by section. Stop on errors; do not flash an old binary after a failed build.

For a public release without a local device test, follow sections **1, 5, 6, 7, and 8**. GitHub Actions builds both firmwares from fresh source checkouts. Sections **2–4** are for local builds and device testing; a local Orb build alone does not build PrintSphere or publish a release.

## 1. Set up your shell

```powershell
Set-Location D:\git\Arduino\orb-os
$ErrorActionPreference = 'Stop'
$orbPython = Join-Path $PWD '.pio\build-venv\Scripts\python.exe'
$devicePort = 'COM5'

git status -sb
git remote -v
```

`origin` should point to `https://github.com/Lerxtwood/orb-os.git`. Always use `-R Lerxtwood/orb-os` with GitHub CLI commands: a fork's default CLI repository can be its upstream.

If Git reports **dubious ownership** for this trusted checkout, allow this specific directory and retry:

```powershell
git config --global --add safe.directory D:/git/Arduino/orb-os
```

If the Python environment does not exist, create it once:

```powershell
py -3 -m venv .pio\build-venv
$orbPython = Join-Path $PWD '.pio\build-venv\Scripts\python.exe'
& $orbPython -m pip install platformio pyserial
if ($LASTEXITCODE -ne 0) { throw 'Python dependency installation failed.' }
```

Then verify the tools:

```powershell
& $orbPython -m platformio --version
if ($LASTEXITCODE -ne 0) { throw 'PlatformIO is not available in the selected Python environment.' }
gh auth status
if ($LASTEXITCODE -ne 0) { throw 'Run gh auth login before continuing.' }
```

Git and GitHub CLI must be installed. PlatformIO downloads its framework and compiler on the first build. Local installer tests also need Node.js 22 or newer; an existing workstation-specific alternative is shown below. In Windows PowerShell, `$ErrorActionPreference` does not stop on a native program's nonzero exit code, so keep the explicit `$LASTEXITCODE` checks.

## 2. Compile Orb

For the combined Orb/PrintSphere device:

```powershell
& $orbPython -m platformio run -e esp32-s3-amoled-175-companion
if ($LASTEXITCODE -ne 0) { throw 'Orb build failed. Do not flash.' }
```

Wait for `SUCCESS`. The output is:

```text
.pio\build\esp32-s3-amoled-175-companion\firmware.bin
```

Normal incremental builds are sufficient. If you need a clean rebuild:

```powershell
& $orbPython -m platformio run -e esp32-s3-amoled-175-companion -t clean
if ($LASTEXITCODE -ne 0) { throw 'Clean failed.' }
& $orbPython -m platformio run -e esp32-s3-amoled-175-companion
if ($LASTEXITCODE -ne 0) { throw 'Orb build failed.' }
```

Local builds display `FW_VERSION` from `src/config.h`. The release workflow stamps its version from the release tag, so a local test build may display a different version from the eventual public release.

## 3. Flash Orb to the device

Close Orb Studio, browser USB connections, serial monitors, and other programs using the port. Connect the device with a USB data cable.

```powershell
& $orbPython -m serial.tools.list_ports
& $orbPython tools/companion/flash.py update-orb --port $devicePort
if ($LASTEXITCODE -ne 0) { throw 'Flash failed. Read the error before continuing.' }
```

The helper checks the partition table and writes only Orb's application at `0x10000`. It preserves PrintSphere, settings, theme artwork, and the partition table. Look for **Hash of data verified**.

The helper defaults to `%USERPROFILE%\.platformio\packages\tool-esptoolpy\esptool.py`. If that installation is elsewhere, supply it explicitly:

```powershell
& $orbPython tools/companion/flash.py update-orb --port $devicePort --esptool 'C:\path\to\esptool.py'
```

After Orb finishes booting:

```powershell
& $orbPython tools/companion/probe.py --port $devicePort hello mem
```

`hello` reports the firmware version, theme, and uptime. `mem` includes free PSRAM and its largest free block. This probe speaks Orb's protocol: return from PrintSphere to Orb before running it.

### Partition-table rejection

If the helper says the device is not using the companion partition table, **do not bypass the check or use a generic PlatformIO upload command**. Standalone Orb has a different layout.

Use the web installer for initial migration. It preserves settings and SD theme files and does not require a firmware backup. Select **Rebuild theme cache** if the old artwork cache does not fit. Keep the SD card inserted for the first-boot rebuild.

After migration, use `update-orb` and `update-printer` for local updates. The helper's `install` and `restore` actions require specially prepared full-device images and are not routine update commands.

### USB troubleshooting

- Close other programs using the port and check the port list again.
- Allow time for boot and theme-cache rebuilding before probing.
- If necessary, unplug/reconnect USB, confirm the port, and retry the app-only flash.
- A probe timeout alone does not mean a verified firmware write failed.

## 4. Build and flash PrintSphere, when needed

Skip this section for Orb-only changes. Public releases build both firmwares automatically.

The public release uses the PrintSphere commit in `tools/companion/printsphere-ref.txt`. Companion adaptations are maintained in this Orb repository. Build an isolated copy rather than modifying the original PrintSphere checkout.

### Prepare pinned source

Run this clone command once, if the destination does not exist:

```powershell
git clone https://github.com/Lerxtwood/PrintSphere.git .pio/companion/PrintSphere-source
if ($LASTEXITCODE -ne 0) { throw 'PrintSphere clone failed.' }
```

Then prepare the pinned revision:

```powershell
$printerRef = (Get-Content tools/companion/printsphere-ref.txt -Raw).Trim()
git -C .pio/companion/PrintSphere-source status --short
# Preserve any edits shown above before changing revisions.
git -C .pio/companion/PrintSphere-source fetch origin
if ($LASTEXITCODE -ne 0) { throw 'PrintSphere fetch failed.' }
git -C .pio/companion/PrintSphere-source checkout --detach $printerRef
if ($LASTEXITCODE -ne 0) { throw 'Pinned PrintSphere checkout failed.' }
# For a fully fresh adapted source tree, remove only this generated copy.
$companionRoot = (Resolve-Path .pio/companion).Path
$adaptedSource = [IO.Path]::GetFullPath((Join-Path $companionRoot 'PrintSphere'))
if ([IO.Path]::GetDirectoryName($adaptedSource) -ne $companionRoot) {
    throw 'Refusing to remove source outside the companion workspace.'
}
if (Test-Path -LiteralPath $adaptedSource) {
    Remove-Item -LiteralPath $adaptedSource -Recurse -Force
}
& $orbPython tools/companion/prepare_printsphere.py --source .pio/companion/PrintSphere-source
if ($LASTEXITCODE -ne 0) { throw 'PrintSphere preparation failed.' }
```

This creates the adapted copy in `.pio\companion\PrintSphere`. Always prepare from the original pinned source, not from the already-adapted copy.

### Compile and flash

Install ESP-IDF **5.5.4** and its Python environment first. On the current workstation:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\companion\build_printsphere.ps1 -Clean
if ($LASTEXITCODE -ne 0) { throw 'PrintSphere build failed.' }
& $orbPython tools/companion/flash.py update-printer --port $devicePort
if ($LASTEXITCODE -ne 0) { throw 'PrintSphere flash failed.' }
```

For different ESP-IDF paths:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\companion\build_printsphere.ps1 `
  -IdfPath 'C:\Users\chris\esp\esp-idf-v5.5.4' `
  -IdfPython 'C:\Users\chris\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe' -Clean
if ($LASTEXITCODE -ne 0) { throw 'PrintSphere build failed.' }
```

Output: `.pio\companion\ps-build\printsphere_idf.bin`. The helper writes only the PrintSphere slot at `0xAD0000`.

Use `-Clean` for the first build, after preparing a new source revision, or when troubleshooting. It removes the generated PrintSphere build directory and regenerates `sdkconfig` from the source defaults. It does not erase anything on the device or change the original PrintSphere checkout. For subsequent builds of the same prepared source, you may omit `-Clean` to build incrementally.

**White screen after selecting Printer:** a previously reused local build stalled during display initialization, while a fresh build from the same pinned source booted successfully. Reprepare the source as above, build with `-Clean`, then run `update-printer` again. Do not use the generic `idf.py flash` command printed by ESP-IDF: it places PrintSphere in Orb's slot and also writes shared boot metadata. Our `update-printer` helper verifies the combined partition layout and updates only PrintSphere.

The local build helper currently labels its build `2.0.22-orbtest`; CI uses the release tag plus `-orb`. Test entering Printer, printer status, dial navigation, and jig/press to return to Orb.

If intentionally changing the PrintSphere revision, commit and push that source first, then update `printsphere-ref.txt` and retest preparation/build. CI cannot include uncommitted changes from your local PrintSphere checkout.

## 5. Run checks and test the device

```powershell
git diff --check
if ($LASTEXITCODE -ne 0) { throw 'Fix whitespace errors before continuing.' }
& $orbPython tools/companion/test_release.py
if ($LASTEXITCODE -ne 0) { throw 'Release asset tests failed.' }
node --test tools/companion/test_layout.mjs
if ($LASTEXITCODE -ne 0) { throw 'Installer tests failed.' }
```

If Node is not on PATH, this workstation also has a copy bundled with Playwright:

```powershell
& .\.pio\build-venv\Lib\site-packages\playwright\driver\node.exe --test tools/companion/test_layout.mjs
if ($LASTEXITCODE -ne 0) { throw 'Installer tests failed.' }
```

That alternative requires Playwright to be installed in the environment (`& $orbPython -m pip install playwright`). Check that the executable exists with `Test-Path` before using it. Otherwise install Node.js 22 or newer and reopen PowerShell. CI installs Node itself and also builds/runs the dial, route-cache, and photo-cache C++ regression tests.

Test the changed behavior on the device before releasing. For radar/photo work, check first lookup, cached revisits, dial controls, leaving/reentering radar, and memory diagnostics. Check configuration and theme uploads when your changes affect those paths.

## 6. Commit and push source changes

Review the branch and changes:

```powershell
git branch --show-current
git status --short
git diff --stat
git diff
```

For the normal release process, finish on `main`. Integrate and test other branches before releasing. See [UPSTREAM.md](UPSTREAM.md) for upstream updates.

Stage the intended source files. Replace these examples with the files you changed:

```powershell
git add src/photo.cpp src/photo.h
git diff --cached --stat
git diff --cached --check
git diff --cached
git commit -m "Describe the firmware behavior changed"
if ($LASTEXITCODE -ne 0) { throw 'Commit failed.' }
```

Do not stage `.pio` outputs or full-device backups. Backups can contain private Wi-Fi and configuration data.

Fetch and review before pushing:

```powershell
git fetch origin
if ($LASTEXITCODE -ne 0) { throw 'Fetch failed.' }
git log --oneline --left-right HEAD...origin/main
```

If the remote advanced, merge its changes, resolve conflicts, and repeat relevant checks **before pushing**. Do not force-push over other work.

```powershell
git push origin main
if ($LASTEXITCODE -ne 0) { throw 'Push failed. Resolve the cause before releasing.' }
git status -sb
```

A push to `main` runs **Check Orb companion build**, but does not publish firmware:

```powershell
gh run list -R Lerxtwood/orb-os --workflow webflasher.yml --limit 5
# Select the run for the commit reported by git rev-parse HEAD.
$checkRun = Read-Host 'Enter the check workflow run ID for your commit'
gh run view $checkRun -R Lerxtwood/orb-os --json headSha,headBranch,status,conclusion,url
gh run watch $checkRun -R Lerxtwood/orb-os --exit-status --interval 30
if ($LASTEXITCODE -ne 0) { throw 'CI checks did not pass.' }
```

## 7. Prepare and publish a release

### Choose a new version

Match the firmware version from the most recently merged upstream revision and
append `-companion`. For example, upstream `2.17.00` releases as
`v2.17.00-companion`; retain the upstream spelling, including leading zeroes.
Keep upstream's `FW_VERSION` in source. The release workflow checks the tag
against that version and stamps the suffix into the published firmware.

```powershell
gh release list -R Lerxtwood/orb-os --limit 10
git fetch origin --tags
if ($LASTEXITCODE -ne 0) { throw 'Tag fetch failed.' }
git status -sb
git log -1 --oneline
$versionMatch = [regex]::Match((Get-Content src/config.h -Raw), '#define FW_VERSION "([^"]+)"')
if (-not $versionMatch.Success) { throw 'Could not read FW_VERSION.' }
$upstreamVersion = $versionMatch.Groups[1].Value -replace '-companion$', ''
$releaseTag = "v$upstreamVersion-companion"
Write-Host "Release tag: $releaseTag"
if ($releaseTag -notmatch '^v[0-9]+\.[0-9]+\.[0-9]+-companion$') { throw 'Use vX.Y.Z-companion.' }
if (($releaseTag + '-orb').Length -gt 31) { throw 'Version is too long for the firmware descriptor.' }
if (git status --porcelain) { throw 'Commit or preserve working-tree changes before releasing.' }
git ls-remote --tags origin "refs/tags/$releaseTag"
```

If the last command lists an existing tag, stop; also check `git tag --list $releaseTag` for a local tag. Do not advance a separate companion version counter or overwrite a published tag. If another release is needed before the next upstream merge, explicitly resolve the naming exception and workflow validation first. Release the tested commit, with a clean working tree and `main` pushed. Local binaries are not uploaded: CI stamps Orb with the tag without `v`, and PrintSphere with the full tag plus `-orb`.

### Write release notes

```powershell
New-Item -ItemType Directory -Force .pio/companion | Out-Null
@'
Orb OS + PrintSphere for the ESP32-S3 AMOLED 1.75.

Web installer: https://lerxtwood.github.io/orb-os/

Changes:
- Describe the user-visible changes and relevant limitations.

Validation:
- Describe what was actually built and tested.
'@ | Set-Content -Encoding UTF8 .pio/companion/release-notes.md
```

Edit that file to describe your actual changes and validation.

### Push the release tag

```powershell
git tag -a $releaseTag -m "Release $releaseTag"
if ($LASTEXITCODE -ne 0) { throw 'Tag creation failed.' }
git push origin $releaseTag
if ($LASTEXITCODE -ne 0) { throw 'Tag push failed.' }
```

**Pushing the tag triggers publication.** The `Release Orb Companion` workflow checks out the tagged Orb source and pinned PrintSphere source, runs the checks, stamps the version, builds both firmwares, publishes the release assets, and deploys the web installer.

There is no need to upload local binaries or run `gh release create` manually.

### Watch publication and add your notes

```powershell
gh run list -R Lerxtwood/orb-os --workflow release.yml --limit 5
$releaseRun = Read-Host 'Enter the release workflow run ID for your tag'
gh run watch $releaseRun -R Lerxtwood/orb-os --exit-status --interval 30
if ($LASTEXITCODE -ne 0) { throw 'Release or installer deployment failed.' }

gh release edit $releaseTag -R Lerxtwood/orb-os `
  --title $releaseTag --notes-file .pio/companion/release-notes.md
if ($LASTEXITCODE -ne 0) { throw 'Release notes update failed.' }
gh release view $releaseTag -R Lerxtwood/orb-os
```

Both the `release` job and `pages / deploy` job must succeed. Attach notes after the workflow has created the release.

Editing release notes replaces the generated body. Keep the installer link, hardware requirements, and installation instructions your users need in your notes.

## 8. Verify the live installer

Open https://lerxtwood.github.io/orb-os/ and confirm your new version appears. The public assets are:

- `orb-bootloader.bin`
- `orb-partitions.bin`
- `Orb-companion.bin`
- `PrintSphere-companion.bin`
- `companion-release.json`

Verify the live manifest and all firmware checksums with this PowerShell block:

```powershell
@'
import hashlib, json, sys, urllib.request
base = 'https://lerxtwood.github.io/orb-os/'
expected = sys.argv[1]
def get(url):
    with urllib.request.urlopen(url, timeout=90) as response:
        return response.read()
index = json.loads(get(base + 'release-index.json'))
entry = next(item for item in index if item['tag'] == expected)
manifest_url = base + entry['manifest']
manifest = json.loads(get(manifest_url))
assert manifest['version'] == expected
print('Release commit:', manifest['commit'])
for part in manifest['parts']:
    data = get(manifest_url.rsplit('/', 1)[0] + '/' + part['path'])
    assert len(data) == part['size']
    assert hashlib.sha256(data).hexdigest() == part['sha256']
    print(part['path'], 'verified')
print(expected, 'is available from the web installer')
'@ | & $orbPython - $releaseTag
if ($LASTEXITCODE -ne 0) { throw 'Live release verification failed.' }
```

Compare the manifest's commit with `git rev-parse "$releaseTag^{commit}"`. The web installer flashes separate images at fixed addresses; never upload a private full-device backup as a release asset.

## 9. Retry a failed workflow

```powershell
gh run view $releaseRun -R Lerxtwood/orb-os --log-failed
```

For a failed `main` check, substitute `$checkRun`. Read the first failing step: an installer test failure prevents compilation and publication. The previously observed `connectionLog.closest is not a function` was an outdated mock DOM in `test_layout.mjs`; fix the harness when browser APIs change, then rerun tests and push the fix. Do not skip the checks to publish.

For a transient failure:

```powershell
gh run rerun $releaseRun -R Lerxtwood/orb-os --failed
```

To deliberately rebuild an existing tag:

```powershell
gh workflow run release.yml -R Lerxtwood/orb-os --ref main -f "tag=$releaseTag"
```

That checks out the specified tag, not your latest untagged edits. For a source fix, commit it and publish a new version rather than moving an already-published tag.

The release manifest records the checked-out tag's commit, including for manual dispatch. If firmware publication succeeded but Pages failed, rerun the failed job or deploy `companion-pages.yml` as below; no replacement firmware tag is needed. Rebuilding an existing published tag can replace its assets, so use it only for an intentional rebuild of that same source.

## 10. Publish installer-only changes

Changes to the website, USB reset logic, or site generator do not require a new firmware tag. Run installer/release tests, commit and push the changes, then deploy:

```powershell
gh workflow run companion-pages.yml -R Lerxtwood/orb-os --ref main
gh run list -R Lerxtwood/orb-os --workflow companion-pages.yml --limit 5
$pagesRun = Read-Host 'Enter the new Pages workflow run ID'
gh run watch $pagesRun -R Lerxtwood/orb-os --exit-status --interval 30
```

The installer continues offering the existing firmware releases. Generated script and stylesheet filenames include content hashes to avoid mixing old browser assets with a newer page. Verify the live release dropdown and the controls you changed after deployment.
