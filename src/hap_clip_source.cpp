// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// hap_clip_source.cpp — OBS source registration + property view.
//
// Phase 1.1 deliverables (PLAN-PHASE-1.md étape 1.1):
//   • obs_source_info with id "dancehap_hap_clip"
//   • get_name → "DanceHAP Clip"
//   • create / destroy (alloc/free source context)
//   • get_defaults (path="", loop=true, autoplay=false)
//   • get_properties (file path + loop toggle + autoplay toggle)
//   • update (re-read settings on change)
//   • activate / deactivate (log)
//   • video_tick / video_render (safe no-ops — black output)
//   • Registration via obs_register_source in register_hap_clip_source()
//
// output_flags justification:
//   VIDEO            — the source produces a video texture (HAP frames)
//   AUDIO            — the source produces audio (clip's audio track)
//   CUSTOM_DRAW      — rendering is handled via our own gs_texture upload,
//                      not OBS's default effect-based draw
//   DO_NOT_DUPLICATE — OBS should not auto-duplicate this source for
//                      monitoring/preview; A/V sync is managed internally

#include "hap_clip_source.hpp"
#include "dancehap/version.h"

#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// Per-instance context
// ---------------------------------------------------------------------------

namespace {

struct hap_clip_context {
    std::string file_path;
    bool loop      = true;
    bool autoplay  = false;
    bool active    = false;
};

// ---------------------------------------------------------------------------
// Callback implementations
// ---------------------------------------------------------------------------

const char *hap_clip_get_name(void * /*type_data*/)
{
    return HAP_CLIP_SOURCE_NAME;
}

void *hap_clip_create(obs_data_t *settings, obs_source_t * /*source*/)
{
    auto *ctx = new hap_clip_context();
    if (settings) {
        const char *path = obs_data_get_string(settings, "path");
        if (path) ctx->file_path = path;
        ctx->loop     = obs_data_get_bool(settings, "loop");
        ctx->autoplay = obs_data_get_bool(settings, "autoplay");
    }
    blog(LOG_INFO, "[DanceHAP] hap_clip_source created "
                   "(path='%s', loop=%d, autoplay=%d)",
         ctx->file_path.c_str(), (int)ctx->loop, (int)ctx->autoplay);
    return ctx;
}

void hap_clip_destroy(void *data)
{
    auto *ctx = static_cast<hap_clip_context *>(data);
    if (!ctx) return;
    blog(LOG_INFO, "[DanceHAP] hap_clip_source destroyed (path='%s')",
         ctx->file_path.c_str());
    delete ctx;
}

void hap_clip_get_defaults(obs_data_t *settings)
{
    if (!settings) return;
    obs_data_set_default_string(settings, "path", "");
    obs_data_set_default_bool(settings, "loop", true);
    obs_data_set_default_bool(settings, "autoplay", false);
}

obs_properties_t *hap_clip_get_properties(void * /*data*/)
{
    obs_properties_t *props = obs_properties_create();
    if (!props) return nullptr;

    // File picker — HAP clips in MOV/MP4 containers.
    obs_properties_add_path(
        props,
        "path",
        "Clip file path",
        OBS_PATH_FILE,
        "HAP clips (*.mov *.mp4)",
        "");

    // Loop toggle.
    obs_properties_add_bool(props, "loop", "Loop");

    // Autoplay toggle.
    obs_properties_add_bool(props, "autoplay", "Autoplay");

    return props;
}

void hap_clip_update(void *data, obs_data_t *settings)
{
    auto *ctx = static_cast<hap_clip_context *>(data);
    if (!ctx || !settings) return;

    const char *path = obs_data_get_string(settings, "path");
    if (path && path != ctx->file_path) {
        blog(LOG_INFO, "[DanceHAP] path changed: '%s' → '%s'",
             ctx->file_path.c_str(), path);
        ctx->file_path = path;
        // Phase 1.2+: trigger reload of HAP decoder here.
    }
    ctx->loop     = obs_data_get_bool(settings, "loop");
    ctx->autoplay = obs_data_get_bool(settings, "autoplay");
}

void hap_clip_activate(void *data)
{
    auto *ctx = static_cast<hap_clip_context *>(data);
    if (!ctx) return;
    ctx->active = true;
    blog(LOG_INFO, "[DanceHAP] hap_clip_source activated");
}

void hap_clip_deactivate(void *data)
{
    auto *ctx = static_cast<hap_clip_context *>(data);
    if (!ctx) return;
    ctx->active = false;
    blog(LOG_INFO, "[DanceHAP] hap_clip_source deactivated");
}

void hap_clip_video_tick(void * /*data*/, float /*seconds*/)
{
    // Phase 1.4: advance HAP playback, update audio sync.
}

void hap_clip_video_render(void * /*data*/, gs_effect_t * /*effect*/)
{
    // Phase 1.3-1.4: upload decoded HAP texture and draw.
    // Safe no-op for now — OBS clears the frame to black by default.
}

// ---------------------------------------------------------------------------
// Build the obs_source_info struct
// ---------------------------------------------------------------------------

struct obs_source_info build_hap_clip_info()
{
    struct obs_source_info info = {};   // zero-initialise

    info.id           = HAP_CLIP_SOURCE_ID;
    info.type         = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_VIDEO
                      | OBS_SOURCE_AUDIO
                      | OBS_SOURCE_CUSTOM_DRAW
                      | OBS_SOURCE_DO_NOT_DUPLICATE;

    info.get_name       = hap_clip_get_name;
    info.create         = hap_clip_create;
    info.destroy        = hap_clip_destroy;
    info.get_width      = nullptr;   // Phase 1.4: report clip dimensions
    info.get_height     = nullptr;
    info.get_defaults   = hap_clip_get_defaults;
    info.get_properties = hap_clip_get_properties;
    info.update         = hap_clip_update;
    info.activate       = hap_clip_activate;
    info.deactivate     = hap_clip_deactivate;
    info.show           = nullptr;
    info.hide           = nullptr;
    info.video_tick     = hap_clip_video_tick;
    info.video_render   = hap_clip_video_render;

    return info;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

const struct obs_source_info *hap_clip_source_get_info(void)
{
    static const struct obs_source_info s_info = build_hap_clip_info();
    return &s_info;
}

void register_hap_clip_source(void)
{
    const struct obs_source_info *info = hap_clip_source_get_info();
    blog(LOG_INFO, "[DanceHAP] registering source '%s' (v%s)",
         info->id, DANCEHAP_VERSION_STRING);
    obs_register_source(info);
}
