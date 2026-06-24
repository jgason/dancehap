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

#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// Per-instance context
// ---------------------------------------------------------------------------

namespace {

struct ai_matte_context {
    bool active = false;

#ifdef DANCEHAP_HAVE_OBS
    obs_source_t *source = nullptr;  // the filter's own OBS source handle
#endif

    // Phase 2.0: no matting engine yet. Fields for matte settings will be
    // added in Phase 2.2 (enable toggle, model selection, etc.).
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
    // Phase 2.0: no settings yet. Phase 2.2 will add "enable_matte" toggle.
}

obs_properties_t *ai_matte_get_properties(void * /*data*/)
{
    obs_properties_t *props = obs_properties_create();
    // Phase 2.0: no properties yet. Phase 2.2/2.5 will add model selection,
    // quality, softness, etc.
    return props;
}

void ai_matte_update(void *data, obs_data_t * /*settings*/)
{
    auto *ctx = static_cast<ai_matte_context *>(data);
    if (!ctx) return;
    // Phase 2.0: no settings to apply yet.
}

void ai_matte_video_tick(void *data, float /*seconds*/)
{
    auto *ctx = static_cast<ai_matte_context *>(data);
    if (!ctx) return;
    // Phase 2.0: pass-through — no processing in video_tick.
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
// Pass-through frame processing (testable in stub mode)
// ---------------------------------------------------------------------------
// This function is the pure logic that a pass-through filter applies to a
// frame: it returns the input data pointer unmodified. In Phase 2.2 this
// will be replaced by the actual matting pipeline (preprocess → infer →
// postprocess → composite). Exposing it here allows unit tests to verify
// the pass-through contract without a running OBS instance.

const void *ai_matte_filter_process_frame(const void *input_data,
                                           uint32_t width,
                                           uint32_t height,
                                           uint32_t *out_width,
                                           uint32_t *out_height)
{
    // Pass-through: output = input, dimensions unchanged.
    if (out_width)  *out_width  = width;
    if (out_height) *out_height = height;
    return input_data;
}