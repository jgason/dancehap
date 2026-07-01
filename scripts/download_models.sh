#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
#
# download_models.sh — Télécharge les 7 modèles de segmentation ONNX Runtime (.ort)
# depuis la release obs-backgroundremoval 1.3.7.
#
# Usage: ./scripts/download_models.sh
# Les modèles sont placés dans data/models/ (pas committés dans git).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MODELS_DIR="$REPO_ROOT/data/models"
OBS_BGR_VERSION="1.3.7"
OBS_BGR_RELEASE="obs-backgroundremoval-${OBS_BGR_VERSION}-windows-x64.zip"
TEMP_DIR="${TMPDIR:-/tmp}/obs-bgr-models-$$"

MODELS=(
    "rvm_mobilenetv3_fp32.with_runtime_opt.ort"
    "mediapipe.with_runtime_opt.ort"
    "selfie_segmentation.with_runtime_opt.ort"
    "selfie_multiclass_256x256.with_runtime_opt.ort"
    "pphumanseg_fp32.with_runtime_opt.ort"
    "SINet_Softmax_simple.with_runtime_opt.ort"
    "tcmonodepth_tcsmallnet_192x320.with_runtime_opt.ort"
)

echo "[DanceHAP] Downloading segmentation models from obs-backgroundremoval ${OBS_BGR_VERSION}..."

mkdir -p "$MODELS_DIR" "$TEMP_DIR"

# Download the release zip
if ! command -v gh &>/dev/null; then
    echo "[ERROR] gh CLI not found. Install: https://cli.github.com/"
    exit 1
fi

gh release download "$OBS_BGR_VERSION" \
    -R royshil/obs-backgroundremoval \
    --dir "$TEMP_DIR" \
    --pattern "$OBS_BGR_RELEASE"

# Extract models
unzip -j "$TEMP_DIR/$OBS_BGR_RELEASE" \
    "obs-backgroundremoval/data/models/*.ort" \
    -d "$TEMP_DIR/extracted/"

# Copy the 7 models we need
for model in "${MODELS[@]}"; do
    if [ -f "$TEMP_DIR/extracted/$model" ]; then
        cp "$TEMP_DIR/extracted/$model" "$MODELS_DIR/"
        echo "  ✓ $model"
    else
        echo "  ✗ MISSING: $model"
    fi
done

# Cleanup
rm -rf "$TEMP_DIR"

echo "[DanceHAP] Done. Models in $MODELS_DIR/"
ls -lh "$MODELS_DIR"/*.ort