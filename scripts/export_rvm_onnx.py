#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
#
# scripts/export_rvm_onnx.py
#
# Export Robust Video Matting to ONNX format.
#
# Loads the RVM model architecture (from the official repo) + state_dict weights,
# wraps it for static (single-frame) inference, and exports to ONNX.
#
# Usage:
#   python3 scripts/export_rvm_onnx.py [input.pth] [output.onnx]
#
# Default: models/rvm_mobilenetv3.pth → models/rvm_mobilenetv3_fp32.onnx
#
# Requires: torch, torchvision, onnx (install via venv).

import sys
import os

try:
    import torch
    from torch import nn
except ImportError:
    print("ERROR: torch not installed.", file=sys.stderr)
    sys.exit(1)

INPUT = sys.argv[1] if len(sys.argv) > 1 else "models/rvm_mobilenetv3.pth"
OUTPUT = sys.argv[2] if len(sys.argv) > 2 else "models/rvm_mobilenetv3_fp32.onnx"


def export():
    if not os.path.exists(INPUT):
        print(f"ERROR: input not found: {INPUT}", file=sys.stderr)
        sys.exit(1)

    # Clone RVM repo to get architecture definition.
    rvm_repo = "/tmp/rvm"
    if not os.path.isdir(rvm_repo + "/model"):
        print("Cloning RVM repo for architecture...")
        os.system(f"git clone --depth 1 https://github.com/PeterL1n/RobustVideoMatting.git {rvm_repo}")

    sys.path.insert(0, rvm_repo)
    from model.model import MattingNetwork

    print(f"Loading model: {INPUT}")
    model = MattingNetwork(variant='mobilenetv3')
    state = torch.load(INPUT, map_location='cpu', weights_only=False)
    if isinstance(state, dict) and 'model' in state:
        state = state['model']
    model.load_state_dict(state, strict=False)
    model.eval()

    # Determine recurrent state sizes for static mode (zeros).
    src = torch.rand(1, 3, 256, 256, dtype=torch.float32)
    with torch.no_grad():
        out = model(src)
        fgr, pha, r1, r2, r3, r4 = out

    print(f"  fgr: {fgr.shape}, pha: {pha.shape}")
    print(f"  r1: {r1.shape}, r2: {r2.shape}, r3: {r3.shape}, r4: {r4.shape}")

    r1z = torch.zeros_like(r1)
    r2z = torch.zeros_like(r2)
    r3z = torch.zeros_like(r3)
    r4z = torch.zeros_like(r4)

    # Static wrapper: src → (fgr, pha), recurrent states fixed at zero.
    class RVMStatic(nn.Module):
        def __init__(self, rvm, r1, r2, r3, r4):
            super().__init__()
            self.rvm = rvm
            self.r1 = r1
            self.r2 = r2
            self.r3 = r3
            self.r4 = r4

        def forward(self, src):
            out = self.rvm(src, self.r1, self.r2, self.r3, self.r4)
            return out[0], out[1]

    static_model = RVMStatic(model, r1z, r2z, r3z, r4z).eval()

    print(f"Exporting ONNX: {OUTPUT}")
    torch.onnx.export(
        static_model,
        (src,),
        OUTPUT,
        export_params=True,
        opset_version=14,
        do_constant_folding=True,
        input_names=["src"],
        output_names=["fgr", "pha"],
        dynamic_axes={
            "src": {0: "batch", 2: "height", 3: "width"},
            "fgr": {0: "batch", 2: "height", 3: "width"},
            "pha": {0: "batch", 2: "height", 3: "width"},
        },
        dynamo=False,
    )
    size_mb = os.path.getsize(OUTPUT) / (1024 * 1024)
    print(f"OK: {OUTPUT} ({size_mb:.1f} MB)")


if __name__ == "__main__":
    export()