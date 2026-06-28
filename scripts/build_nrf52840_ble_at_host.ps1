param(
    [string]$Board = "nrf52840dongle/nrf52840",
    [string]$BuildDir = "build_nrf52840_ble_at_host",
    [switch]$Pristine
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$appDir = Join-Path $repoRoot "apps\nrf52840_ble_at_host"
$toolchainRoot = "C:\ncs\toolchains\936afb6332"
$sdkRoot = "C:\ncs\v3.3.0"
$pythonExe = Join-Path $toolchainRoot "opt\bin\python.exe"

if (-not (Test-Path $pythonExe)) {
    throw "NCS toolchain Python not found: $pythonExe"
}

$env:ZEPHYR_BASE = Join-Path $sdkRoot "zephyr"
$env:PATH = (Join-Path $toolchainRoot "opt\bin") + ";" + $env:PATH
$legacyHexPath = Join-Path $repoRoot "$BuildDir\zephyr\zephyr.hex"
$namedHexPath = Join-Path $repoRoot "$BuildDir\zephyr\nrf52840_ble_at_host.hex"

$westArgs = @(
    "-m", "west", "build",
    "--no-sysbuild",
    "-b", $Board,
    "-d", $BuildDir,
    $appDir
)

if ($Pristine) {
    $westArgs += "--pristine"
}

Write-Host "Building nRF52840 BLE AT Host for $Board into $BuildDir" -ForegroundColor Cyan
& $pythonExe @westArgs

if (Test-Path $legacyHexPath) {
    Move-Item -LiteralPath $legacyHexPath -Destination $namedHexPath -Force
} elseif (-not (Test-Path $namedHexPath)) {
    throw "Named application hex not found: $namedHexPath"
}
