// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// ai_matte_filter.cpp — OBS filter that applies AI-based matting to a webcam
// source.
//
// Phase 2.0: skeleton — the filter is registered as an OBS_SOURCE_TYPE_FILTER
// with OBS_SOURCE_CAP_VIDEO. It is a pass-through: video_render draws the
// target (parent) source without modification. The actual matting inference
// (RVM / MediaPipe) arrives in Phase 2.2 / 2.4.
//
// In stub mode (no real OBS), the filter compiles and the obs_source_info is
// inspectable by unit tests. video_tick and video_render are safe no-ops.
//
// Reference: obs-studio/plugins/obs-filters/ (e.g. noise-filter.c, mask-filter.c)
//   • A filter has type OBS_SOURCE_TYPE_FILTER, output_flags = OBS_SOURCE_VIDEO.
//   • It is applied on top of an existing video source (webcam).
//   • The filter's video_render() draws the target source via
//     obs_source_default_render() when OBS_SOURCE_CUSTOM_DRAW is not set.

#include "ai_matte_filter.hpp"
#include "dancehap/version.h"
#include "matte_engine.hpp"

#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// Per-instance context
// ---------------------------------------------------------------------------

namespace dancehap {

/// Apply an alpha mask to a BGRA frame.
/// Multiplies each pixel's alpha channel by the corresponding mask value.
std::vector<uint8_t> apply_alpha_mask_to_bgra(
    const uint8_t *bgra_data, uint32_t width, uint32_t height,
    const std::vector<float> &mask)
{
    std::vector<uint8_t> out;
    if (!bgra_data || width == 0 || height == 0) return out;

    size_t pixel_count = static_cast<size_t>(width) * height;
    out.resize(pixel_count * 4);

    // If mask is empty or wrong size, pass through unchanged (no matting).
    bool mask_valid = (mask.size() == pixel_count);

    for (size_t i = 0; i < pixel_count; ++i) {
        out[i * 4 + 0] = bgra_data[i * 4 + 0];  // B
        out[i * 4 + 1] = bgra_data[i * 4 + 1];  // G
        out[i * 4 + 2] = bgra_data[i * 4 + 2];  // R
        // Alpha: original alpha * mask value (clamped).
        float a = mask_valid ? mask[i] : 1.0f;
        if (a < 0.0f) a = 0.0f;
        if (a > 1.0f) a = 1.0f;
        out[i * 4 + 3] = static_cast<uint8_t>(
            static_cast<float>(bgra_data[i * 4 + 3]) * a);
    }
    return out;
}

} // namespace dancehap

// ---------------------------------------------------------------------------
// Per-instance context
// ---------------------------------------------------------------------------

namespace {

struct ai_matte_context {
    bool active = false;
    bool matte_enabled = false;       // user toggle (Phase 2.5 properties)
    std::string model_path;           // ONNX model path (current setting)
    std::string loaded_model_path;    // ONNX model path (actually loaded)

#ifdef DANCEHAP_HAVE_ONNXRUNTIME
    dancehap::MatteEngine engine;     // matting inference engine
#endif

#ifdef DANCEHAP_HAVE_OBS
    obs_source_t *source = nullptr;   // the filter's own OBS source handle
#endif

    // Last processed frame (for render). Owned by this context.
    std::vector<uint8_t> processed_frame;
    uint32_t processed_width  = 0;
    uint32_t processed_height = 0;
};

// ---------------------------------------------------------------------------
// Callback implementations
// ---------------------------------------------------------------------------

const char *ai_matte_get_name(void * /*type_data*/)
{
    return AI_MATTE_FILTER_NAME " v" DANCEHAP_VERSION_STRING;
}

void *ai_matte_create(obs_data_t * /*settings*/, obs_source_t *source)
{
    auto *ctx = new ai_matte_context();
#ifdef DANCEHAP_HAVE_OBS
    ctx->source = source;
#endif
    blog(LOG_INFO, "[DanceHAP] ai_matte_filter created");
    return ctx;
}

void ai_matte_destroy(void *data)
{
    auto *ctx = static_cast<ai_matte_context *>(data);
    if (!ctx) return;
    blog(LOG_INFO, "[DanceHAP] ai_matte_filter destroyed");
    delete ctx;
}

void ai_matte_get_defaults(obs_data_t *settings)
{
    if (!settings) return;
    obs_data_set_default_bool(settings, "matte_enable", false);
    obs_data_set_default_string(settings, "matte_model_path", "");
    obs_data_set_default_int(settings, "matte_provider", 0);  // Auto
    obs_data_set_default_int(settings, "matte_quality", 1);   // Balanced
}

obs_properties_t *ai_matte_get_properties(void * /*data*/)
{
    obs_properties_t *props = obs_properties_create();

    // Enable toggle.
    obs_properties_add_bool(props, "matte_enable",
        obs_module_text("DanceHAP.Matte.Enable"));

    // Model path (.onnx file).
    obs_properties_add_path(props, "matte_model_path",
        obs_module_text("DanceHAP.Matte.ModelPath"),
        OBS_PATH_FILE, "ONNX model (*.onnx)", nullptr);

    // Execution provider.
    obs_property_t *prop_prov = obs_properties_add_list(props,
        "matte_provider",
        obs_module_text("DanceHAP.Matte.Provider"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(prop_prov,
        obs_module_text("DanceHAP.Matte.Provider.Auto"), 0);
    obs_property_list_add_int(prop_prov,
        obs_module_text("DanceHAP.Matte.Provider.DirectML"), 1);
    obs_property_list_add_int(prop_prov,
        obs_module_text("DanceHAP.Matte.Provider.CoreML"), 2);
    obs_property_list_add_int(prop_prov,
        obs_module_text("DanceHAP.Matte.Provider.CPU"), 3);

    // Quality (internal resolution).
    obs_property_t *prop_qual = obs_properties_add_list(props,
        "matte_quality",
        obs_module_text("DanceHAP.Matte.Quality"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(prop_qual,
        obs_module_text("DanceHAP.Matte.Quality.Performance"), 0);  // 192px
    obs_property_list_add_int(prop_qual,
        obs_module_text("DanceHAP.Matte.Quality.Balanced"), 1);    // 256px
    obs_property_list_add_int(prop_qual,
        obs_module_text("DanceHAP.Matte.Quality.High"), 2);        // 512px

    return props;
}

void ai_matte_update(void *data, obs_data_t *settings)
{
    auto *ctx = static_cast<ai_matte_context *>(data);
    if (!ctx) return;

    bool enable = obs_data_get_bool(settings, "matte_enable");
    const char *model = obs_data_get_string(settings, "matte_model_path");
    int provider = static_cast<int>(obs_data_get_int(settings, "matte_provider"));
    int quality  = static_cast<int>(obs_data_get_int(settings, "matte_quality"));

    ctx->matte_enabled = enable;
    ctx->model_path = model ? model : "";

    // Map quality index → internal resolution.
    int res = (quality == 0) ? 192 : (quality == 2) ? 512 : 256;

#ifdef DANCEHAP_HAVE_ONNXRUNTIME
    // Update engine config.
    dancehap::MatteModelConfig cfg;
    cfg.input_width  = res;
    cfg.input_height = res;
    ctx->engine.setConfig(cfg);

    dancehap::ExecutionProvider ep =
        (provider == 1) ? dancehap::ExecutionProvider::DirectML :
        (provider == 2) ? dancehap::ExecutionProvider::CoreML :
        (provider == 3) ? dancehap::ExecutionProvider::CPU :
                          dancehap::ExecutionProvider::Auto;
    ctx->engine.setDesiredProvider(ep);

    // (Re)load model if enabled and path changed.
    if (enable && !ctx->model_path.empty()) {
        if (!ctx->engine.isReady() || ctx->model_path != ctx->loaded_model_path) {
            if (!ctx->engine.loadModel(ctx->model_path)) {
                blog(LOG_WARNING, "[DanceHAP] MatteEngine: failed to load '%s'",
                     ctx->model_path.c_str());
                ctx->matte_enabled = false;
            } else {
                ctx->loaded_model_path = ctx->model_path;
            }
        }
    }
    // Disable engine if matte toggled off or path cleared.
    if (!enable && ctx->engine.isReady()) {
        // Engine stays loaded but matte_enabled=false → video_tick skips it.
    }
#endif
}

void ai_matte_video_tick(void *data, float /*seconds*/)
{
    auto *ctx = static_cast<ai_matte_context *>(data);
    if (!ctx) return;

#ifdef DANCEHAP_HAVE_OBS
    if (!ctx->matte_enabled) return;

    // Get the target (parent) source — the webcam we're filtering.
    obs_source_t *target = obs_filter_get_target(ctx->source);
    if (!target) return;

    // Get the latest frame from the target source (async video API).
    obs_source_frame *frame = obs_source_get_frame(target);
    if (!frame) return;

    uint32_t w = frame->width;
    uint32_t h = frame->height;

#ifdef DANCEHAP_HAVE_ONNXRUNTIME
    // Run matting inference if the engine is ready.
    if (ctx->engine.isReady() && w > 0 && h > 0) {
        // Build ImageFrame. OBS frames are typically BGRA or RGBA.
        // We pass the data as RGBA (the preprocess handles normalization).
        dancehap::ImageFrame img;
        img.width  = static_cast<int>(w);
        img.height = static_cast<int>(h);
        img.data_rgba = frame->data[0];  // plane 0

        dancehap::MatteMask mask = ctx->engine.infer(img);
        if (mask.width == static_cast<int>(w) && mask.height == static_cast<int>(h)) {
            // Apply mask to the frame's alpha channel.
            // Note: OBS frames may not have alpha. We produce a processed
            // copy that the render callback will use.
            ctx->processed_frame = dancehap::apply_alpha_mask_to_bgra(
                frame->data[0], w, h, mask.alpha);
            ctx->processed_width  = w;
            ctx->processed_height = h;
        }
    }
#endif // DANCEHAP_HAVE_ONNXRUNTIME

    obs_source_release_frame(target, frame);
#endif // DANCEHAP_HAVE_OBS
}

void ai_matte_video_render(void *data, gs_effect_t * /*effect*/)
{
    auto *ctx = static_cast<ai_matte_context *>(data);
    if (!ctx) return;

#ifdef DANCEHAP_HAVE_OBS
    // Pass-through: draw the target (parent) source using OBS's default
    // render path. Without OBS_SOURCE_CUSTOM_DRAW, OBS wraps our
    // video_render() in the default effect's Draw technique — we just need
    // to call obs_source_default_render() on the filter's target.
    //
    // NOTE: obs_source_default_render() is the standard way for a filter to
    // pass through the parent source's video. It binds the target texture
    // and draws it. This is the exact pattern used by simple pass-through
    // filters in obs-studio (e.g. crop-filter.c, mask-filter.c).
    obs_source_t *target = obs_filter_get_target(ctx->source);
    if (target) {
        obs_source_default_render(target);
    }
#else
    // Stub mode: no OBS graphics API — safe no-op.
#endif
}

// ---------------------------------------------------------------------------
// Build the obs_source_info struct
// ---------------------------------------------------------------------------

struct obs_source_info build_ai_matte_info()
{
    struct obs_source_info info = {};   // zero-initialise

    info.id           = AI_MATTE_FILTER_ID;
    info.type         = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_VIDEO;

    info.get_name       = ai_matte_get_name;
    info.create         = ai_matte_create;
    info.destroy        = ai_matte_destroy;
    info.get_defaults   = ai_matte_get_defaults;
    info.get_properties = ai_matte_get_properties;
    info.update         = ai_matte_update;
    info.video_tick     = ai_matte_video_tick;
    info.video_render   = ai_matte_video_render;

    // Filters typically do not define get_width/get_height — OBS uses the
    // target source's dimensions. In the stub struct these are left null.
    info.get_width      = nullptr;
    info.get_height     = nullptr;

    return info;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

const struct obs_source_info *ai_matte_filter_get_info(void)
{
    static const struct obs_source_info s_info = build_ai_matte_info();
    return &s_info;
}

void register_ai_matte_filter(void)
{
    const struct obs_source_info *info = ai_matte_filter_get_info();
    blog(LOG_INFO, "[DanceHAP] registering filter '%s' (v%s)",
         info->id, DANCEHAP_VERSION_STRING);
    obs_register_source(info);
}

// ---------------------------------------------------------------------------
// Matte frame processing (testable in stub mode)
// ---------------------------------------------------------------------------
// This function applies the full matting pipeline to a BGRA frame:
//   1. If a MatteEngine is provided and ready, run inference → alpha mask.
//   2. Apply the mask to the frame's alpha channel.
//   3. Return the processed frame (BGRA with alpha).
// If no engine or engine not ready, returns the input unmodified (pass-through).

const void *ai_matte_filter_process_frame(const void *input_data,
                                           uint32_t width,
                                           uint32_t height,
                                           uint32_t *out_width,
                                           uint32_t *out_height)
{
    if (out_width)  *out_width  = width;
    if (out_height) *out_height = height;

#ifdef DANCEHAP_HAVE_ONNXRUNTIME
    // If an engine is available, apply the matting pipeline.
    // Note: in this standalone function we don't have a context, so the
    // actual integration happens in ai_matte_video_tick via the context's
    // engine. This function remains a pass-through for unit tests.
#endif

    // Pass-through: output = input, dimensions unchanged.
    return input_data;
}