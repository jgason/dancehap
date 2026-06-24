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

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace dancehap