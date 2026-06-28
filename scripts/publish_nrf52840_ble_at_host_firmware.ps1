param(
    [string]$BuildDir = "build_nrf52840_ble_at_host",
    [string]$OutputDir = "firmware\nrf52840_ble_at_host",
    [int]$HwVersion = 52,
    [string]$SdReq = "0x00",
    [int]$ApplicationVersion = 1
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$toolchainRoot = "C:\ncs\toolchains\936afb6332"
$nrfutilExe = Join-Path $toolchainRoot "nrfutil\home\bin\nrfutil.exe"
$hexPath = Join-Path $repoRoot "$BuildDir\zephyr\nrf52840_ble_at_host.hex"
$zipPath = Join-Path $repoRoot "$BuildDir\nrf52840_ble_at_host.zip"
$targetDir = Join-Path $repoRoot $OutputDir
$legacyTargetHexPath = Join-Path $targetDir "zephyr.hex"
$targetHexPath = Join-Path $targetDir "nrf52840_ble_at_host.hex"
$targetZipPath = Join-Path $targetDir "nrf52840_ble_at_host.zip"

if (-not (Test-Path $nrfutilExe)) {
    throw "nrfutil not found: $nrfutilExe"
}

if (-not (Test-Path $hexPath)) {
    throw "Application hex not found: $hexPath"
}

if (Test-Path $zipPath) {
    Remove-Item $zipPath -Force
}

Write-Host "Generating DFU package: $zipPath" -ForegroundColor Cyan
& $nrfutilExe nrf5sdk-tools pkg generate `
    --hw-version $HwVersion `
    --sd-req=$SdReq `
    --application $hexPath `
    --application-version $ApplicationVersion `
    $zipPath

New-Item -ItemType Directory -Path $targetDir -Force | Out-Null

if (Test-Path $legacyTargetHexPath) {
    Remove-Item -LiteralPath $legacyTargetHexPath -Force
}

Copy-Item -LiteralPath $hexPath -Destination $targetHexPath -Force
Copy-Item -LiteralPath $zipPath -Destination $targetZipPath -Force

Write-Host "Published firmware artifacts to $targetDir" -ForegroundColor Cyan
