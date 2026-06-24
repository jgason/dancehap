// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// matte_engine.cpp — MatteEngine implementation.
//
// Phase 2.2: real ONNX Runtime implementation + shared helpers.
// Stub mode returns false/empty (no ONNX Runtime linked).

#include "matte_engine.hpp"
#include "obs_compat.hpp"

#include <algorithm>
#include <cmath>

namespace dancehap {

// ===========================================================================
// Shared helpers (compile in both stub and ONNX modes)
// ===========================================================================

// ImageNet normalization constants (RVM backbone = MobileNetV3).
static constexpr float kMean[3] = {0.485f, 0.456f, 0.406f};
static constexpr float kStd[3]  = {0.229f, 0.224f, 0.225f};

/// Bilinear sample of a single channel from RGBA data at (fx, fy).
static inline float sample_rgba_channel_bilinear(
    const uint8_t *data, int w, int h, float fx, float fy, int ch)
{
    // Clamp to edges.
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

std::vector<float> preprocess_rgba_to_rgb_normalized(
    const ImageFrame &input, int out_w, int out_h)
{
    std::vector<float> out;
    if (!input.data_rgba || input.width <= 0 || input.height <= 0) return out;
    if (out_w <= 0 || out_h <= 0) return out;

    out.resize(static_cast<size_t>(3) * out_w * out_h);

    const float sx = static_cast<float>(input.width) / static_cast<float>(out_w);
    const float sy = static_cast<float>(input.height) / static_cast<float>(out_h);

    float *dst_r = out.data();
    float *dst_g = dst_r + static_cast<size_t>(out_w) * out_h;
    float *dst_b = dst_g + static_cast<size_t>(out_w) * out_h;

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

            // Normalize to [0,1] then (x - mean) / std.
            size_t idx = static_cast<size_t>(oy) * out_w + ox;
            dst_r[idx] = (r / 255.0f - kMean[0]) / kStd[0];
            dst_g[idx] = (g / 255.0f - kMean[1]) / kStd[1];
            dst_b[idx] = (b / 255.0f - kMean[2]) / kStd[2];
        }
    }
    return out;
}

/// Bilinear resize of a single-channel float buffer.
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
            // Clamp to [0, 1].
            mask.alpha[static_cast<size_t>(oy) * out_w + ox] =
                std::max(0.0f, std::min(1.0f, v));
        }
    }
    return mask;
}

} // namespace dancehap (helpers)

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
        return ActiveProvider::CPU;  // CoreML not available on Windows
    }
    return ActiveProvider::CPU;
#elif defined(__APPLE__)
    if (desired == ExecutionProvider::Auto ||
        desired == ExecutionProvider::CoreML) {
        return ActiveProvider::CoreML;
    }
    if (desired == ExecutionProvider::DirectML) {
        return ActiveProvider::CPU;  // DirectML not available on macOS
    }
    return ActiveProvider::CPU;
#else
    // Linux and others: CPU only (ADR-001: Linux is non-goal).
    return ActiveProvider::CPU;
#endif
}

} // namespace dancehap

// ===========================================================================
//  Real ONNX Runtime implementation (Phase 2.2)
// ===========================================================================
#ifdef DANCEHAP_HAVE_ONNXRUNTIME

#include <onnxruntime_c_api.h>

namespace dancehap {

// Convenience macro to access the ONNX Runtime API.
static const OrtApi *g_ort = nullptr;

static bool ensure_ort()
{
    if (g_ort) return true;
    g_ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    return g_ort != nullptr;
}

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct MatteEngine::Impl {
    OrtEnv     *env     = nullptr;
    OrtSession *session = nullptr;
    ActiveProvider active = ActiveProvider::Unknown;

    ~Impl()
    {
        if (session) g_ort->ReleaseSession(session);
        if (env) g_ort->ReleaseEnv(env);
    }

    void reset()
    {
        if (session) { g_ort->ReleaseSession(session); session = nullptr; }
        if (env) { g_ort->ReleaseEnv(env); env = nullptr; }
        active = ActiveProvider::Unknown;
    }
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

MatteEngine::MatteEngine()
    : pimpl_(std::make_unique<Impl>())
{
    ensure_ort();
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

    pimpl_->reset();

    // Create environment.
    OrtStatus *st = g_ort->CreateEnv(
        ORT_LOGGING_LEVEL_WARNING, "DanceHAP", &pimpl_->env);
    if (st) {
        blog(LOG_WARNING, "[DanceHAP] MatteEngine: CreateEnv failed");
        g_ort->ReleaseStatus(st);
        return false;
    }

    // Create session options.
    OrtSessionOptions *opts;
    st = g_ort->CreateSessionOptions(&opts);
    if (st) {
        blog(LOG_WARNING, "[DanceHAP] MatteEngine: CreateSessionOptions failed");
        g_ort->ReleaseStatus(st);
        return false;
    }

    // Resolve and append the execution provider (ADR-003: DirectML/CoreML/CPU).
    ActiveProvider ap = resolve_provider(provider_);
    bool ep_ok = false;

#if defined(_WIN32)
    if (ap == ActiveProvider::DirectML) {
        // DirectML EP: device index 0 = default GPU.
        // OrtSessionOptionsAppendExecutionProvider_DML is declared in
        // onnxruntime_c_api.h when DirectML support is compiled in.
        st = g_ort->SessionOptionsAppendExecutionProvider_DML(opts, 0);
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
        // CoreML EP: flag 1 = enable ANE (Apple Neural Engine).
        st = g_ort->SessionOptionsAppendExecutionProvider_CoreML(opts, 1);
        if (st) {
            blog(LOG_WARNING, "[DanceHAP] MatteEngine: CoreML EP failed, falling back to CPU");
            g_ort->ReleaseStatus(st);
            ap = ActiveProvider::CPU;
        } else {
            ep_ok = true;
        }
    }
#endif
    (void)ep_ok;  // CPU is the default; no explicit append needed.

    pimpl_->active = ap;
    const char *provider_name =
        (ap == ActiveProvider::DirectML) ? "DirectML" :
        (ap == ActiveProvider::CoreML)   ? "CoreML"   : "CPU";
    blog(LOG_INFO, "[DanceHAP] MatteEngine: using %s provider", provider_name);

    // Create session.
    st = g_ort->CreateSession(pimpl_->env, model_path.c_str(), opts,
                              &pimpl_->session);
    g_ort->ReleaseSessionOptions(opts);
    if (st) {
        const char *msg = g_ort->GetErrorMessage(st);
        blog(LOG_WARNING, "[DanceHAP] MatteEngine: CreateSession failed: %s",
             msg ? msg : "(unknown)");
        g_ort->ReleaseStatus(st);
        return false;
    }

    blog(LOG_INFO, "[DanceHAP] MatteEngine: model loaded '%s'", model_path.c_str());
    return true;
}

bool MatteEngine::isReady() const
{
    return pimpl_->session != nullptr;
}

ActiveProvider MatteEngine::activeProvider() const
{
    return pimpl_->active;
}

// ---------------------------------------------------------------------------
// Inference
// ---------------------------------------------------------------------------

MatteMask MatteEngine::infer(const ImageFrame &input)
{
    if (!isReady() || !input.data_rgba || input.width <= 0 || input.height <= 0)
        return MatteMask{};

    const int in_w = config_.input_width;
    const int in_h = config_.input_height;

    // Preprocess: RGBA → RGB normalized CHW float32.
    std::vector<float> input_tensor =
        preprocess_rgba_to_rgb_normalized(input, in_w, in_h);
    if (input_tensor.empty()) return MatteMask{};

    // Create input OrtValue (tensor [1,3,in_w,in_h] float32).
    OrtMemoryInfo *mem_info;
    OrtStatus *st = g_ort->CreateCpuMemoryInfo(
        OrtArenaAllocator, OrtMemTypeDefault, &mem_info);
    if (st) {
        g_ort->ReleaseStatus(st);
        return MatteMask{};
    }

    int64_t input_shape[4] = {1, 3, in_h, in_w};
    OrtValue *input_value = nullptr;
    st = g_ort->CreateTensorWithDataAsOrtValue(
        mem_info,
        input_tensor.data(),
        input_tensor.size() * sizeof(float),
        input_shape, 4,
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
        &input_value);
    g_ort->ReleaseMemoryInfo(mem_info);
    if (st) {
        blog(LOG_WARNING, "[DanceHAP] MatteEngine: CreateTensorWithData failed");
        g_ort->ReleaseStatus(st);
        return MatteMask{};
    }

    // Run inference.
    const char *input_names[]  = {"src"};
    const char *output_names[] = {"fgr", "pha"};
    OrtValue *output_values[2] = {nullptr, nullptr};

    st = g_ort->Run(pimpl_->session, nullptr,
        input_names, const_cast<const OrtValue *const *>(&input_value), 1,
        output_names, 2, output_values);

    g_ort->ReleaseValue(input_value);

    if (st) {
        const char *msg = g_ort->GetErrorMessage(st);
        blog(LOG_WARNING, "[DanceHAP] MatteEngine: Run failed: %s",
             msg ? msg : "(unknown)");
        g_ort->ReleaseStatus(st);
        return MatteMask{};
    }

    // Extract "pha" output [1,1,H,W] → resize to original dims.
    OrtValue *pha_value = output_values[1];
    OrtTensorTypeAndShapeInfo *pha_info;
    st = g_ort->GetTensorTypeAndShape(pha_value, &pha_info);
    if (st) {
        g_ort->ReleaseStatus(st);
        g_ort->ReleaseValue(output_values[0]);
        g_ort->ReleaseValue(output_values[1]);
        return MatteMask{};
    }

    size_t dim_count = 0;
    g_ort->GetDimensionsCount(pha_info, &dim_count);
    int64_t dims[4] = {0, 0, 0, 0};
    if (dim_count <= 4) {
        g_ort->GetDimensionsShape(pha_info, dims, dim_count);
    }
    g_ort->ReleaseTensorTypeAndShapeInfo(pha_info);

    int pha_h = static_cast<int>(dims[2] > 0 ? dims[2] : in_h);
    int pha_w = static_cast<int>(dims[3] > 0 ? dims[3] : in_w);

    float *pha_data = nullptr;
    st = g_ort->GetTensorMutableData(pha_value, reinterpret_cast<void **>(&pha_data));
    if (st || !pha_data) {
        if (st) g_ort->ReleaseStatus(st);
        g_ort->ReleaseValue(output_values[0]);
        g_ort->ReleaseValue(output_values[1]);
        return MatteMask{};
    }

    MatteMask mask = postprocess_pha_to_mask(
        pha_data, pha_w, pha_h, input.width, input.height);

    g_ort->ReleaseValue(output_values[0]);
    g_ort->ReleaseValue(output_values[1]);

    return mask;
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

MatteMask MatteEngine::infer(const ImageFrame & /*input*/)
{
    // Stub: returns empty mask.
    return MatteMask{};
}

} // namespace dancehap

#endif // DANCEHAP_HAVE_ONNXRUNTIME