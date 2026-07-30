// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// matte_engine.hpp — MatteEngine: ONNX Runtime-based matting engine.
//
// Phase 2.1: interface + types defined. Stub mode returns empty/false.
// Phase 2.2: real ONNX Runtime implementation (#ifdef DANCEHAP_HAVE_ONNXRUNTIME).
// Phase 2.6: multi-model support (7 segmentation models via Strategy pattern).
//
// The engine loads a matting model (one of 7 supported types) and runs
// inference on RGBA image frames, producing an alpha mask where
// 1.0 = person (opaque), 0.0 = background (transparent).
//
// Inference pipeline (generalized):
//   ImageFrame RGBA (8-bit)
//     → resize to model input dims (bilinear)
//     → preprocess per-model spec (layout BHWC/BCHW, normalize mode)
//     → ONNX Runtime infer (input/output names discovered dynamically)
//     → postprocess per-model spec (extract mask channel, MINMAX, argmax, etc.)
//     → resize back to original dimensions
//     → fill MatteMask.alpha (1=person, 0=background)

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
    int             width      = 0;
    int             height     = 0;
    const uint8_t  *data_rgba  = nullptr;  // owned by caller, not copied
};

/// Output mask: per-pixel alpha values (0.0 = transparent, 1.0 = opaque).
/// Convention: 1.0 = person opaque, 0.0 = background transparent.
/// This must match the shader expectation (mask_alpha_filter.effect).
struct MatteMask {
    int                 width  = 0;
    int                 height = 0;
    std::vector<float>  alpha;  // width * height elements, row-major
};

/// Model configuration: internal resolution for inference.
/// Smaller = faster but less precise. Default 256x256 (generic).
struct MatteModelConfig {
    int input_width  = 256;
    int input_height = 256;
};

/// Execution provider for ONNX Runtime inference.
/// ADR-003 (révisé 2026-06-24): CUDA retiré, DirectML = Windows, CoreML = macOS.
enum class ExecutionProvider {
    Auto,       ///< Best available: DirectML (Win) → CoreML (macOS) → CPU
    DirectML,   ///< Windows only (DirectX 12, all GPU vendors)
    CoreML,      ///< macOS only (Metal)
    CPU,        ///< Portable fallback (slow for RVM)
};

/// Detected provider after engine initialization.
enum class ActiveProvider {
    Unknown,
    DirectML,
    CoreML,
    CPU,
};

// ---------------------------------------------------------------------------
// Multi-model support (Phase 2.6)
// ---------------------------------------------------------------------------

/// Identifies which of the 7 supported segmentation models is in use.
enum class MatteModelType {
    RVM,                ///< Robust Video Matting (stateful, BCHW, ImageNet norm)
    MediaPipe,          ///< MediaPipe Selfie Segmentation (BHWC, /255, 2ch out)
    Selfie,             ///< Google Selfie Segmentation (BHWC, /255, 1ch + MINMAX)
    SelfieMulticlass,   ///< Selfie Multiclass (BHWC, /255, 6ch argmax)
    PPHumanSeg,         ///< PaddlePaddle Human Seg (BCHW, (x/256-0.5)/0.5, 2ch)
    SINet,              ///< SINet (BCHW, custom mean/std, 2ch)
    TCMonoDepth,        ///< TC MonoDepth (BCHW, NO normalize, depth→MINMAX)
};

/// Tensor data layout for model input.
enum class TensorLayout {
    BHWC,   ///< [1, H, W, C] — MediaPipe, Selfie, SelfieMulticlass
    BCHW,   ///< [1, C, H, W] — RVM, PPHumanSeg, SINet, TCMonoDepth
};

/// Preprocessing normalization mode.
enum class NormalizeMode {
    None,       ///< Raw [0,255] → [0,255] (TCMonoDepth)
    Divide255,  ///< [0,255] → [0,1] (MediaPipe, Selfie, SelfieMulticlass)
    ImageNet,   ///< (x/255 - mean) / std, mean=[.485,.456,.406] std=[.229,.224,.225] (RVM)
    PPHumanSeg, ///< (x/256 - 0.5) / 0.5 (PPHumanSeg)
    SINet,      ///< (x - mean) / (std*255), mean=[102.89,111.25,126.91] std=[62.93,62.82,66.36]
};

/// Postprocessing mode for converting raw model output to a single-channel mask.
enum class PostprocessMode {
    Direct,     ///< Output is already [0,1] single-channel (RVM pha)
    Channel1,   ///< Take channel index 1 from 2-channel output (MediaPipe, PPHumanSeg, SINet)
    MinMax,     ///< NORM_MINMAX to [0,1] on single channel (Selfie, TCMonoDepth)
    Channel1MinMax, ///< Take channel 1 + NORM_MINMAX (PPHumanSeg)
    ArgmaxMulticlass, ///< Argmax across 6 channels, foreground = class>0 (SelfieMulticlass)
};

/// Complete specification for a segmentation model.
/// All fields are testable in stub mode (no ONNX Runtime needed).
struct MatteModelSpec {
    MatteModelType    type                    = MatteModelType::RVM;
    std::string       name;                  ///< "rvm", "mediapipe", ...
    std::string       display_name;          ///< "Robust Video Matting"
    std::string       default_filename;      ///< "rvm_mobilenetv3_fp32.onnx"
    bool              is_stateful            = false;  ///< RVM = true (recurrent states)
    int               default_width          = 256;
    int               default_height         = 256;
    TensorLayout      layout                 = TensorLayout::BCHW;
    NormalizeMode     normalize              = NormalizeMode::Divide255;
    PostprocessMode   postprocess             = PostprocessMode::Direct;
    int               output_channels        = 1;  ///< 1, 2, or 6
};

/// Get the spec for a given model type. Returns a fully-populated MatteModelSpec.
const MatteModelSpec &getModelSpec(MatteModelType type);

/// Resolve a model type from a filename (e.g., "rvm_mobilenetv3_fp32.onnx" → RVM).
/// Falls back to RVM if the filename doesn't match any known model.
MatteModelType resolveModelType(const std::string &filename);

/// Get the list of all 7 model specs (for UI population, iteration, etc.).
const std::vector<MatteModelSpec> &getAllModelSpecs();

// ---------------------------------------------------------------------------
// Helpers (testable without ONNX Runtime)
// ---------------------------------------------------------------------------

/// Preprocess RGBA 8-bit image into RGB float32 tensor, normalized.
/// Output layout: CHW (channel-first), size = 3 * out_w * out_h.
/// Uses simple bilinear resize when source != out dimensions.
/// mean=[0.485,0.456,0.406], std=[0.229,0.224,0.225] (ImageNet standard).
/// NOTE: This is the original RVM-specific helper, kept for backward compat.
std::vector<float> preprocess_rgba_to_rgb_normalized(
    const ImageFrame &input,
    int out_w, int out_h);

/// Preprocess RGBA 8-bit image into a float32 tensor according to a model spec.
/// Handles layout (BHWC/BCHW), normalization mode, and bilinear resize.
/// Returns a vector of size = 3 * out_w * out_h (for BHWC it's interleaved HWC).
std::vector<float> preprocess_rgba_for_model(
    const ImageFrame &input,
    int out_w, int out_h,
    TensorLayout layout,
    NormalizeMode norm);

/// Postprocess: extract alpha channel from a "pha" tensor [1,1,H,W] float32.
/// Produces a MatteMask at the given output dimensions (resized via bilinear).
MatteMask postprocess_pha_to_mask(
    const float *pha_data, int pha_w, int pha_h,
    int out_w, int out_h);

/// Postprocess model output into a single-channel mask [0,1] where 1=person.
/// Handles multi-channel extraction, NORM_MINMAX, argmax multiclass.
/// Output mask is resized to (out_w, out_h) via bilinear interpolation.
MatteMask postprocess_output_to_mask(
    const float *output_data,
    int out_w, int out_h,
    int num_channels,
    PostprocessMode mode);

/// NORM_MINMAX normalization: rescales values to [0,1].
/// Used by Selfie, PPHumanSeg, TCMonoDepth postprocessing.
std::vector<float> normalize_minmax(const float *data, size_t count);

/// Argmax across C channels for each pixel: foreground=1 if class>0, else 0.
/// output has H*W elements. Used by SelfieMulticlass.
std::vector<float> argmax_multiclass_to_mask(
    const float *data, int h, int w, int num_classes);

/// Resolve the actual active provider given a desired provider and the platform.
/// ADR-003: DirectML on Windows, CoreML on macOS, CPU elsewhere.
/// Pure logic — testable without ONNX Runtime.
///   - ExecutionProvider::Auto → platform default (DirectML/CoreML)
///   - DirectML requested on non-Windows → CPU (not available)
///   - CoreML requested on non-macOS → CPU (not available)
ActiveProvider resolve_provider(ExecutionProvider desired);

// ---------------------------------------------------------------------------
// MatteModelBackend (Strategy interface — Phase 2.6)
// ---------------------------------------------------------------------------

#ifdef DANCEHAP_HAVE_ONNXRUNTIME

/// Forward declaration of the ONNX Runtime implementation details.
struct MatteEngineImpl;

/// Abstract strategy interface for model-specific inference.
/// Each of the 7 model types has a concrete backend implementation.
/// The backend handles preprocessing, tensor creation, inference dispatch,
/// and postprocessing for its specific model architecture.
class MatteModelBackend {
public:
    virtual ~MatteModelBackend() = default;

    /// Run inference on the input frame, producing a mask.
    /// Returns an empty mask on error.
    /// The backend has access to the engine's Impl (session, ORT API, etc.).
    virtual MatteMask infer(MatteEngineImpl &impl, const ImageFrame &input) = 0;

    /// Get the model spec for this backend.
    virtual const MatteModelSpec &spec() const = 0;
};

/// Factory: create a backend for the given model type.
/// Returns nullptr if the type is unknown.
std::unique_ptr<MatteModelBackend> createBackend(MatteModelType type);

#endif // DANCEHAP_HAVE_ONNXRUNTIME

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
    /// The model type is auto-detected from the filename, or can be set
    /// explicitly via setModelType() before calling loadModel().
    /// Returns false if the model cannot be loaded.
    bool loadModel(const std::string &model_path);

    /// Whether the engine is ready to run inference.
    bool isReady() const;

    /// Run inference on an input frame, producing an alpha mask.
    /// Returns an empty mask (width=0) on error or if not ready.
    MatteMask infer(const ImageFrame &input);

    /// Get/set the model type. If set before loadModel(), the engine
    /// will use the spec's default dimensions and create the right backend.
    MatteModelType modelType() const;
    void setModelType(MatteModelType type);

    /// Get the model spec currently in use.
    const MatteModelSpec &modelSpec() const;

    /// Get/set the internal model resolution.
    const MatteModelConfig &config() const { return config_; }
    void setConfig(const MatteModelConfig &c) { config_ = c; }

    /// Get/set the desired execution provider.
    ExecutionProvider desiredProvider() const { return provider_; }
    void setDesiredProvider(ExecutionProvider p) { provider_ = p; }

    /// Get the active provider (after loadModel). Unknown until model loaded.
    ActiveProvider activeProvider() const;

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
    MatteModelConfig      config_;
    ExecutionProvider     provider_ = ExecutionProvider::Auto;
    MatteModelType        model_type_ = MatteModelType::RVM;
};

} // namespace dancehap