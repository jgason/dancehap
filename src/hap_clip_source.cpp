// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// hap_clip_source.cpp — OBS source registration + property view + playback.
//
// Phase 1.4: refactored to delegate playback to ClipPlayer (1.4.1-1.4.4).
//   • hap_clip_context now owns a dancehap::ClipPlayer instead of raw
//     demuxer + decoder pointers.
//   • video_tick drives the ClipPlayer (tick + audio pull/push).
//   • video_render draws the ClipPlayer's decoded texture.
//   • Audio is pushed to OBS via obs_source_output_audio_data (real OBS
//     mode only). In stub mode, the player still advances its clock from
//     pullAudio so timing logic is exercised.
//
// Phase 1.1 deliverables (preserved):
//   • obs_source_info with id "dancehap_hap_clip"
//   • get_name → "DanceHAP Clip"
//   • get_defaults (path="", loop=true, autoplay=false)
//   • get_properties (file path + loop toggle + autoplay toggle)
//   • activate / deactivate
//   • Registration via obs_register_source
//
// output_flags justification:
//   VIDEO            — produces a video texture (HAP frames)
//   AUDIO            — produces audio (clip's audio track)
//   CUSTOM_DRAW      — rendering via our own gs_texture upload
//   DO_NOT_DUPLICATE — A/V sync managed internally

#include "hap_clip_source.hpp"
#include "clip_player.hpp"
#include "dancehap/version.h"

#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------+
// OBS audio output types (real OBS mode only)
// ---------------------------------------------------------------------------+
#ifdef DANCEHAP_HAVE_OBS
// Pull in the OBS headers that provide the audio output API + high-res clock.
// obs_source_output_audio() and struct obs_source_audio live in obs.h (already
// included via obs_compat.hpp), but os_gettime_ns() needs util/platform.h.
#include <util/platform.h>
#endif

// ---------------------------------------------------------------------------+
// Per-instance context
// ---------------------------------------------------------------------------+

namespace {

// Audio chunk size pulled per video_tick call (~21 ms at 48 kHz ≈ 1008 frames).
// This matches OBS's typical audio processing block size.
static constexpr int64_t AUDIO_CHUNK_US = 21'000;

struct hap_clip_context {
    std::string file_path;
    bool loop      = true;
    bool autoplay  = false;
    bool active    = false;

    // Phase 1.4: ClipPlayer owns demuxer + decoder + timing + loop logic.
    dancehap::ClipPlayer player;

#ifdef DANCEHAP_HAVE_OBS
    obs_source_t *source = nullptr;  // for obs_source_output_audio_data

    /// Push interleaved float audio to OBS as planar float.
    /// Converts AudioOutput.samples (interleaved) → obs_source_audio (planar).
    void push_audio(const dancehap::AudioOutput &audio)
    {
        if (!source || !audio.valid || audio.frames <= 0) return;

        // Planar buffers (one per channel). Allocate on each call — small
        // (frames * channels * 4 bytes, typically < 4 KB).
        std::vector<float> planar[8];
        int ch = audio.channels < 8 ? audio.channels : 8;
        for (int c = 0; c < ch; ++c) {
            planar[c].resize(audio.frames);
            for (int i = 0; i < audio.frames; ++i) {
                planar[c][i] = audio.samples[
                    static_cast<size_t>(i) * audio.channels + c];
            }
        }

        struct obs_source_audio osa = {};
        osa.format         = AUDIO_FORMAT_FLOAT_PLANAR;
        osa.samples_per_sec = static_cast<uint32_t>(audio.sample_rate);
        osa.frames         = static_cast<uint32_t>(audio.frames);
        osa.timestamp      = os_gettime_ns();

        switch (ch) {
        case 1:  osa.speakers = SPEAKERS_MONO;     break;
        case 2:  osa.speakers = SPEAKERS_STEREO;   break;
        case 3:  osa.speakers = SPEAKERS_2POINT1;  break;
        case 4:  osa.speakers = SPEAKERS_4POINT0;  break;
        case 5:  osa.speakers = SPEAKERS_4POINT1;  break;
        case 6:  osa.speakers = SPEAKERS_5POINT1;  break;
        case 7:  osa.speakers = SPEAKERS_5POINT1;  break;  // OBS max layout
        default: osa.speakers = SPEAKERS_STEREO;   break;
        }

        for (int c = 0; c < ch; ++c) {
            // OBS audio planes are typed const uint8_t* regardless of sample
            // format; for FLOAT_PLANAR we reinterpret the float buffer.
            osa.data[c] = reinterpret_cast<const uint8_t *>(planar[c].data());
        }

        obs_source_output_audio(source, &osa);
    }
#endif // DANCEHAP_HAVE_OBS

    /// Load (or reload) the clip at file_path into the player.
    void load_clip()
    {
        player.setLoop(loop);
        if (file_path.empty()) return;

        if (!player.load(file_path)) {
            blog(LOG_WARNING, "[DanceHAP] failed to load clip '%s': %s",
                 file_path.c_str(), player.getLastError().c_str());
        }
    }

    void unload_clip()
    {
        player.stop();
    }
};

// ---------------------------------------------------------------------------
// Callback implementations
// ---------------------------------------------------------------------------

const char *hap_clip_get_name(void * /*type_data*/)
{
    // Embed the version directly in the source name so it shows up in the
    // OBS "Add source" menu, the scene source list, and the properties
    // window title. This lets the operator confirm at a glance which DLL
    // version is actually loaded — critical when iterating on smoke tests.
    return HAP_CLIP_SOURCE_NAME " v" DANCEHAP_VERSION_STRING;
}

void *hap_clip_create(obs_data_t *settings, obs_source_t *source)
{
    auto *ctx = new hap_clip_context();
#ifdef DANCEHAP_HAVE_OBS
    ctx->source = source;
#endif
    if (settings) {
        const char *path = obs_data_get_string(settings, "path");
        if (path) ctx->file_path = path;
        ctx->loop     = obs_data_get_bool(settings, "loop");
        ctx->autoplay = obs_data_get_bool(settings, "autoplay");
    }
    blog(LOG_INFO, "[DanceHAP] hap_clip_source created "
                   "(path='%s', loop=%d, autoplay=%d)",
         ctx->file_path.c_str(), (int)ctx->loop, (int)ctx->autoplay);

    // Phase 1.4: load the clip via ClipPlayer.
    ctx->load_clip();

    return ctx;
}

void hap_clip_destroy(void *data)
{
    auto *ctx = static_cast<hap_clip_context *>(data);
    if (!ctx) return;
    blog(LOG_INFO, "[DanceHAP] hap_clip_source destroyed (path='%s')",
         ctx->file_path.c_str());

    ctx->unload_clip();
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

    obs_properties_add_path(
        props,
        "path",
        "Clip file path",
        OBS_PATH_FILE,
        "HAP clips (*.mov *.mp4)",
        "");

    obs_properties_add_bool(props, "loop", "Loop");
    obs_properties_add_bool(props, "autoplay", "Autoplay");

    return props;
}

void hap_clip_update(void *data, obs_data_t *settings)
{
    auto *ctx = static_cast<hap_clip_context *>(data);
    if (!ctx || !settings) return;

    const char *path = obs_data_get_string(settings, "path");
    bool new_loop = obs_data_get_bool(settings, "loop");
    bool new_autoplay = obs_data_get_bool(settings, "autoplay");

    if (path && ctx->file_path != path) {
        blog(LOG_INFO, "[DanceHAP] path changed: '%s' -> '%s'",
             ctx->file_path.c_str(), path);
        ctx->file_path = path;
        ctx->loop = new_loop;
        ctx->load_clip();  // ClipPlayer.load replaces current clip
    } else {
        // Loop setting may have changed — sync to player.
        if (ctx->loop != new_loop) {
            ctx->loop = new_loop;
            ctx->player.setLoop(new_loop);
        }
    }
    ctx->autoplay = new_autoplay;
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

// ---------------------------------------------------------------------------
// Phase 1.4: video_tick drives the ClipPlayer.
// Audio master clock: when the clip has audio, pull audio + advance the
// clock from audio output (ADR-007). When no audio, use wall-clock dt.
// ---------------------------------------------------------------------------

void hap_clip_video_tick(void *data, float seconds)
{
    auto *ctx = static_cast<hap_clip_context *>(data);
    if (!ctx) return;
    if (ctx->player.getState() != dancehap::PlayerState::Playing) return;

    if (ctx->player.hasAudio()) {
        // Audio master clock (ADR-007).
        dancehap::AudioOutput audio =
            ctx->player.pullAudio(AUDIO_CHUNK_US);

        if (audio.valid) {
#ifdef DANCEHAP_HAVE_OBS
            ctx->push_audio(audio);
#endif
            ctx->player.advanceAudioClock(audio.duration_us);
        }
        // tick(0) decodes due frames — clock already advanced by audio.
        ctx->player.tick(0.0f);
    } else {
        // Video master clock (no audio in clip).
        ctx->player.tick(seconds);
    }
}

void hap_clip_video_render(void *data, gs_effect_t *effect)
{
    auto *ctx = static_cast<hap_clip_context *>(data);
    if (!ctx) return;

    // Upload any pending decoded frame to the GPU. This MUST happen on the
    // graphics thread (i.e. here in video_render) — gs_texture_create fails
    // when called from video_tick on Windows OBS 31.
    ctx->player.uploadToGpu();

    gs_texture_t *tex = ctx->player.getTexture();
    if (!tex) return;  // stub mode or no decoded frame yet

#ifdef DANCEHAP_HAVE_OBS
    // OBS draw pattern (per obs-source.c::render_filter_tex): the default
    // effect must be activated via its technique before calling
    // gs_draw_sprite, otherwise nothing renders (the framebuffer stays
    // as it was — typically black for a fresh source).
    uint32_t w = static_cast<uint32_t>(ctx->player.getVideoWidth());
    uint32_t h = static_cast<uint32_t>(ctx->player.getVideoHeight());

    gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
    gs_effect_set_texture(image, tex);

    gs_technique_t *tech = gs_effect_get_technique(effect, "Draw");
    if (tech) {
        size_t passes = gs_technique_begin(tech);
        for (size_t i = 0; i < passes; ++i) {
            gs_technique_begin_pass(tech, i);
            gs_draw_sprite(tex, 0, w, h);
            gs_technique_end_pass(tech);
        }
        gs_technique_end(tech);
    } else {
        // Fallback: older OBS effect loop API.
        while (gs_effect_loop(effect, "Draw")) {
            gs_draw_sprite(tex, 0, w, h);
        }
    }
#endif
    // Stub mode: no OBS graphics API — safe no-op.
}

// ---------------------------------------------------------------------------
// Dimensions — report clip size from the player's video info.
// ---------------------------------------------------------------------------

uint32_t hap_clip_get_width(void *data)
{
    auto *ctx = static_cast<hap_clip_context *>(data);
    if (!ctx || !ctx->player.hasVideo())
        return 0;
    return (uint32_t)ctx->player.getVideoInfo().width;
}

uint32_t hap_clip_get_height(void *data)
{
    auto *ctx = static_cast<hap_clip_context *>(data);
    if (!ctx || !ctx->player.hasVideo())
        return 0;
    return (uint32_t)ctx->player.getVideoInfo().height;
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
    info.get_width      = hap_clip_get_width;
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
