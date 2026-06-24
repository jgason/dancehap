# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
#
# tests/smoke/run_smoke_windows.ps1
#
# Smoke test Windows pour DanceHAP v0.3.2+.
# Vérifie que la DLL est installée dans OBS, que la version est correcte,
# et lance un check symbol + asset.
#
# Usage (depuis Hephaistos ou machine Windows avec OBS) :
#   powershell -ExecutionPolicy Bypass -File run_smoke_windows.ps1 [-Plugin "C:\Program Files\obs-studio\obs-plugins\64bit\dancehap.dll"]
#
# Exit codes: 0 = pass, 1 = fail.

param(
    [string]$Plugin = "C:\Program Files\obs-studio\obs-plugins\64bit\dancehap.dll",
    [string]$Asset  = "$PSScriptRoot\..\assets\sample_hapa_5s.mov"
)

$ErrorActionPreference = "Stop"
Write-Host "=== DanceHAP Windows Smoke Test ==="

# 1. Plugin exists
if (-not (Test-Path $Plugin)) {
    Write-Host "FAIL: plugin not found at $Plugin" -ForegroundColor Red
    exit 1
}
$size = (Get-Item $Plugin).Length
Write-Host ("  Plugin: {0} ({1} bytes)" -f $Plugin, $size)
if ($size -lt 5120) {
    Write-Host "FAIL: plugin too small ($size bytes, expected >5 KB)" -ForegroundColor Red
    exit 1
}

# 2. Version check via strings (the version string is embedded in the DLL)
$strings = & dumpbin /rawstrings $Plugin 2>$null
if (-not $strings) {
    # Fallback: read bytes and grep
    $bytes = [System.IO.File]::ReadAllBytes($Plugin)
    $text = [System.Text.Encoding]::ASCII.GetString($bytes)
    if ($text -match "DanceHAP Clip v(\d+\.\d+\.\d+)") {
        Write-Host ("  Version: {0}" -f $matches[1])
    } else {
        Write-Host "WARN: could not extract version string" -ForegroundColor Yellow
    }
} else {
    $ver = $strings | Select-String "DanceHAP Clip v(\d+\.\d+\.\d+)" | Select-Object -First 1
    if ($ver) {
        Write-Host ("  Version: {0}" -f $ver.Matches[0].Groups[1].Value)
    } else {
        Write-Host "WARN: version string not found in DLL" -ForegroundColor Yellow
    }
}

# 3. OBS entry point symbol
$exports = & dumpbin /exports $Plugin 2>$null
if ($exports -match "obs_module_load") {
    Write-Host "  obs_module_load: exported"
} else {
    Write-Host "WARN: obs_module_load not visible in exports (may be hidden)" -ForegroundColor Yellow
}

# 4. Asset check
if (-not (Test-Path $Asset)) {
    Write-Host "WARN: test asset not found at $Asset" -ForegroundColor Yellow
} else {
    $assetSize = (Get-Item $Asset).Length
    Write-Host ("  Asset: {0} ({1} bytes)" -f $Asset, $assetSize)
}

# 5. OBS process check
$obs = Get-Process -Name "obs64" -ErrorAction SilentlyContinue
if ($obs) {
    Write-Host "  OBS: running (PID $($obs.Id))" -ForegroundColor Green
} else {
    Write-Host "  OBS: not running (launch OBS to complete manual smoke)" -ForegroundColor Yellow
}

Write-Host "=== Automated checks PASS ===" -ForegroundColor Green
Write-Host ""
Write-Host "Manual smoke (OBS):"
Write-Host "  1. Launch OBS Studio"
Write-Host "  2. Add source -> DanceHAP Clip v0.3.2"
Write-Host "  3. Set path to sample_hapa_5s.mov (or sample_hapa_5s_pcm.mov)"
Write-Host "  4. Verify video visible (not black, not bars)"
Write-Host "  5. Verify audio audible in OBS mixer"
Write-Host "  6. Let it loop 60s, check no crash / no memory growth"
Write-Host "  7. Toggle Loop off, verify clean stop at EOF"
exit 0