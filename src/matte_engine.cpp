// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// matte_engine.cpp — MatteEngine implementation.
//
// Phase 2.2: real ONNX Runtime implementation + shared helpers.
// Phase 2.6: multi-model support (7 models via Strategy pattern).
// Stub mode returns false/empty (no ONNX Runtime linked).

#include "matte_engine.hpp"
#include "obs_compat.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace dancehap {

// ===========================================================================
// Shared helpers (compile in both stub and ONNX modes)
// ===========================================================================

// ImageNet normalization constants (RVM backbone = MobileNetV3).
static constexpr float kMean[3] = {0.485f, 0.456f, 0.406f};
static constexpr float kStd[3]  = {0.229f, 0.224f, 0.225f};

// SINet-specific normalization constants.
static constexpr float kSINetMean[3] = {102.890434f, 111.25247f, 126.91212f};
static constexpr float kSINetStd[3]  = {62.93292f, 62.82138f, 66.355705f};

/// Bilinear sample of a single channel from RGBA data at (fx, fy).
static inline float sample_rgba_channel_bilinear(
    const uint8_t *data, int w, int h, float fx, float fy, int ch)
{
    fx = std::max(0.0f, std::min(static_cast<float>(w - 1), fx));
    fy = std::max(0.0f, std::min(static_cast<float>(h - 1), fy));

    int x0 = static_cast<int>(fx);
    int y0 = static_cast<int>(fy);
    int x1 = std::min(x0 + 1, w - 1);
    int y1 = std::min(y0 + 1, h - 1);

    float tx = fx - static_cast<float>(x0);
    float ty = fy - static_cast<float>(y0);

    const uint8_t *p00 = data + (y0 * w + x0) * 4 + ch;
    const uint8_t *p01 = data + (y0 * w + x1) * 4 + ch;
    const uint8_t *p10 = data + (y1 * w + x0) * 4 + ch;
    const uint8_t *p11 = data + (y1 * w + x1) * 4 + ch;

    float v0 = static_cast<float>(*p00) * (1.0f - tx) +
               static_cast<float>(*p01) * tx;
    float v1 = static_cast<float>(*p10) * (1.0f - tx) +
               static_cast<float>(*p11) * tx;
    return v0 * (1.0f - ty) + v1 * ty;
}

// ---------------------------------------------------------------------------
// Original RVM-specific helper (backward compat, used by existing tests)
// ---------------------------------------------------------------------------

std::vector<float> preprocess_rgba_to_rgb_normalized(
    const ImageFrame &input, int out_w, int out_h)
{
    return preprocess_rgba_for_model(input, out_w, out_h,
        TensorLayout::BCHW, NormalizeMode::ImageNet);
}

// ---------------------------------------------------------------------------
// Generalized preprocessing (Phase 2.6)
// ---------------------------------------------------------------------------

/// Apply normalization to a single channel value [0,255].
static inline float normalize_pixel(float val, NormalizeMode mode, int channel)
{
    switch (mode) {
        case NormalizeMode::None:
            return val;  // raw [0,255]
        case NormalizeMode::Divide255:
            return val / 255.0f;
        case NormalizeMode::ImageNet:
            return (val / 255.0f - kMean[channel]) / kStd[channel];
        case NormalizeMode::PPHumanSeg:
            return (val / 256.0f - 0.5f) / 0.5f;
        case NormalizeMode::SINet:
            return (val - kSINetMean[channel]) / (kSINetStd[channel] * 255.0f);
        default:
            return val / 255.0f;
    }
}

std::vector<float> preprocess_rgba_for_model(
    const ImageFrame &input,
    int out_w, int out_h,
    TensorLayout layout,
    NormalizeMode norm)
{
    std::vector<float> out;
    if (!input.data_rgba || input.width <= 0 || input.height <= 0) return out;
    if (out_w <= 0 || out_h <= 0) return out;

    size_t total = static_cast<size_t>(3) * out_w * out_h;
    out.resize(total);

    const float sx = static_cast<float>(input.width) / static_cast<float>(out_w);
    const float sy = static_cast<float>(input.height) / static_cast<float>(out_h);

    // Étape 1 : remplir un buffer HWC intermédiaire normalisé
    std::vector<float> hwc(total);
    for (int oy = 0; oy < out_h; ++oy) {
        for (int ox = 0; ox < out_w; ++ox) {
            float fx = (static_cast<float>(ox) + 0.5f) * sx - 0.5f;
            float fy = (static_cast<float>(oy) + 0.5f) * sy - 0.5f;

            float r = sample_rgba_channel_bilinear(input.data_rgba,
                input.width, input.height, fx, fy, 0);
            float g = sample_rgba_channel_bilinear(input.data_rgba,
                input.width, input.height, fx, fy, 1);
            float b = sample_rgba_channel_bilinear(input.data_rgba,
                input.width, input.height, fx, fy, 2);

            size_t hwc_idx = (static_cast<size_t>(oy) * out_w + ox) * 3;
            hwc[hwc_idx + 0] = normalize_pixel(r, norm, 0);
            hwc[hwc_idx + 1] = normalize_pixel(g, norm, 1);
            hwc[hwc_idx + 2] = normalize_pixel(b, norm, 2);
        }
    }

    // Étape 2 : convertir au layout demandé
    if (layout == TensorLayout::BHWC) {
        // BHWC = [1, H, W, C] = interleaved HWC (déjà le format intermédiaire)
        out = std::move(hwc);
    } else {
        // BCHW = [1, C, H, W] = planar CHW
        size_t plane = static_cast<size_t>(out_w) * out_h;
        for (int c = 0; c < 3; ++c) {
            for (int oy = 0; oy < out_h; ++oy) {
                for (int ox = 0; ox < out_w; ++ox) {
                    size_t hwc_idx = (static_cast<size_t>(oy) * out_w + ox) * 3 + c;
                    size_t chw_idx = static_cast<size_t>(c) * plane
                                   + static_cast<size_t>(oy) * out_w + ox;
                    out[chw_idx] = hwc[hwc_idx];
                }
            }
        }
    }

    return out;
}

// ---------------------------------------------------------------------------
// Bilinear resize helpers for float data
// ---------------------------------------------------------------------------

static inline float sample_float_bilinear(
    const float *data, int w, int h, float fx, float fy)
{
    fx = std::max(0.0f, std::min(static_cast<float>(w - 1), fx));
    fy = std::max(0.0f, std::min(static_cast<float>(h - 1), fy));

    int x0 = static_cast<int>(fx);
    int y0 = static_cast<int>(fy);
    int x1 = std::min(x0 + 1, w - 1);
    int y1 = std::min(y0 + 1, h - 1);

    float tx = fx - static_cast<float>(x0);
    float ty = fy - static_cast<float>(y0);

    float v00 = data[y0 * w + x0];
    float v01 = data[y0 * w + x1];
    float v10 = data[y1 * w + x0];
    float v11 = data[y1 * w + x1];

    float v0 = v00 * (1.0f - tx) + v01 * tx;
    float v1 = v10 * (1.0f - tx) + v11 * tx;
    return v0 * (1.0f - ty) + v1 * ty;
}

/// Resize a single-channel float buffer (w×h) to (out_w×out_h) via bilinear.
static std::vector<float> resize_float_bilinear(
    const float *data, int w, int h, int out_w, int out_h)
{
    std::vector<float> out(static_cast<size_t>(out_w) * out_h);
    float sx = static_cast<float>(w) / static_cast<float>(out_w);
    float sy = static_cast<float>(h) / static_cast<float>(out_h);

    for (int oy = 0; oy < out_h; ++oy) {
        for (int ox = 0; ox < out_w; ++ox) {
            float fx = (static_cast<float>(ox) + 0.5f) * sx - 0.5f;
            float fy = (static_cast<float>(oy) + 0.5f) * sy - 0.5f;
            float v = sample_float_bilinear(data, w, h, fx, fy);
            out[static_cast<size_t>(oy) * out_w + ox] = v;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Postprocessing helpers
// ---------------------------------------------------------------------------

MatteMask postprocess_pha_to_mask(
    const float *pha_data, int pha_w, int pha_h,
    int out_w, int out_h)
{
    MatteMask mask;
    if (!pha_data || pha_w <= 0 || pha_h <= 0) return mask;
    if (out_w <= 0 || out_h <= 0) return mask;

    mask.width  = out_w;
    mask.height = out_h;
    mask.alpha.resize(static_cast<size_t>(out_w) * out_h);

    const float sx = static_cast<float>(pha_w) / static_cast<float>(out_w);
    const float sy = static_cast<float>(pha_h) / static_cast<float>(out_h);

    for (int oy = 0; oy < out_h; ++oy) {
        for (int ox = 0; ox < out_w; ++ox) {
            float fx = (static_cast<float>(ox) + 0.5f) * sx - 0.5f;
            float fy = (static_cast<float>(oy) + 0.5f) * sy - 0.5f;
            float v = sample_float_bilinear(pha_data, pha_w, pha_h, fx, fy);
            mask.alpha[static_cast<size_t>(oy) * out_w + ox] =
                std::max(0.0f, std::min(1.0f, v));
        }
    }
    return mask;
}

std::vector<float> normalize_minmax(const float *data, size_t count)
{
    if (count == 0) return {};
    float lo = data[0], hi = data[0];
    for (size_t i = 1; i < count; ++i) {
        if (data[i] < lo) lo = data[i];
        if (data[i] > hi) hi = data[i];
    }
    std::vector<float> out(count);
    float range = hi - lo;
    if (range < 1e-8f) {
        std::fill(out.begin(), out.end(), 0.0f);
        return out;
    }
    for (size_t i = 0; i < count; ++i) {
        out[i] = (data[i] - lo) / range;
    }
    return out;
}

std::vector<float> argmax_multiclass_to_mask(
    const float *data, int h, int w, int num_classes)
{
    // data is laid out as [h, w, num_classes] (BHWC)
    size_t pixel_count = static_cast<size_t>(h) * w;
    std::vector<float> mask(pixel_count);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            size_t base = (static_cast<size_t>(y) * w + x) * num_classes;
            int best_class = 0;
            float best_val = data[base];
            for (int c = 1; c < num_classes; ++c) {
                float val = data[base + c];
                if (val > best_val) {
                    best_val = val;
                    best_class = c;
                }
            }
            // foreground = class > 0 (1=hair, 2=body-skin, 3=face-skin, 4=clothes, 5=others)
            // mask = best_val if class > 0, else 0
            mask[static_cast<size_t>(y) * w + x] =
                (best_class > 0) ? best_val : 0.0f;
        }
    }
    return mask;
}

MatteMask postprocess_output_to_mask(
    const float *output_data,
    int out_w, int out_h,
    int num_channels,
    PostprocessMode mode)
{
    MatteMask mask;
    if (!output_data || out_w <= 0 || out_h <= 0) return mask;

    size_t pixel_count = static_cast<size_t>(out_w) * out_h;

    switch (mode) {
        case PostprocessMode::Direct:
        case PostprocessMode::MinMax:
        case PostprocessMode::Channel1MinMax:
        case PostprocessMode::Channel1:
        case PostprocessMode::ArgmaxMulticlass:
            break;
    }

    std::vector<float> single_channel;

    if (mode == PostprocessMode::Direct) {
        // Output is already single-channel [0,1] — just clamp
        single_channel.resize(pixel_count);
        for (size_t i = 0; i < pixel_count; ++i) {
            single_channel[i] = std::max(0.0f, std::min(1.0f, output_data[i]));
        }
    } else if (mode == PostprocessMode::Channel1 && num_channels >= 2) {
        // BHWC: [1,H,W,C] → take channel index 1
        // data layout: [H, W, C] interleaved
        single_channel.resize(pixel_count);
        for (int y = 0; y < out_h; ++y) {
            for (int x = 0; x < out_w; ++x) {
                size_t base = (static_cast<size_t>(y) * out_w + x) * num_channels;
                single_channel[static_cast<size_t>(y) * out_w + x] = output_data[base + 1];
            }
        }
    } else if (mode == PostprocessMode::Channel1MinMax && num_channels >= 2) {
        // BCHW: [1,C,H,W] → take channel index 1 (plane at offset H*W)
        // Then normalize MINMAX
        const float *ch1 = output_data + pixel_count; // channel 1 starts at offset H*W
        single_channel = normalize_minmax(ch1, pixel_count);
    } else if (mode == PostprocessMode::MinMax) {
        // BCHW: [1,1,H,W] — single channel, normalize MINMAX
        single_channel = normalize_minmax(output_data, pixel_count);
    } else if (mode == PostprocessMode::ArgmaxMulticlass && num_channels >= 2) {
        // BHWC: [1,H,W,C] — argmax across channels
        single_channel = argmax_multiclass_to_mask(output_data, out_h, out_w, num_channels);
    } else {
        // Fallback: treat as direct
        single_channel.resize(pixel_count);
        for (size_t i = 0; i < pixel_count; ++i) {
            single_channel[i] = std::max(0.0f, std::min(1.0f, output_data[i]));
        }
    }

    // Clamp to [0,1]
    for (size_t i = 0; i < pixel_count; ++i) {
        single_channel[i] = std::max(0.0f, std::min(1.0f, single_channel[i]));
    }

    mask.width = out_w;
    mask.height = out_h;
    mask.alpha = std::move(single_channel);

    return mask;
}

// ---------------------------------------------------------------------------
// Model specs (static data, shared between stub and ONNX modes)
// ---------------------------------------------------------------------------

static const std::vector<MatteModelSpec> kModelSpecs = {
    // 0: RVM
    {
        MatteModelType::RVM,
        "rvm",
        "Robust Video Matting",
        "rvm_mobilenetv3_fp32.onnx",
        true,   // stateful
        320, 192,
        TensorLayout::BCHW,
        NormalizeMode::ImageNet,
        PostprocessMode::Direct,
        1,      // pha is 1 channel
    },
    // 1: MediaPipe
    {
        MatteModelType::MediaPipe,
        "mediapipe",
        "MediaPipe Selfie Segmentation",
        "mediapipe.with_runtime_opt.ort",
        false,
        256, 256,
        TensorLayout::BHWC,
        NormalizeMode::Divide255,
        PostprocessMode::Channel1,
        2,      // 2 output channels
    },
    // 2: Selfie
    {
        MatteModelType::Selfie,
        "selfie",
        "Selfie Segmentation",
        "selfie_segmentation.with_runtime_opt.ort",
        false,
        256, 256,
        TensorLayout::BHWC,
        NormalizeMode::Divide255,
        PostprocessMode::MinMax,
        1,
    },
    // 3: Selfie Multiclass
    {
        MatteModelType::SelfieMulticlass,
        "selfie_multiclass",
        "Selfie Multiclass",
        "selfie_multiclass_256x256.with_runtime_opt.ort",
        false,
        256, 256,
        TensorLayout::BHWC,
        NormalizeMode::Divide255,
        PostprocessMode::ArgmaxMulticlass,
        6,
    },
    // 4: PPHumanSeg
    {
        MatteModelType::PPHumanSeg,
        "pphumanseg",
        "PPHumanSeg",
        "pphumanseg_fp32.with_runtime_opt.ort",
        false,
        192, 192,
        TensorLayout::BCHW,
        NormalizeMode::PPHumanSeg,
        PostprocessMode::Channel1MinMax,
        2,
    },
    // 5: SINet
    {
        MatteModelType::SINet,
        "sinet",
        "SINet",
        "SINet_Softmax_simple.with_runtime_opt.ort",
        false,
        320, 320,
        TensorLayout::BCHW,
        NormalizeMode::SINet,
        PostprocessMode::Channel1,
        2,
    },
    // 6: TCMonoDepth
    {
        MatteModelType::TCMonoDepth,
        "tcmonodepth",
        "TCMonoDepth (experimental)",
        "tcmonodepth_tcsmallnet_192x320.with_runtime_opt.ort",
        false,
        320, 192,
        TensorLayout::BCHW,
        NormalizeMode::None,
        PostprocessMode::MinMax,
        1,
    },
};

const std::vector<MatteModelSpec> &getAllModelSpecs()
{
    return kModelSpecs;
}

const MatteModelSpec &getModelSpec(MatteModelType type)
{
    for (const auto &s : kModelSpecs) {
        if (s.type == type) return s;
    }
    // Fallback: RVM (index 0)
    return kModelSpecs[0];
}

MatteModelType resolveModelType(const std::string &filename)
{
    // Extract basename
    std::string base = filename;
    size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);

    // Convert to lowercase for matching
    std::string lower = base;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return std::tolower(c); });

    if (lower.find("rvm") != std::string::npos)
        return MatteModelType::RVM;
    if (lower.find("mediapipe") != std::string::npos)
        return MatteModelType::MediaPipe;
    if (lower.find("selfie_multiclass") != std::string::npos)
        return MatteModelType::SelfieMulticlass;
    if (lower.find("selfie_segmentation") != std::string::npos ||
        lower.find("selfie") != std::string::npos)
        return MatteModelType::Selfie;
    if (lower.find("pphumanseg") != std::string::npos)
        return MatteModelType::PPHumanSeg;
    if (lower.find("sinet") != std::string::npos)
        return MatteModelType::SINet;
    if (lower.find("tcmonodepth") != std::string::npos)
        return MatteModelType::TCMonoDepth;

    // Default: RVM
    return MatteModelType::RVM;
}

} // namespace dancehap (helpers + specs)

// ===========================================================================
// Provider resolution (shared, testable in stub mode)
// ===========================================================================

namespace dancehap {

ActiveProvider resolve_provider(ExecutionProvider desired)
{
#if defined(_WIN32)
    if (desired == ExecutionProvider::Auto ||
        desired == ExecutionProvider::DirectML) {
        return ActiveProvider::DirectML;
    }
    if (desired == ExecutionProvider::CoreML) {
        return ActiveProvider::CPU;
    }
    return ActiveProvider::CPU;
#elif defined(__APPLE__)
    if (desired == ExecutionProvider::Auto ||
        desired == ExecutionProvider::CoreML) {
        return ActiveProvider::CoreML;
    }
    if (desired == ExecutionProvider::DirectML) {
        return ActiveProvider::CPU;
    }
    return ActiveProvider::CPU;
#else
    return ActiveProvider::CPU;
#endif
}

} // namespace dancehap

// ===========================================================================
//  Real ONNX Runtime implementation (Phase 2.2 + 2.6 multi-model)
// ===========================================================================
#ifdef DANCEHAP_HAVE_ONNXRUNTIME

#include <onnxruntime_c_api.h>
#if defined(_WIN32)
#include <dml_provider_factory.h>
#endif

namespace dancehap {

static const OrtApi *g_ort = nullptr;

static bool ensure_ort()
{
    if (g_ort) return true;
    g_ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    return g_ort != nullptr;
}

// ---------------------------------------------------------------------------
// MatteEngineImpl — exposed for backends
// ---------------------------------------------------------------------------

struct MatteEngineImpl {
    OrtEnv     *env      = nullptr;
    OrtSession *session  = nullptr;
    ActiveProvider active = ActiveProvider::Unknown;

    // Cached input/output names (populated after loadModel)
    std::vector<std::string> input_names_str;
    std::vector<std::string> output_names_str;
    std::vector<const char *> input_names_raw;
    std::vector<const char *> output_names_raw;

    // Cached input/output shapes
    std::vector<std::vector<int64_t>> input_shapes;
    std::vector<std::vector<int64_t>> output_shapes;

    // RVM recurrent state buffers (r1i..r4i → r1o..r4o)
    // For stateful models, these persist between infer() calls.
    std::vector<std::vector<float>> recurrent_state;

    ~MatteEngineImpl()
    {
        if (session) g_ort->ReleaseSession(session);
        if (env) g_ort->ReleaseEnv(env);
    }

    void reset()
    {
        if (session) { g_ort->ReleaseSession(session); session = nullptr; }
        if (env) { g_ort->ReleaseEnv(env); env = nullptr; }
        active = ActiveProvider::Unknown;
        input_names_str.clear();
        output_names_str.clear();
        input_names_raw.clear();
        output_names_raw.clear();
        input_shapes.clear();
        output_shapes.clear();
        recurrent_state.clear();
    }

    /// Populate input/output names dynamically from the session.
    bool populateNames()
    {
        if (!session) return false;

        input_names_str.clear();
        output_names_str.clear();
        input_names_raw.clear();
        output_names_raw.clear();

        size_t in_count = 0, out_count = 0;
        OrtStatus *st;

        st = g_ort->SessionGetInputCount(session, &in_count);
        if (st) { g_ort->ReleaseStatus(st); return false; }

        st = g_ort->SessionGetOutputCount(session, &out_count);
        if (st) { g_ort->ReleaseStatus(st); return false; }

        OrtAllocator *allocator = nullptr;
        st = g_ort->GetAllocatorWithDefaultOptions(&allocator);
        if (st) { g_ort->ReleaseStatus(st); return false; }

        for (size_t i = 0; i < in_count; ++i) {
            char *name = nullptr;
            st = g_ort->SessionGetInputName(session, i, allocator, &name);
            if (st) { g_ort->ReleaseStatus(st); return false; }
            input_names_str.push_back(name);
            g_ort->AllocatorFree(allocator, name);
        }
        for (size_t i = 0; i < out_count; ++i) {
            char *name = nullptr;
            st = g_ort->SessionGetOutputName(session, i, allocator, &name);
            if (st) { g_ort->ReleaseStatus(st); return false; }
            output_names_str.push_back(name);
            g_ort->AllocatorFree(allocator, name);
        }

        // Build raw C-string arrays for Run()
        for (auto &s : input_names_str)
            input_names_raw.push_back(s.c_str());
        for (auto &s : output_names_str)
            output_names_raw.push_back(s.c_str());

        return true;
    }

    /// Populate input/output shapes from the session.
    bool populateShapes()
    {
        if (!session) return false;

        input_shapes.clear();
        output_shapes.clear();

        // Input shapes
        for (size_t i = 0; i < input_names_str.size(); ++i) {
            OrtTypeInfo *type_info = nullptr;
            OrtStatus *st = g_ort->SessionGetInputTypeInfo(session, i, &type_info);
            if (st) { g_ort->ReleaseStatus(st); return false; }

            // CastTypeInfoToTensorInfo: OrtTypeInfo* → OrtTensorTypeAndShapeInfo*
            // (GetTensorTypeAndShape takes an OrtValue*, NOT OrtTypeInfo*)
            const OrtTensorTypeAndShapeInfo *tensor_info_const;
            st = g_ort->CastTypeInfoToTensorInfo(type_info, &tensor_info_const);
            if (st) { g_ort->ReleaseStatus(st); g_ort->ReleaseTypeInfo(type_info); return false; }

            size_t dim_count = 0;
            g_ort->GetDimensionsCount(tensor_info_const, &dim_count);
            std::vector<int64_t> dims(dim_count);
            g_ort->GetDimensions(tensor_info_const, dims.data(), dim_count);

            // Fix -1 (dynamic) to 1
            for (auto &d : dims) if (d < 0) d = 1;

            input_shapes.push_back(dims);
            g_ort->ReleaseTypeInfo(type_info);
            // Note: CastTypeInfoToTensorInfo returns a borrowed reference,
            // do NOT call ReleaseTensorTypeAndShapeInfo on it.
        }

        // Output shapes
        for (size_t i = 0; i < output_names_str.size(); ++i) {
            OrtTypeInfo *type_info = nullptr;
            OrtStatus *st = g_ort->SessionGetOutputTypeInfo(session, i, &type_info);
            if (st) { g_ort->ReleaseStatus(st); return false; }

            const OrtTensorTypeAndShapeInfo *tensor_info_const;
            st = g_ort->CastTypeInfoToTensorInfo(type_info, &tensor_info_const);
            if (st) { g_ort->ReleaseStatus(st); g_ort->ReleaseTypeInfo(type_info); return false; }

            size_t dim_count = 0;
            g_ort->GetDimensionsCount(tensor_info_const, &dim_count);
            std::vector<int64_t> dims(dim_count);
            g_ort->GetDimensions(tensor_info_const, dims.data(), dim_count);

            for (auto &d : dims) if (d < 0) d = 1;

            output_shapes.push_back(dims);
            g_ort->ReleaseTypeInfo(type_info);
        }

        return true;
    }
};

// Forward declaration used by the header's MatteModelBackend
// (MatteEngineImpl is now defined above)

// ---------------------------------------------------------------------------
// Backend base class with common utilities
// ---------------------------------------------------------------------------

class BaseBackend : public MatteModelBackend {
protected:
    MatteModelSpec spec_;

public:
    explicit BaseBackend(MatteModelType type) {
        spec_ = getModelSpec(type);
    }

    const MatteModelSpec &spec() const override { return spec_; }

    /// Create a float32 OrtValue tensor with the given shape and data.
    /// The data vector must outlive the OrtValue.
    OrtValue *createTensor(MatteEngineImpl &impl,
        std::vector<float> &data,
        const std::vector<int64_t> &shape) const
    {
        OrtMemoryInfo *mem_info = nullptr;
        OrtStatus *st = g_ort->CreateCpuMemoryInfo(
            OrtArenaAllocator, OrtMemTypeDefault, &mem_info);
        if (st) { g_ort->ReleaseStatus(st); return nullptr; }

        OrtValue *value = nullptr;
        st = g_ort->CreateTensorWithDataAsOrtValue(
            mem_info,
            data.data(),
            data.size() * sizeof(float),
            shape.data(),
            static_cast<int>(shape.size()),
            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
            &value);
        g_ort->ReleaseMemoryInfo(mem_info);
        if (st) {
            const char *msg = g_ort->GetErrorMessage(st);
            blog(LOG_WARNING, "[DanceHAP] CreateTensorWithData failed: %s",
                 msg ? msg : "(unknown)");
            g_ort->ReleaseStatus(st);
            return nullptr;
        }
        return value;
    }

    /// Run inference with the given input/output OrtValues.
    bool runInference(MatteEngineImpl &impl,
        std::vector<OrtValue *> &inputs,
        std::vector<OrtValue *> &outputs) const
    {
        OrtStatus *st = g_ort->Run(impl.session, nullptr,
            impl.input_names_raw.data(), inputs.data(),
            static_cast<size_t>(inputs.size()),
            impl.output_names_raw.data(),
            static_cast<size_t>(impl.output_names_raw.size()),
            outputs.data());

        if (st) {
            const char *msg = g_ort->GetErrorMessage(st);
            blog(LOG_WARNING, "[DanceHAP] Run failed: %s",
                 msg ? msg : "(unknown)");
            g_ort->ReleaseStatus(st);
            return false;
        }
        return true;
    }

    /// Extract float* data pointer from an OrtValue.
    float *getTensorData(OrtValue *value) const
    {
        float *ptr = nullptr;
        OrtStatus *st = g_ort->GetTensorMutableData(value,
            reinterpret_cast<void **>(&ptr));
        if (st) {
            g_ort->ReleaseStatus(st);
            return nullptr;
        }
        return ptr;
    }

    /// Get the H and W from a 4D shape [1, ?, H, W] or [1, H, W, ?].
    void getHWFromShape(const std::vector<int64_t> &shape,
        int &out_h, int &out_w, TensorLayout layout) const
    {
        if (shape.size() < 4) {
            out_h = spec_.default_height;
            out_w = spec_.default_width;
            return;
        }
        if (layout == TensorLayout::BCHW) {
            out_h = static_cast<int>(shape[2]);
            out_w = static_cast<int>(shape[3]);
        } else { // BHWC
            out_h = static_cast<int>(shape[1]);
            out_w = static_cast<int>(shape[2]);
        }
    }
};

// ---------------------------------------------------------------------------
// Stateless backend (MediaPipe, Selfie, SelfieMulticlass, PPHumanSeg, SINet, TCMonoDepth)
// ---------------------------------------------------------------------------

class StatelessBackend : public BaseBackend {
public:
    explicit StatelessBackend(MatteModelType type) : BaseBackend(type) {}

    MatteMask infer(MatteEngineImpl &impl, const ImageFrame &input) override
    {
        if (!impl.session || !input.data_rgba || input.width <= 0 || input.height <= 0)
            return MatteMask{};

        int in_w = spec_.default_width;
        int in_h = spec_.default_height;

        // Override with config if set
        // (MatteEngine passes config_ through to the backend via the spec)
        // Use the input shape from the model if available
        if (!impl.input_shapes.empty() && impl.input_shapes[0].size() >= 4) {
            getHWFromShape(impl.input_shapes[0], in_h, in_w, spec_.layout);
        }

        // 1. Preprocess
        std::vector<float> input_tensor = preprocess_rgba_for_model(
            input, in_w, in_h, spec_.layout, spec_.normalize);
        if (input_tensor.empty()) return MatteMask{};

        // 2. Create input OrtValue with shape [1, C, H, W] or [1, H, W, C]
        std::vector<int64_t> input_shape;
        if (spec_.layout == TensorLayout::BCHW) {
            input_shape = {1, 3, in_h, in_w};
        } else {
            input_shape = {1, in_h, in_w, 3};
        }

        OrtValue *input_value = createTensor(impl, input_tensor, input_shape);
        if (!input_value) return MatteMask{};

        // 3. Run inference (1 input, 1 output)
        std::vector<OrtValue *> inputs = {input_value};
        std::vector<OrtValue *> outputs(impl.output_names_raw.size(), nullptr);

        if (!runInference(impl, inputs, outputs)) {
            g_ort->ReleaseValue(input_value);
            return MatteMask{};
        }
        g_ort->ReleaseValue(input_value);

        if (outputs.empty() || !outputs[0]) {
            for (auto *o : outputs) if (o) g_ort->ReleaseValue(o);
            return MatteMask{};
        }

        // 4. Get output data and shape
        float *output_data = getTensorData(outputs[0]);
        if (!output_data) {
            for (auto *o : outputs) if (o) g_ort->ReleaseValue(o);
            return MatteMask{};
        }

        // Determine output H, W, C
        int out_h = in_h, out_w = in_w;
        int out_c = spec_.output_channels;

        if (!impl.output_shapes.empty() && impl.output_shapes[0].size() >= 4) {
            const auto &oshape = impl.output_shapes[0];
            if (spec_.layout == TensorLayout::BCHW) {
                // [1, C, H, W]
                out_c = static_cast<int>(oshape[1]);
                out_h = static_cast<int>(oshape[2]);
                out_w = static_cast<int>(oshape[3]);
            } else {
                // [1, H, W, C]
                out_h = static_cast<int>(oshape[1]);
                out_w = static_cast<int>(oshape[2]);
                out_c = static_cast<int>(oshape[3]);
            }
        }

        // 5. Postprocess: extract single-channel mask from output
        // For BCHW output: data is [1, C, H, W] = C planes of H*W
        // For BHWC output: data is [1, H, W, C] = interleaved
        MatteMask mask;

        if (spec_.postprocess == PostprocessMode::Direct) {
            // Single channel [0,1] at model resolution
            mask = postprocess_pha_to_mask(output_data, out_w, out_h,
                input.width, input.height);
        } else if (spec_.postprocess == PostprocessMode::Channel1 &&
                   spec_.layout == TensorLayout::BHWC) {
            // BHWC: take channel index 1 from interleaved [H,W,C]
            // Reuse postprocess_output_to_mask which handles BHWC Channel1
            mask = postprocess_output_to_mask(output_data, out_w, out_h,
                out_c, PostprocessMode::Channel1);
            // Resize to input dims
            auto resized = resize_float_bilinear(mask.alpha.data(),
                mask.width, mask.height, input.width, input.height);
            mask.width = input.width;
            mask.height = input.height;
            mask.alpha = std::move(resized);
        } else if (spec_.postprocess == PostprocessMode::Channel1 &&
                   spec_.layout == TensorLayout::BCHW) {
            // BCHW: channel 1 is at offset H*W
            const float *ch1 = output_data + static_cast<size_t>(out_w) * out_h;
            mask = postprocess_pha_to_mask(ch1, out_w, out_h,
                input.width, input.height);
        } else if (spec_.postprocess == PostprocessMode::MinMax) {
            // BCHW single channel [1,1,H,W]
            auto normed = normalize_minmax(output_data,
                static_cast<size_t>(out_w) * out_h);
            mask = postprocess_pha_to_mask(normed.data(), out_w, out_h,
                input.width, input.height);
        } else if (spec_.postprocess == PostprocessMode::Channel1MinMax) {
            // BCHW 2-channel: take channel 1, then MINMAX
            const float *ch1 = output_data + static_cast<size_t>(out_w) * out_h;
            auto normed = normalize_minmax(ch1,
                static_cast<size_t>(out_w) * out_h);
            mask = postprocess_pha_to_mask(normed.data(), out_w, out_h,
                input.width, input.height);
        } else if (spec_.postprocess == PostprocessMode::ArgmaxMulticlass) {
            // BHWC: [1, H, W, 6] — argmax across 6 channels
            auto mask_vals = argmax_multiclass_to_mask(output_data, out_h, out_w, out_c);
            mask = postprocess_pha_to_mask(mask_vals.data(), out_w, out_h,
                input.width, input.height);
        }

        // Clamp final mask to [0,1]
        for (auto &v : mask.alpha) {
            if (v < 0.0f) v = 0.0f;
            else if (v > 1.0f) v = 1.0f;
        }

        // Cleanup
        for (auto *o : outputs) if (o) g_ort->ReleaseValue(o);

        return mask;
    }
};

// ---------------------------------------------------------------------------
// RVM backend (stateful: 6 inputs, 6 outputs, recurrent states)
// ---------------------------------------------------------------------------

class RVMBackend : public BaseBackend {
public:
    RVMBackend() : BaseBackend(MatteModelType::RVM) {}

    MatteMask infer(MatteEngineImpl &impl, const ImageFrame &input) override
    {
        if (!impl.session || !input.data_rgba || input.width <= 0 || input.height <= 0)
            return MatteMask{};

        int in_w = spec_.default_width;   // 320
        int in_h = spec_.default_height;  // 192

        // Override from model shape if available
        if (!impl.input_shapes.empty() && impl.input_shapes[0].size() >= 4) {
            in_h = static_cast<int>(impl.input_shapes[0][2]);
            in_w = static_cast<int>(impl.input_shapes[0][3]);
        }

        // 1. Preprocess: RGBA → BCHW ImageNet normalized
        std::vector<float> input_tensor = preprocess_rgba_for_model(
            input, in_w, in_h, spec_.layout, spec_.normalize);
        if (input_tensor.empty()) return MatteMask{};

        // 2. Create 6 input tensors: src + r1i..r4i + downsample
        std::vector<int64_t> src_shape = {1, 3, in_h, in_w};

        // RVM recurrent state channel dimensions:
        // r1i: [1, 16, H/2, W/2]
        // r2i: [1, 20, H/4, W/4]
        // r3i: [1, 40, H/8, W/8]
        // r4i: [1, 64, H/16, W/16]
        int ch_dims[4] = {16, 20, 40, 64};
        std::vector<std::vector<float>> state_bufs(4);
        std::vector<std::vector<int64_t>> state_shapes(4);

        for (int i = 0; i < 4; ++i) {
            int sh = in_h / (2 << i);
            int sw = in_w / (2 << i);
            state_shapes[i] = {1, ch_dims[i], sh, sw};
            state_bufs[i].resize(static_cast<size_t>(ch_dims[i]) * sh * sw, 0.0f);
        }

        // If we have recurrent state from previous frame, use it
        if (impl.recurrent_state.size() == 4) {
            for (int i = 0; i < 4; ++i) {
                if (impl.recurrent_state[i].size() == state_bufs[i].size()) {
                    state_bufs[i] = impl.recurrent_state[i];
                }
            }
        }

        // downsample scalar = 1.0
        std::vector<float> downsample_buf = {1.0f};
        std::vector<int64_t> downsample_shape = {1};

        // Create OrtValues
        OrtValue *src_val = createTensor(impl, input_tensor, src_shape);
        if (!src_val) return MatteMask{};

        OrtValue *state_vals[4] = {nullptr, nullptr, nullptr, nullptr};
        for (int i = 0; i < 4; ++i) {
            state_vals[i] = createTensor(impl, state_bufs[i], state_shapes[i]);
            if (!state_vals[i]) {
                g_ort->ReleaseValue(src_val);
                for (int j = 0; j < i; ++j) g_ort->ReleaseValue(state_vals[j]);
                return MatteMask{};
            }
        }

        OrtValue *ds_val = createTensor(impl, downsample_buf, downsample_shape);
        if (!ds_val) {
            g_ort->ReleaseValue(src_val);
            for (int i = 0; i < 4; ++i) g_ort->ReleaseValue(state_vals[i]);
            return MatteMask{};
        }

        // 3. Assemble inputs: [src, r1i, r2i, r3i, r4i, downsample]
        std::vector<OrtValue *> inputs = {
            src_val, state_vals[0], state_vals[1],
            state_vals[2], state_vals[3], ds_val
        };

        // 4. Run inference (6 outputs: fgr, pha, r1o, r2o, r3o, r4o)
        std::vector<OrtValue *> outputs(impl.output_names_raw.size(), nullptr);

        if (!runInference(impl, inputs, outputs)) {
            g_ort->ReleaseValue(src_val);
            for (int i = 0; i < 4; ++i) g_ort->ReleaseValue(state_vals[i]);
            g_ort->ReleaseValue(ds_val);
            return MatteMask{};
        }

        // Cleanup inputs
        g_ort->ReleaseValue(src_val);
        for (int i = 0; i < 4; ++i) g_ort->ReleaseValue(state_vals[i]);
        g_ort->ReleaseValue(ds_val);

        // 5. Extract pha output — find it by name or use index 1
        // RVM output order: fgr(0), pha(1), r1o(2), r2o(3), r3o(4), r4o(5)
        int pha_index = 1;
        for (size_t i = 0; i < impl.output_names_str.size(); ++i) {
            if (impl.output_names_str[i] == "pha") {
                pha_index = static_cast<int>(i);
                break;
            }
        }

        if (pha_index >= static_cast<int>(outputs.size()) || !outputs[pha_index]) {
            for (auto *o : outputs) if (o) g_ort->ReleaseValue(o);
            return MatteMask{};
        }

        float *pha_data = getTensorData(outputs[pha_index]);
        if (!pha_data) {
            for (auto *o : outputs) if (o) g_ort->ReleaseValue(o);
            return MatteMask{};
        }

        // Get pha dimensions
        int pha_h = in_h, pha_w = in_w;
        if (pha_index < static_cast<int>(impl.output_shapes.size()) &&
            impl.output_shapes[pha_index].size() >= 4) {
            pha_h = static_cast<int>(impl.output_shapes[pha_index][2]);
            pha_w = static_cast<int>(impl.output_shapes[pha_index][3]);
        }

        // 6. Postprocess: pha is [1,1,H,W] float32 [0,1] → resize to input dims
        MatteMask mask = postprocess_pha_to_mask(
            pha_data, pha_w, pha_h, input.width, input.height);

        // 7. Save recurrent state: r1o→r1i, r2o→r2i, etc.
        // Outputs after pha: r1o, r2o, r3o, r4o
        impl.recurrent_state.clear();
        impl.recurrent_state.resize(4);

        for (int i = 0; i < 4; ++i) {
            int r_index = pha_index + 1 + i; // outputs: fgr, pha, r1o, r2o, r3o, r4o
            if (r_index < static_cast<int>(outputs.size()) && outputs[r_index]) {
                float *r_data = getTensorData(outputs[r_index]);
                if (r_data) {
                    // Determine size from shape or buffer
                    size_t r_size = state_bufs[i].size();
                    if (r_index < static_cast<int>(impl.output_shapes.size())) {
                        const auto &rshape = impl.output_shapes[r_index];
                        size_t computed = 1;
                        for (auto d : rshape) {
                            if (d > 0) computed *= static_cast<size_t>(d);
                        }
                        if (computed > 0) r_size = computed;
                    }
                    impl.recurrent_state[i].assign(r_data, r_data + r_size);
                }
            }
        }

        // Cleanup outputs
        for (auto *o : outputs) if (o) g_ort->ReleaseValue(o);

        return mask;
    }
};

// ---------------------------------------------------------------------------
// Backend factory
// ---------------------------------------------------------------------------

std::unique_ptr<MatteModelBackend> createBackend(MatteModelType type)
{
    switch (type) {
        case MatteModelType::RVM:
            return std::make_unique<RVMBackend>();
        case MatteModelType::MediaPipe:
        case MatteModelType::Selfie:
        case MatteModelType::SelfieMulticlass:
        case MatteModelType::PPHumanSeg:
        case MatteModelType::SINet:
        case MatteModelType::TCMonoDepth:
            return std::make_unique<StatelessBackend>(type);
        default:
            return nullptr;
    }
}

// ---------------------------------------------------------------------------
// MatteEngine::Impl (wraps MatteEngineImpl + backend)
// ---------------------------------------------------------------------------

struct MatteEngine::Impl {
    MatteEngineImpl               ort;
    std::unique_ptr<MatteModelBackend> backend;
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

MatteEngine::MatteEngine()
    : pimpl_(std::make_unique<Impl>())
{
    ensure_ort();
    model_type_ = MatteModelType::RVM;
}

MatteEngine::~MatteEngine() = default;
MatteEngine::MatteEngine(MatteEngine &&) noexcept = default;
MatteEngine &MatteEngine::operator=(MatteEngine &&) noexcept = default;

bool MatteEngine::loadModel(const std::string &model_path)
{
    if (!ensure_ort()) {
        blog(LOG_WARNING, "[DanceHAP] MatteEngine: ONNX Runtime API unavailable");
        return false;
    }

    pimpl_->ort.reset();

    // Auto-detect model type from filename
    model_type_ = resolveModelType(model_path);
    const auto &spec = getModelSpec(model_type_);

    // Update config with model defaults if not explicitly set
    if (config_.input_width == 256 && config_.input_height == 256) {
        config_.input_width = spec.default_width;
        config_.input_height = spec.default_height;
    }

    // Create environment
    OrtStatus *st = g_ort->CreateEnv(
        ORT_LOGGING_LEVEL_WARNING, "DanceHAP", &pimpl_->ort.env);
    if (st) {
        blog(LOG_WARNING, "[DanceHAP] MatteEngine: CreateEnv failed");
        g_ort->ReleaseStatus(st);
        return false;
    }

    // Create session options
    OrtSessionOptions *opts;
    st = g_ort->CreateSessionOptions(&opts);
    if (st) {
        blog(LOG_WARNING, "[DanceHAP] MatteEngine: CreateSessionOptions failed");
        g_ort->ReleaseStatus(st);
        return false;
    }

    // Resolve and append execution provider
    ActiveProvider ap = resolve_provider(provider_);
    bool ep_ok = false;

#if defined(_WIN32)
    if (ap == ActiveProvider::DirectML) {
        st = OrtSessionOptionsAppendExecutionProvider_DML(opts, 0);
        if (st) {
            blog(LOG_WARNING, "[DanceHAP] MatteEngine: DirectML EP failed, falling back to CPU");
            g_ort->ReleaseStatus(st);
            ap = ActiveProvider::CPU;
        } else {
            ep_ok = true;
        }
    }
#elif defined(__APPLE__)
    if (ap == ActiveProvider::CoreML) {
        extern "C" OrtStatus *OrtSessionOptionsAppendExecutionProvider_CoreML(
            OrtSessionOptions *options, uint32_t flags);
        st = OrtSessionOptionsAppendExecutionProvider_CoreML(opts, 1);
        if (st) {
            blog(LOG_WARNING, "[DanceHAP] MatteEngine: CoreML EP failed, falling back to CPU");
            g_ort->ReleaseStatus(st);
            ap = ActiveProvider::CPU;
        } else {
            ep_ok = true;
        }
    }
#endif
    (void)ep_ok;

    pimpl_->ort.active = ap;
    const char *provider_name =
        (ap == ActiveProvider::DirectML) ? "DirectML" :
        (ap == ActiveProvider::CoreML)   ? "CoreML"   : "CPU";
    blog(LOG_INFO, "[DanceHAP] MatteEngine: using %s provider", provider_name);

    // Create session
    st =
#if defined(_WIN32)
        g_ort->CreateSession(pimpl_->ort.env,
            std::wstring(model_path.begin(), model_path.end()).c_str(),
            opts, &pimpl_->ort.session);
#else
        g_ort->CreateSession(pimpl_->ort.env, model_path.c_str(), opts,
            &pimpl_->ort.session);
#endif
    g_ort->ReleaseSessionOptions(opts);
    if (st) {
        const char *msg = g_ort->GetErrorMessage(st);
        blog(LOG_WARNING, "[DanceHAP] MatteEngine: CreateSession failed: %s",
             msg ? msg : "(unknown)");
        g_ort->ReleaseStatus(st);
        return false;
    }

    // Populate input/output names and shapes dynamically
    if (!pimpl_->ort.populateNames()) {
        blog(LOG_WARNING, "[DanceHAP] MatteEngine: failed to populate I/O names");
        return false;
    }
    if (!pimpl_->ort.populateShapes()) {
        blog(LOG_WARNING, "[DanceHAP] MatteEngine: failed to populate I/O shapes");
        return false;
    }

    // Create the backend for this model type
    pimpl_->backend = createBackend(model_type_);
    if (!pimpl_->backend) {
        blog(LOG_WARNING, "[DanceHAP] MatteEngine: no backend for model type %d",
             static_cast<int>(model_type_));
        return false;
    }

    blog(LOG_INFO, "[DanceHAP] MatteEngine: model loaded '%s' (type=%s)",
         model_path.c_str(), getModelSpec(model_type_).name.c_str());
    return true;
}

bool MatteEngine::isReady() const
{
    return pimpl_->ort.session != nullptr && pimpl_->backend != nullptr;
}

ActiveProvider MatteEngine::activeProvider() const
{
    return pimpl_->ort.active;
}

MatteModelType MatteEngine::modelType() const
{
    return model_type_;
}

void MatteEngine::setModelType(MatteModelType type)
{
    model_type_ = type;
}

const MatteModelSpec &MatteEngine::modelSpec() const
{
    return getModelSpec(model_type_);
}

// ---------------------------------------------------------------------------
// Inference dispatch
// ---------------------------------------------------------------------------

MatteMask MatteEngine::infer(const ImageFrame &input)
{
    if (!isReady() || !input.data_rgba || input.width <= 0 || input.height <= 0)
        return MatteMask{};

    return pimpl_->backend->infer(pimpl_->ort, input);
}

} // namespace dancehap

// ===========================================================================
//  Stub implementation (no ONNX Runtime)
// ===========================================================================
#else // !DANCEHAP_HAVE_ONNXRUNTIME

namespace dancehap {

struct MatteEngine::Impl {};

MatteEngine::MatteEngine()
    : pimpl_(std::make_unique<Impl>())
{
}

MatteEngine::~MatteEngine() = default;
MatteEngine::MatteEngine(MatteEngine &&) noexcept = default;
MatteEngine &MatteEngine::operator=(MatteEngine &&) noexcept = default;

bool MatteEngine::loadModel(const std::string & /*model_path*/)
{
    // Stub: no ONNX Runtime available.
    return false;
}

bool MatteEngine::isReady() const
{
    return false;
}

ActiveProvider MatteEngine::activeProvider() const
{
    return ActiveProvider::Unknown;
}

MatteModelType MatteEngine::modelType() const
{
    return model_type_;
}

void MatteEngine::setModelType(MatteModelType type)
{
    model_type_ = type;
}

const MatteModelSpec &MatteEngine::modelSpec() const
{
    return getModelSpec(model_type_);
}

MatteMask MatteEngine::infer(const ImageFrame & /*input*/)
{
    // Stub: returns empty mask.
    return MatteMask{};
}

} // namespace dancehap

#endif // DANCEHAP_HAVE_ONNXRUNTIME