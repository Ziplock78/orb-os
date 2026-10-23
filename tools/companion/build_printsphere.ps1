param(
    [string]$IdfPath = "C:\Users\chris\esp\esp-idf-v5.5.4",
    [string]$IdfPython = "C:\Users\chris\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe",
    [switch]$Clean
)
$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path "$PSScriptRoot\..\..").Path
$env:PATH = "$(Split-Path $IdfPython);$env:PATH"
. "$IdfPath\export.ps1"
if ($LASTEXITCODE -ne 0) { throw "ESP-IDF environment setup failed" }
if ($Clean) {
    # These are generated files in our isolated build copy, never the source repo.
    $workRoot = (Resolve-Path "$repoRoot\.pio\companion").Path
    $buildPath = [IO.Path]::GetFullPath("$workRoot\ps-build")
    if ([IO.Path]::GetDirectoryName($buildPath) -ne $workRoot) {
        throw "Refusing to clean outside the companion workspace"
    }
    if (Test-Path -LiteralPath $buildPath) {
        Remove-Item -LiteralPath $buildPath -Recurse -Force
    }
    foreach ($name in @('sdkconfig', 'sdkconfig.old')) {
        $configPath = Join-Path "$workRoot\PrintSphere" $name
        if (Test-Path -LiteralPath $configPath) {
            Remove-Item -LiteralPath $configPath -Force
        }
    }
}
& $IdfPython "$IdfPath\tools\idf.py" -C "$repoRoot\.pio\companion\PrintSphere" -B "$repoRoot\.pio\companion\ps-build" -D PRINTSPHERE_HW_VARIANT=amoled_1_75 -D PRINTSPHERE_RELEASE_VERSION=2.0.22-orbtest build
if ($LASTEXITCODE -ne 0) { throw "PrintSphere build failed" }
