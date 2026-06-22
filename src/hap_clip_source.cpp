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
//   DO_NOT_DUPLICATE — A/V sync managed internally
//
// NOTE: we do NOT set OBS_SOURCE_CUSTOM_DRAW. Without that flag, OBS calls
// obs_source_default_render() which wraps our video_render() in the proper
// default-effect "Draw" technique begin/end (see libobs/obs-source.c).
// With CUSTOM_DRAW set, OBS passes a NULL effect to video_render and we
// would have to load the default effect ourselves — which produced a
// silent transparent render in OBS 31 (gs_effect_get_technique(NULL, ...)
// returns NULL, so no draw pass ever runs).

#include "hap_clip_source.hpp"
#include "clip_player.hpp"
#include "dancehap/version.h"

#include <cstring>
#include <fstream>
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
    int  volume    = 100;  // 0..100, applied to audio output (Phase 1.5.a)

    // Phase 1.4: ClipPlayer owns demuxer + decoder + timing + loop logic.
    dancehap::ClipPlayer player;

#ifdef DANCEHAP_HAVE_OBS
    obs_source_t *source = nullptr;  // for obs_source_output_audio_data

    /// Push interleaved float audio to OBS as planar float.
    /// Converts AudioOutput.samples (interleaved) → obs_source_audio (planar).
    /// Applies the per-source volume gain (0..100 → 0.0..1.0) to each sample.
    void push_audio(const dancehap::AudioOutput &audio)
    {
        if (!source || !audio.valid || audio.frames <= 0) return;

        // Volume gain: 0..100 → 0.0..1.0. Clamped in case update() saw an
        // out-of-range value (defensive — OBS UI should enforce 0..100).
        const float gain = (volume < 0 || volume > 100)
            ? 1.0f
            : static_cast<float>(volume) / 100.0f;

        // Planar buffers (one per channel). Allocate on each call — small
        // (frames * channels * 4 bytes, typically < 4 KB).
        std::vector<float> planar[8];
        int ch = audio.channels < 8 ? audio.channels : 8;
        for (int c = 0; c < ch; ++c) {
            planar[c].resize(audio.frames);
            for (int i = 0; i < audio.frames; ++i) {
                planar[c][i] = gain * audio.samples[
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
        ctx->volume   = static_cast<int>(obs_data_get_int(settings, "volume"));
    }
    blog(LOG_INFO, "[DanceHAP] hap_clip_source created "
                   "(path='%s', loop=%d, autoplay=%d, volume=%d)",
         ctx->file_path.c_str(), (int)ctx->loop, (int)ctx->autoplay,
         ctx->volume);

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
    obs_data_set_default_int(settings, "volume", 100);
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
        "HAP clips (*.mov)",
        "");

    obs_properties_add_bool(props, "loop", "Loop");
    obs_properties_add_bool(props, "autoplay", "Autoplay");
    obs_properties_add_int(props, "volume", "Volume", 0, 100, 1);

    return props;
}

// Check if a file exists and is readable. Uses std::ifstream (portable,
// no OBS dependency) so it works identically in stub and real modes.
static bool hap_clip_file_exists(const std::string &path)
{
    if (path.empty()) return false;
    std::ifstream f(path, std::ios::binary);
    return f.is_open();
}

void hap_clip_update(void *data, obs_data_t *settings)
{
    auto *ctx = static_cast<hap_clip_context *>(data);
    if (!ctx || !settings) return;

    const char *path = obs_data_get_string(settings, "path");
    bool new_loop = obs_data_get_bool(settings, "loop");
    bool new_autoplay = obs_data_get_bool(settings, "autoplay");
    int  new_volume = static_cast<int>(obs_data_get_int(settings, "volume"));

    if (path && ctx->file_path != path) {
        // Phase 1.5.a Étape 2: validate the new path before reloading.
        // If the file doesn't exist, log a warning and skip the reload —
        // the source keeps playing the previous clip (or stays idle).
        // This avoids a confusing silent failure when the user mistypes
        // a path or picks a deleted file.
        if (!hap_clip_file_exists(path)) {
            blog(LOG_WARNING, "[DanceHAP] path '%s' does not exist or is "
                 "not readable — keeping previous clip '%s'",
                 path, ctx->file_path.c_str());
        } else {
            blog(LOG_INFO, "[DanceHAP] path changed: '%s' -> '%s'",
                 ctx->file_path.c_str(), path);
            // Phase 1.5.a Étape 3: stop the player before reloading so
            // any in-flight decode/upload is cleanly released. load_clip
            // recreates the demuxer + decoder via unique_ptr reset, which
            // handles the old ones, but an explicit stop makes the
            // transition visible in the log and guards against future
            // refactors that might not use unique_ptr.
            ctx->player.stop();
            ctx->file_path = path;
            ctx->loop = new_loop;
            ctx->load_clip();  // ClipPlayer.load replaces current clip
        }
    } else {
        // Loop setting may have changed — sync to player.
        if (ctx->loop != new_loop) {
            ctx->loop = new_loop;
            ctx->player.setLoop(new_loop);
        }
    }
    ctx->autoplay = new_autoplay;
    ctx->volume = new_volume;
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
    if (!tex) {
        // Log the first ~5 times video_render is called with no texture, so
        // we can tell from the OBS log whether video_render is being reached.
        static int no_tex_log_count = 0;
        if (no_tex_log_count < 5) {
            ++no_tex_log_count;
            blog(LOG_WARNING, "[DanceHAP] video_render: no texture yet "
                 "(state=%d, hasVideo=%d, frameCount=%d, lastError='%s')",
                 (int)ctx->player.getState(),
                 (int)ctx->player.hasVideo(),
                 ctx->player.getFrameCount(),
                 ctx->player.getLastError().c_str());
        }
        return;
    }

#ifdef DANCEHAP_HAVE_OBS
    // Log the first successful draw so we can confirm render path is reached.
    static thread_local bool logged_first_draw = false;
    if (!logged_first_draw) {
        logged_first_draw = true;
        blog(LOG_INFO, "[DanceHAP] video_render: first draw with valid "
             "texture %dx%d variant=%d", ctx->player.getVideoWidth(),
             ctx->player.getVideoHeight(),
             (int)ctx->player.getVideoInfo().variant);
    }

    // Without OBS_SOURCE_CUSTOM_DRAW, OBS wraps our video_render() call in
    // the default effect's "Draw" technique begin/pass/end (see
    // libobs/obs-source.c::obs_source_default_render). We just need to
    // bind the texture to the "image" parameter and draw.
    uint32_t w = static_cast<uint32_t>(ctx->player.getVideoWidth());
    uint32_t h = static_cast<uint32_t>(ctx->player.getVideoHeight());

    if (effect) {
        gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
        if (image) gs_effect_set_texture(image, tex);
    }
    gs_draw_sprite(tex, 0, w, h);
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
