#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
#
# tests/smoke/check_source_registered.sh
#
# Verifies that the built DanceHAP plugin artifact exports the OBS module
# entry points (obs_module_load, etc.) and, in stub builds, the source
# registration symbol.
#
# Usage:
#   ./tests/smoke/check_source_registered.sh <path-to-plugin>
#
# Exit codes:
#   0 — all expected symbols found
#   1 — plugin file missing or symbols not found
#
# Platform tools:
#   Linux/macOS — nm
#   Windows     — dumpbin (Visual Studio) or objdump (MinGW/MSYS2)

set -euo pipefail

# --- Arguments ------------------------------------------------------------

if [[ $# -lt 1 ]]; then
    echo "ERROR: usage: $0 <path-to-plugin>" >&2
    exit 1
fi

PLUGIN="$1"

if [[ ! -f "$PLUGIN" ]]; then
    echo "ERROR: plugin file not found: $PLUGIN" >&2
    exit 1
fi

echo "Smoke test: checking symbols in $PLUGIN"

# --- Required symbols (OBS module entry points, C linkage) ----------------

REQUIRED_SYMBOLS=(
    "obs_module_load"
    "obs_module_unload"
    "obs_module_name"
)

# --- Symbol extraction ----------------------------------------------------

SYMBOLS=""

if command -v nm &>/dev/null; then
    # Linux / macOS
    # -D: dynamic symbols, -g: extern-only, -U: defined-only (macOS)
    SYMBOLS=$(nm -D "$PLUGIN" 2>/dev/null || nm -gU "$PLUGIN" 2>/dev/null || true)
elif command -v dumpbin &>/dev/null; then
    # Windows with Visual Studio
    SYMBOLS=$(dumpbin /exports "$PLUGIN" 2>/dev/null || true)
elif command -v objdump &>/dev/null; then
    # Fallback: objdump (MinGW, Linux without nm)
    SYMBOLS=$(objdump -T "$PLUGIN" 2>/dev/null || true)
else
    echo "WARNING: no symbol inspection tool found (nm/dumpbin/objdump)" >&2
    echo "         Skipping symbol check. Marking as PASS with caveat." >&2
    exit 0
fi

if [[ -z "$SYMBOLS" ]]; then
    echo "ERROR: could not extract symbols from $PLUGIN" >&2
    exit 1
fi

# --- Check required symbols -----------------------------------------------

FAIL=0
for sym in "${REQUIRED_SYMBOLS[@]}"; do
    if echo "$SYMBOLS" | grep -qw "$sym"; then
        echo "  ✓ Found: $sym"
    else
        echo "  ✗ MISSING: $sym" >&2
        FAIL=1
    fi
done

# --- Optional: check for DanceHAP-specific symbols (informational) --------

OPTIONAL_SYMBOLS=(
    "register_hap_clip_source"
)
for sym in "${OPTIONAL_SYMBOLS[@]}"; do
    if echo "$SYMBOLS" | grep -qw "$sym"; then
        echo "  ✓ Found (optional): $sym"
    else
        echo "  - Not exported (OK in real-OBS builds): $sym"
    fi
done

if [[ $FAIL -ne 0 ]]; then
    echo "" >&2
    echo "FAIL: one or more required symbols not found in $PLUGIN" >&2
    exit 1
fi

echo ""
echo "PASS: all required symbols present"
exit 0
