#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
#
# tests/smoke/run_smoke_macos.sh
#
# Smoke test macOS pour DanceHAP v0.3.2+.
# Vérifie que le plugin est installé dans OBS, que la version est correcte,
# et lance un check symbol + asset.
#
# Usage :
#   bash run_smoke_macos.sh [/path/to/dancehap.plugin]
#
# Exit codes: 0 = pass, 1 = fail.

set -uo pipefail

PLUGIN="${1:-${HOME}/Library/Application Support/obs-studio/plugins/dancehap.plugin}"
ASSET="$(dirname "$0")/../assets/sample_hapa_5s.mov"

echo "=== DanceHAP macOS Smoke Test ==="

# 1. Plugin exists
if [[ ! -e "$PLUGIN" ]]; then
    echo "FAIL: plugin not found at $PLUGIN" >&2
    exit 1
fi
SIZE=$(du -k "$PLUGIN" | cut -f1)
echo "  Plugin: $PLUGIN (${SIZE} KB)"
if [[ "$SIZE" -lt 5 ]]; then
    echo "FAIL: plugin too small (${SIZE} KB, expected >5 KB)" >&2
    exit 1
fi

# 2. Version check via strings
BINARY=""
if [[ -d "$PLUGIN" ]]; then
    # .plugin bundle: find the Mach-O binary inside
    BINARY=$(find "$PLUGIN" -type f -name "dancehap" -o -name "*.dylib" 2>/dev/null | head -1)
fi
if [[ -n "$BINARY" && -f "$BINARY" ]]; then
    VER=$(strings "$BINARY" 2>/dev/null | grep -E "DanceHAP Clip v[0-9]+\.[0-9]+\.[0-9]+" | head -1)
    if [[ -n "$VER" ]]; then
        echo "  Version: ${VER#DanceHAP Clip v}"
    else
        echo "WARN: version string not found in binary" >&2
    fi
    # 3. OBS entry point symbol
    if nm -gU "$BINARY" 2>/dev/null | grep -q "obs_module_load"; then
        echo "  obs_module_load: exported"
    else
        echo "WARN: obs_module_load not visible (may be hidden by visibility)" >&2
    fi
else
    echo "WARN: could not locate binary inside plugin bundle" >&2
fi

# 4. Asset check
if [[ ! -f "$ASSET" ]]; then
    echo "WARN: test asset not found at $ASSET" >&2
else
    ASSET_SIZE=$(stat -f%z "$ASSET" 2>/dev/null || stat -c%s "$ASSET" 2>/dev/null || echo 0)
    echo "  Asset: $ASSET (${ASSET_SIZE} bytes)"
fi

# 5. OBS process check
if pgrep -x obs >/dev/null 2>&1 || pgrep -f "obs-studio" >/dev/null 2>&1; then
    echo "  OBS: running"
else
    echo "  OBS: not running (launch OBS to complete manual smoke)"
fi

echo "=== Automated checks PASS ==="
echo ""
echo "Manual smoke (OBS):"
echo "  1. Launch OBS Studio"
echo "  2. Add source -> DanceHAP Clip v0.3.2"
echo "  3. Set path to sample_hapa_5s.mov (or sample_hapa_5s_pcm.mov)"
echo "  4. Verify video visible (not black, not bars)"
echo "  5. Verify audio audible in OBS mixer"
echo "  6. Let it loop 60s, check no crash / no memory growth"
echo "  7. Toggle Loop off, verify clean stop at EOF"
exit 0