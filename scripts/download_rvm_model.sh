#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
#
# scripts/download_rvm_model.sh
#
# Download the Robust Video Matting ONNX model for Phase 2.2.
#
# Model: rvm_mobilenetv3_fp32.torchscript → ONNX export
# Source: https://github.com/PeterL1n/RobustVideoMatting
# License: MIT (model weights)
# Size: ~25 MB
#
# Usage:
#   bash scripts/download_rvm_model.sh [output_dir]
#
# Default output: models/rvm_mobilenetv3_fp32.onnx

set -euo pipefail

OUTPUT_DIR="${1:-models}"
MODEL_NAME="rvm_mobilenetv3_fp32.onnx"
OUTPUT_PATH="${OUTPUT_DIR}/${MODEL_NAME}"

# HuggingFace mirror of the RVM ONNX export (community-maintained).
# The official TorchScript weights are at
# https://github.com/PeterL1n/RobustVideoMatting/releases/download/v1.0.0/rvm_mobilenetv3.pth
# but we need ONNX. This mirror provides a pre-exported ONNX.
MODEL_URL="https://huggingface.co/onnx-community/rvm-mobilenetv3/resolve/main/rvm_mobilenetv3_fp32.onnx"

# Fallback: official TorchScript (requires manual ONNX export via Python).
TORCHSCRIPT_URL="https://github.com/PeterL1n/RobustVideoMatting/releases/download/v1.0.0/rvm_mobilenetv3.pth"

mkdir -p "${OUTPUT_DIR}"

if [[ -f "${OUTPUT_PATH}" ]]; then
    echo "Model already exists: ${OUTPUT_PATH}"
    echo "Size: $(du -h "${OUTPUT_PATH}" | cut -f1)"
    exit 0
fi

echo "Downloading RVM ONNX model..."
echo "  URL: ${MODEL_URL}"
echo "  Output: ${OUTPUT_PATH}"

if command -v curl &>/dev/null; then
    if curl -L -f -o "${OUTPUT_PATH}.tmp" "${MODEL_URL}" 2>/dev/null; then
        mv "${OUTPUT_PATH}.tmp" "${OUTPUT_PATH}"
        echo "OK: downloaded $(du -h "${OUTPUT_PATH}" | cut -f1)"
        exit 0
    fi
    echo "Falling back to TorchScript weights (requires manual ONNX export)..."
    curl -L -f -o "${OUTPUT_DIR}/rvm_mobilenetv3.pth" "${TORCHSCRIPT_URL}"
    echo ""
    echo "TorchScript weights downloaded to ${OUTPUT_DIR}/rvm_mobilenetv3.pth"
    echo "To export to ONNX, run:"
    echo "  python3 scripts/export_rvm_onnx.py ${OUTPUT_DIR}/rvm_mobilenetv3.pth ${OUTPUT_PATH}"
    exit 0
elif command -v wget &>/dev/null; then
    if wget -q -O "${OUTPUT_PATH}.tmp" "${MODEL_URL}" 2>/dev/null; then
        mv "${OUTPUT_PATH}.tmp" "${OUTPUT_PATH}"
        echo "OK: downloaded $(du -h "${OUTPUT_PATH}" | cut -f1)"
        exit 0
    fi
    echo "ERROR: wget download failed"
    exit 1
else
    echo "ERROR: neither curl nor wget available"
    exit 1
fi