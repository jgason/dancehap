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
#include "hap_demuxer.hpp"
#include "hap_decoder.hpp"
#include "dancehap/version.h"

#include <cstring>
#include <string>
#include <memory>

// ---------------------------------------------------------------------------
// Per-instance context
// ---------------------------------------------------------------------------

namespace {

struct hap_clip_context {
    std::string file_path;
    bool loop      = true;
    bool autoplay  = false;
    bool active    = false;

    // Phase 1.2: container demuxer.
    // Phase 1.3: HAP decoder.
    // Owned via unique_ptr so the context stays movable/copyable-friendly
    // and resources are destroyed deterministically in hap_clip_destroy.
    std::unique_ptr<dancehap::HapDemuxer> demuxer;
    std::unique_ptr<dancehap::HapDecoder> decoder;
    dancehap::DemuxState demux_state = dancehap::DemuxState::Idle;

    /// Open (or re-open) the demuxer on the current file_path.
    /// On failure: logs a warning and leaves the source valid but without
    /// video/audio — never crashes OBS.
    void open_demuxer()
    {
        demux_state = dancehap::DemuxState::Loading;
        if (!demuxer) demuxer = std::make_unique<dancehap::HapDemuxer>();

        if (file_path.empty()) {
            demux_state = dancehap::DemuxState::Idle;
            return;
        }

        if (!demuxer->open(file_path)) {
            blog(LOG_WARNING, "[DanceHAP] failed to open clip '%s': %s",
                 file_path.c_str(), demuxer->getLastError().c_str());
            demux_state = dancehap::DemuxState::Error;
            return;
        }

        demux_state = demuxer->getState();
        const auto &vi = demuxer->getVideoInfo();
        blog(LOG_INFO, "[DanceHAP] clip opened: %s %dx%d %.3fs (audio=%s)",
             dancehap::hap_variant_to_string(vi.variant),
             vi.width, vi.height, demuxer->getDuration(),
             demuxer->hasAudio() ? "yes" : "no");

        // Phase 1.3: create decoder with video info from demuxer.
        decoder = std::make_unique<dancehap::HapDecoder>();
        decoder->setVideoInfo(vi);
    }

    void close_demuxer()
    {
        decoder.reset();  // Phase 1.3: release decoder + texture
        if (demuxer) demuxer->close();
        demux_state = dancehap::DemuxState::Idle;
    }
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

    // Phase 1.2: open the container if a path was provided.
    ctx->open_demuxer();

    return ctx;
}

void hap_clip_destroy(void *data)
{
    auto *ctx = static_cast<hap_clip_context *>(data);
    if (!ctx) return;
    blog(LOG_INFO, "[DanceHAP] hap_clip_source destroyed (path='%s')",
         ctx->file_path.c_str());

    // Phase 1.2: release demuxer resources before freeing the context.
    ctx->close_demuxer();

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
    if (path && ctx->file_path != path) {
        blog(LOG_INFO, "[DanceHAP] path changed: '%s' -> '%s'",
             ctx->file_path.c_str(), path);
        ctx->file_path = path;

        // Phase 1.2: close old demuxer, open new file.
        ctx->close_demuxer();
        ctx->open_demuxer();
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

void hap_clip_video_tick(void *data, float /*seconds*/)
{
    auto *ctx = static_cast<hap_clip_context *>(data);
    if (!ctx || !ctx->demuxer || !ctx->decoder) return;
    if (ctx->demux_state != dancehap::DemuxState::Ready) return;

    // Phase 1.3: read next video packet and decode it.
    // Phase 1.4 will add proper FPS timing + loop + A/V sync.
    dancehap::DemuxPacket pkt = ctx->demuxer->readNextVideoPacket();
    if (pkt.valid) {
        ctx->decoder->decode(pkt);
    }
    // EOF / loop handling arrives in Phase 1.4.
}

void hap_clip_video_render(void *data, gs_effect_t *effect)
{
    auto *ctx = static_cast<hap_clip_context *>(data);
    if (!ctx || !ctx->decoder) return;

    gs_texture_t *tex = ctx->decoder->getTexture();
    if (!tex) return;  // stub mode or no decoded frame yet

#ifdef DANCEHAP_HAVE_OBS
    // Draw the decoded HAP texture using the default OBS effect.
    gs_effect_set_texture(
        gs_effect_get_param_by_name(effect, "image"), tex);
    gs_draw_sprite(tex, 0,
                   ctx->decoder->getWidth(),
                   ctx->decoder->getHeight());
#endif
    // Stub mode: no OBS graphics API — safe no-op.
}

// ---------------------------------------------------------------------------
// Dimensions — report clip size from the demuxer (Phase 1.2).
// If no clip is loaded, return 0 (OBS will skip rendering).
// ---------------------------------------------------------------------------

uint32_t hap_clip_get_width(void *data)
{
    auto *ctx = static_cast<hap_clip_context *>(data);
    if (!ctx || !ctx->demuxer || !ctx->demuxer->hasVideo())
        return 0;
    return (uint32_t)ctx->demuxer->getVideoInfo().width;
}

uint32_t hap_clip_get_height(void *data)
{
    auto *ctx = static_cast<hap_clip_context *>(data);
    if (!ctx || !ctx->demuxer || !ctx->demuxer->hasVideo())
        return 0;
    return (uint32_t)ctx->demuxer->getVideoInfo().height;
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
    info.get_width      = hap_clip_get_width;   // Phase 1.2: report clip dimensions
    info.get_height     = hap_clip_get_height;
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
