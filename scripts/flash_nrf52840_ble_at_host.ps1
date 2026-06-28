param(
    [string]$Board = "nrf52840dongle/nrf52840",
    [string]$BuildDir = "build_nrf52840_ble_at_host",
    [string]$Port = "COM11",
    [int]$BaudRate = 115200,
    [int]$HwVersion = 52,
    [string]$SdReq = "0x00",
    [int]$ApplicationVersion = 1,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$toolchainRoot = "C:\ncs\toolchains\936afb6332"
$nrfutilExe = Join-Path $toolchainRoot "nrfutil\home\bin\nrfutil.exe"
$buildScript = Join-Path $PSScriptRoot "build_nrf52840_ble_at_host.ps1"
$hexPath = Join-Path $repoRoot "$BuildDir\zephyr\nrf52840_ble_at_host.hex"
$zipPath = Join-Path $repoRoot "$BuildDir\nrf52840_ble_at_host.zip"

if (-not (Test-Path $nrfutilExe)) {
    throw "nrfutil not found: $nrfutilExe"
}

if (-not $SkipBuild) {
    & $buildScript -Board $Board -BuildDir $BuildDir
}

if (-not (Test-Path $hexPath)) {
    throw "Application hex not found: $hexPath"
}

if (Test-Path $zipPath) {
    Remove-Item $zipPath
}

Write-Host "Generating DFU package: $zipPath" -ForegroundColor Cyan
& $nrfutilExe nrf5sdk-tools pkg generate `
    --hw-version $HwVersion `
    --sd-req=$SdReq `
    --application $hexPath `
    --application-version $ApplicationVersion `
    $zipPath

Write-Host "Flashing DFU package to $Port" -ForegroundColor Cyan
& $nrfutilExe nrf5sdk-tools dfu usb-serial `
    -pkg $zipPath `
    -p $Port `
    -b $BaudRate
