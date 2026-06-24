// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// matte_engine.hpp — MatteEngine: ONNX Runtime-based matting engine.
//
// Phase 2.1: interface + types defined. Stub mode returns empty/false.
// Phase 2.2: real ONNX Runtime implementation (#ifdef DANCEHAP_HAVE_ONNXRUNTIME).
//
// The engine loads a matting model (RVM ONNX) and runs inference on
// RGBA image frames, producing an alpha mask.
//
// Inference pipeline:
//   ImageFrame RGBA (8-bit)
//     → resize to model input (default 256x256, bilinear)
//     → convert RGBA→RGB + normalize (mean=[0.485,0.456,0.406], std=[0.229,0.224,0.225])
//     → ONNX Runtime infer (input "src" [1,3,H,W] float32)
//     → extract "pha" output [1,1,H,W] float32 (alpha 0-1)
//     → resize back to original dimensions
//     → fill MatteMask.alpha

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dancehap {

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

/// Input frame: RGBA image data (8-bit per channel, interleaved).
struct ImageFrame {
    int         width       = 0;
    int         height      = 0;
    const uint8_t *data_rgba = nullptr;  // owned by caller, not copied
};

/// Output mask: per-pixel alpha values (0.0 = transparent, 1.0 = opaque).
struct MatteMask {
    int                 width  = 0;
    int                 height = 0;
    std::vector<float>  alpha;  // width * height elements, row-major
};

/// Model configuration: internal resolution for inference.
/// Smaller = faster but less precise. Default 256x256 (RVM MobileNetV3).
struct MatteModelConfig {
    int input_width  = 256;
    int input_height = 256;
};

// ---------------------------------------------------------------------------
// Helpers (testable without ONNX Runtime)
// ---------------------------------------------------------------------------

/// Preprocess RGBA 8-bit image into RGB float32 tensor, normalized.
/// Output layout: CHW (channel-first), size = 3 * out_w * out_h.
/// Uses simple bilinear resize when source != out dimensions.
/// mean=[0.485,0.456,0.406], std=[0.229,0.224,0.225] (ImageNet standard).
std::vector<float> preprocess_rgba_to_rgb_normalized(
    const ImageFrame &input,
    int out_w, int out_h);

/// Postprocess: extract alpha channel from a "pha" tensor [1,1,H,W] float32.
/// Produces a MatteMask at the given output dimensions (resized via bilinear).
MatteMask postprocess_pha_to_mask(
    const float *pha_data, int pha_w, int pha_h,
    int out_w, int out_h);

// ---------------------------------------------------------------------------
// MatteEngine
// ---------------------------------------------------------------------------

class MatteEngine {
public:
    MatteEngine();
    ~MatteEngine();

    MatteEngine(const MatteEngine &) = delete;
    MatteEngine &operator=(const MatteEngine &) = delete;

    MatteEngine(MatteEngine &&) noexcept;
    MatteEngine &operator=(MatteEngine &&) noexcept;

    /// Load a matting model from the given file path.
    /// Returns false if the model cannot be loaded.
    bool loadModel(const std::string &model_path);

    /// Whether the engine is ready to run inference.
    bool isReady() const;

    /// Run inference on an input frame, producing an alpha mask.
    /// Returns an empty mask (width=0) on error or if not ready.
    MatteMask infer(const ImageFrame &input);

    /// Get/set the internal model resolution.
    const MatteModelConfig &config() const { return config_; }
    void setConfig(const MatteModelConfig &c) { config_ = c; }

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
    MatteModelConfig config_;
};

} // namespace dancehap