// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// dancehap_composite.cpp — OBS source composite for DanceHAP Phase 3.
//
// Phase 3, Étapes 3-4 (ADR-010, ADR-013):
//   Implements a single OBS source that composites 3 DLayers from a show
//   file (.dhp):
//     DLayer 1 — HAP background (clips + crossfade + B-spline opacity)
//     DLayer 2 — LIVE webcam + matting (placeholder — black texture for now)
//     DLayer 3 — HAP overlay (clips + crossfade + B-spline opacity)
//
// Transport: play / stop / pause via the global show timeline.
// Markers: jump to a specific timecode (for hotkeys / dock buttons).
// Crossfade (ADR-013): double buffering 2 ClipPlayers per DLayer, blend
//   in the composite effect shader via crossfade_t (lerp bg_image → bg_image2).
//
// Anti-patternes respectés (voir dancehap-brief):
//   • OBS_SOURCE_CUSTOM_DRAW → OBS passe un effect NULL → on charge notre
//     propre gs_effect_t dans update() via gs_effect_create_from_file()
//     avec obs_enter/leave_graphics(). JAMAIS dans create().
//   • gs_* depuis video_render uniquement — video_tick fait CPU-only
//     (B-spline eval, ClipPlayer tick, crossfade state machine).
//   • ClipPlayer::uploadToGpu() est appelé depuis video_render (render thread).

#include "dancehap_composite.hpp"
#include "dancehap/version.h"
#include "clip_player.hpp"
#include "show_file.hpp"
#include "bspline.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Transport state machine — definition in header (dancehap_composite.hpp)
// ---------------------------------------------------------------------------

namespace dancehap {

/// State for one DLayer timeline (background or overlay).
/// Manages the active ClipPlayer and, during crossfade, a second one.
struct DLayerRuntime {
    // Clip management
    std::vector<ClipSpec> clips;       // sorted by start time
    int  current_clip_index = -1;      // index into clips, -1 = none active

    // Players: player_a = current clip, player_b = incoming clip (crossfade)
    std::unique_ptr<ClipPlayer> player_a;
    std::unique_ptr<ClipPlayer> player_b;

    // Crossfade state (ADR-013)
    bool   crossfade_active  = false;
    double crossfade_t       = 0.0;    // 0..1
    double crossfade_duration = 0.0;   // seconds

    // Opacity (evaluated from B-spline each tick)
    double opacity = 1.0;
    std::optional<BSpline> bspline;

    // Dimensions
    int width  = 0;
    int height = 0;

    /// Find the clip that should be playing at the given show time.
    /// Returns -1 if no clip covers this time.
    int find_clip_at_time(double show_time) const
    {
        for (int i = 0; i < (int)clips.size(); ++i) {
            const auto &c = clips[i];
            if (show_time >= c.start && show_time < c.start + c.duration)
                return i;
        }
        // Check if we're before the first clip or after the last — return -1.
        return -1;
    }

    /// Check if a crossfade should start at the boundary between clip `idx`
    /// and the next clip. The crossfade starts when the current clip is
    /// within `crossfade_out` seconds of its end AND the next clip has a
    /// `crossfade_in` > 0.
    bool should_start_crossfade(double show_time, int clip_idx) const
    {
        if (clip_idx < 0 || clip_idx + 1 >= (int)clips.size()) return false;
        const auto &cur = clips[clip_idx];
        const auto &next = clips[clip_idx + 1];
        if (next.crossfade_in <= 0.0) return false;

        double time_to_end = (cur.start + cur.duration) - show_time;
        // Crossfade window = min(cur.crossfade_out, next.crossfade_in)
        double xf_window = std::min(cur.crossfade_out, next.crossfade_in);
        if (xf_window <= 0.0) return false;

        return time_to_end <= xf_window && time_to_end > 0.0;
    }

    /// Get the crossfade duration (min of out/in).
    double get_crossfade_duration(int clip_idx) const
    {
        if (clip_idx < 0 || clip_idx + 1 >= (int)clips.size()) return 0.0;
        return std::min(clips[clip_idx].crossfade_out,
                        clips[clip_idx + 1].crossfade_in);
    }
};

} // namespace dancehap

// ---------------------------------------------------------------------------
// Composite context — the per-instance data for the OBS source
// ---------------------------------------------------------------------------

namespace {

/// Composite source context — one instance per OBS source added to a scene.
struct CompositeContext {
    // Show file
    std::string show_file_path;
    std::optional<ShowFile> show;
    bool show_loaded = false;

    // Transport
    dancehap::TransportState transport = dancehap::TransportState::Stopped;
    double show_time = 0.0;       // current position in the show timeline (seconds)

    // Output dimensions (from the first clip that has video, or default 1920x1080)
    uint32_t width  = 1920;
    uint32_t height = 1080;

    // DLayers
    dancehap::DLayerRuntime dlayer1;  // background
    dancehap::DLayerRuntime dlayer3;  // overlay
    // DLayer 2 (webcam) — placeholder for Étape 5, opacity only
    double dlayer2_opacity = 0.0;
    std::optional<dancehap::BSpline> dlayer2_bspline;

    // Composite effect shader (loaded in update(), used in video_render)
    gs_effect_t *composite_effect = nullptr;

#ifdef DANCEHAP_HAVE_OBS
    obs_source_t *source = nullptr;
#endif

    // --- Transport control -------------------------------------------------

    void play()
    {
        if (!show_loaded) return;
        if (transport == dancehap::TransportState::Stopped) {
            show_time = 0.0;
            // Start the first clips
            start_clips_for_time(0.0);
        }
        transport = dancehap::TransportState::Playing;
    }

    void stop()
    {
        transport = dancehap::TransportState::Stopped;
        show_time = 0.0;
        // Stop all players
        dlayer1.player_a.reset();
        dlayer1.player_b.reset();
        dlayer1.current_clip_index = -1;
        dlayer1.crossfade_active = false;
        dlayer3.player_a.reset();
        dlayer3.player_b.reset();
        dlayer3.current_clip_index = -1;
        dlayer3.crossfade_active = false;
    }

    void pause()
    {
        if (transport == dancehap::TransportState::Playing)
            transport = dancehap::TransportState::Paused;
    }

    void resume()
    {
        if (transport == dancehap::TransportState::Paused)
            transport = dancehap::TransportState::Playing;
    }

    /// Jump to a specific show time (marker jump).
    void seek(double target_time)
    {
        show_time = target_time;
        // Reset all clip state and start fresh from the target time
        dlayer1.player_a.reset();
        dlayer1.player_b.reset();
        dlayer1.current_clip_index = -1;
        dlayer1.crossfade_active = false;
        dlayer3.player_a.reset();
        dlayer3.player_b.reset();
        dlayer3.current_clip_index = -1;
        dlayer3.crossfade_active = false;
        start_clips_for_time(target_time);
    }

    /// Jump to a marker by index.
    bool jump_to_marker(int marker_index)
    {
        if (!show_loaded || !show) return false;
        if (marker_index < 0 || marker_index >= (int)show->markers.size())
            return false;
        seek(show->markers[marker_index].time);
        return true;
    }

    // --- Show file loading --------------------------------------------------

    void load_show_file(const std::string &path)
    {
        ParseResult result = parse_show_file(path);
        if (!result) {
            blog(LOG_WARNING, "[DanceHAP] Failed to load show file '%s': %s",
                 path.c_str(),
                 result.errors.empty() ? "unknown error" :
                 result.errors[0].message.c_str());
            show_loaded = false;
            return;
        }
        show_file_path = path;
        show = std::move(result.show);
        show_loaded = true;

        // Build DLayer runtimes from the show file
        build_dlayer(dlayer1, show->dlayer1);
        build_dlayer(dlayer3, show->dlayer3);
        // DLayer 2 opacity only (webcam placeholder)
        build_bspline(dlayer2_bspline, show->dlayer2.opacity_keyframes);
        dlayer2_opacity = 0.0; // webcam disabled until Étape 5

        // Determine output dimensions from DLayer 1 first clip
        if (!show->dlayer1.clips.empty()) {
            // We'll get actual dimensions from ClipPlayer after load.
            // For now, keep default 1920x1080.
        }

        blog(LOG_INFO, "[DanceHAP] Show file loaded: '%s' (duration=%.1f, "
             "markers=%zu, dlayer1_clips=%zu, dlayer3_clips=%zu)",
             show->show.name.c_str(), show->show.duration,
             show->markers.size(), show->dlayer1.clips.size(),
             show->dlayer3.clips.size());
    }

    // --- Clip management ----------------------------------------------------

    /// Start the appropriate clips for the given show time.
    void start_clips_for_time(double time)
    {
        start_dlayer_clips(dlayer1, time);
        start_dlayer_clips(dlayer3, time);
    }

    void start_dlayer_clips(dancehap::DLayerRuntime &dl, double time)
    {
        int idx = dl.find_clip_at_time(time);
        if (idx >= 0) {
            dl.current_clip_index = idx;
            dl.player_a = std::make_unique<dancehap::ClipPlayer>();
            dl.player_a->setLoop(dl.clips[idx].loop);
            // In stub mode, load() returns false (no FFmpeg) but that's OK —
            // the player state machine handles it gracefully.
            dl.player_a->load(dl.clips[idx].file);
            dl.player_a->play();
            // Update dimensions from player
            if (dl.player_a->hasVideo()) {
                dl.width = dl.player_a->getVideoWidth();
                dl.height = dl.player_a->getVideoHeight();
                if (&dl == &dlayer1) {
                    width = (uint32_t)dl.width;
                    height = (uint32_t)dl.height;
                }
            }
        }
    }

    // --- video_tick logic (CPU-only, no gs_*) --------------------------------

    void tick(float dt_seconds)
    {
        if (transport != dancehap::TransportState::Playing) return;
        if (!show_loaded) return;

        show_time += dt_seconds;

        // Clamp to show duration
        if (show && show_time >= show->show.duration) {
            show_time = show->show.duration;
            transport = dancehap::TransportState::Stopped;
            return;
        }

        // Evaluate B-spline opacities
        if (dlayer1.bspline) dlayer1.opacity = dlayer1.bspline->evaluate(show_time);
        if (dlayer2_bspline) dlayer2_opacity = dlayer2_bspline->evaluate(show_time);
        if (dlayer3.bspline) dlayer3.opacity = dlayer3.bspline->evaluate(show_time);

        // Tick DLayer 1 (background)
        tick_dlayer(dlayer1, dt_seconds);

        // DLayer 2 (webcam) — placeholder, no tick needed yet

        // Tick DLayer 3 (overlay)
        tick_dlayer(dlayer3, dt_seconds);
    }

    void tick_dlayer(dancehap::DLayerRuntime &dl, float dt)
    {
        // If in crossfade, tick both players
        if (dl.crossfade_active && dl.player_a && dl.player_b) {
            dl.player_a->tick(dt);
            dl.player_b->tick(dt);
            dl.crossfade_t += (double)dt / dl.crossfade_duration;
            if (dl.crossfade_t >= 1.0) {
                // Crossfade complete: player_b becomes player_a
                dl.player_a = std::move(dl.player_b);
                dl.player_b.reset();
                dl.crossfade_active = false;
                dl.crossfade_t = 0.0;
                dl.current_clip_index++;
            }
            return;
        }

        // Normal mode: tick the active player
        if (dl.player_a) {
            dl.player_a->tick(dt);

            // Check if we need to start a crossfade
            if (!dl.crossfade_active) {
                int idx = dl.current_clip_index;
                if (dl.should_start_crossfade(show_time, idx)) {
                    // Start crossfade: create player_b for the next clip
                    int next_idx = idx + 1;
                    if (next_idx < (int)dl.clips.size()) {
                        dl.player_b = std::make_unique<dancehap::ClipPlayer>();
                        dl.player_b->setLoop(dl.clips[next_idx].loop);
                        dl.player_b->load(dl.clips[next_idx].file);
                        dl.player_b->play();
                        dl.crossfade_active = true;
                        dl.crossfade_t = 0.0;
                        dl.crossfade_duration = dl.get_crossfade_duration(idx);
                        blog(LOG_INFO, "[DanceHAP] Crossfade started: "
                             "clip %d → %d (duration=%.2fs)",
                             idx, next_idx, dl.crossfade_duration);
                    }
                }
            }

            // Check if current clip ended (no crossfade — hard cut)
            if (!dl.crossfade_active && dl.player_a->getState() ==
                dancehap::PlayerState::Ended) {
                int next_idx = dl.current_clip_index + 1;
                if (next_idx < (int)dl.clips.size() &&
                    show_time >= dl.clips[next_idx].start) {
                    // Hard cut to next clip
                    dl.current_clip_index = next_idx;
                    dl.player_a = std::make_unique<dancehap::ClipPlayer>();
                    dl.player_a->setLoop(dl.clips[next_idx].loop);
                    dl.player_a->load(dl.clips[next_idx].file);
                    dl.player_a->play();
                }
            }
        } else {
            // No active player — check if a clip should start now
            int idx = dl.find_clip_at_time(show_time);
            if (idx >= 0 && idx != dl.current_clip_index) {
                start_dlayer_clips(dl, show_time);
            }
        }
    }

    // --- video_render logic (GPU, gs_* allowed) -----------------------------

    void render()
    {
#ifdef DANCEHAP_HAVE_OBS
        if (!composite_effect) return;

        // Upload pending frames to GPU (must be on render thread)
        if (dlayer1.player_a) dlayer1.player_a->uploadToGpu();
        if (dlayer1.player_b) dlayer1.player_b->uploadToGpu();
        if (dlayer3.player_a) dlayer3.player_a->uploadToGpu();
        if (dlayer3.player_b) dlayer3.player_b->uploadToGpu();

        // Set textures
        gs_texture_t *bg_tex = dlayer1.player_a ? dlayer1.player_a->getTexture() : nullptr;
        gs_texture_t *bg2_tex = (dlayer1.crossfade_active && dlayer1.player_b) ?
                                 dlayer1.player_b->getTexture() : nullptr;
        gs_texture_t *overlay_tex = dlayer3.player_a ? dlayer3.player_a->getTexture() : nullptr;

        // Set shader parameters
        gs_eparam_t *param;

        param = gs_effect_get_param_by_name(composite_effect, "bg_image");
        if (param && bg_tex) gs_effect_set_texture(param, bg_tex);

        param = gs_effect_get_param_by_name(composite_effect, "bg_image2");
        if (param && bg2_tex) gs_effect_set_texture(param, bg2_tex);

        // DLayer 2 (webcam) — placeholder: no texture yet (black/transparent)
        param = gs_effect_get_param_by_name(composite_effect, "webcam_image");
        // In stub mode or before Étape 5, we don't set webcam_image — the
        // shader samples a default (black) texture. In real OBS we should
        // create a 1x1 black texture as placeholder.

        param = gs_effect_get_param_by_name(composite_effect, "overlay_image");
        if (param && overlay_tex) gs_effect_set_texture(param, overlay_tex);

        // Crossfade
        param = gs_effect_get_param_by_name(composite_effect, "crossfade_t");
        if (param) gs_effect_set_float(param, (float)dlayer1.crossfade_t);

        param = gs_effect_get_param_by_name(composite_effect, "crossfade_active");
        if (param) gs_effect_set_float(param, dlayer1.crossfade_active ? 1.0f : 0.0f);

        // Opacities
        param = gs_effect_get_param_by_name(composite_effect, "bg_opacity");
        if (param) gs_effect_set_float(param, (float)dlayer1.opacity);

        param = gs_effect_get_param_by_name(composite_effect, "webcam_opacity");
        if (param) gs_effect_set_float(param, (float)dlayer2_opacity);

        param = gs_effect_get_param_by_name(composite_effect, "overlay_opacity");
        if (param) gs_effect_set_float(param, (float)dlayer3.opacity);

        // Draw with blend state for alpha compositing
        gs_blend_state_push();
        gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

        gs_technique_t *tech = gs_effect_get_technique(composite_effect, "Draw");
        if (tech) {
            size_t passes = gs_technique_begin(tech);
            for (size_t i = 0; i < passes; i++) {
                gs_technique_begin_pass(tech, i);
                gs_draw_sprite(nullptr, 0, width, height);
                gs_technique_end_pass(tech);
            }
            gs_technique_end(tech);
        }

        gs_blend_state_pop();
#endif // DANCEHAP_HAVE_OBS
    }

    // --- Load composite effect shader (in update(), with graphics context) --

    void load_composite_effect()
    {
#ifdef DANCEHAP_HAVE_OBS
        // CRITICAL: gs_effect_create_from_file() requires the graphics context
        // to be active. OBS guarantees it's available in update(). We wrap
        // with obs_enter/leave_graphics() for safety (leçon bug 9 du brief).
        obs_enter_graphics();

        const char *effect_path = obs_module_file("effects/composite.effect");
        if (effect_path && effect_path[0]) {
            gs_effect_t *eff = gs_effect_create_from_file(effect_path, nullptr);
            if (eff) {
                if (composite_effect) gs_effect_destroy(composite_effect);
                composite_effect = eff;
                blog(LOG_INFO, "[DanceHAP] Composite effect loaded: %s",
                     effect_path);
            } else {
                blog(LOG_ERROR, "[DanceHAP] Failed to load composite effect: %s",
                     effect_path);
            }
        } else {
            blog(LOG_ERROR, "[DanceHAP] obs_module_file returned empty path "
                 "for effects/composite.effect");
        }

        obs_leave_graphics();
#endif
    }

private:
    void build_dlayer(dancehap::DLayerRuntime &dl, const dancehap::DLayerConfig &cfg)
    {
        dl.clips = cfg.clips;
        // Sort clips by start time (should already be sorted, but ensure)
        std::sort(dl.clips.begin(), dl.clips.end(),
                  [](const dancehap::ClipSpec &a, const dancehap::ClipSpec &b) {
                      return a.start < b.start;
                  });
        dl.current_clip_index = -1;
        dl.crossfade_active = false;
        dl.crossfade_t = 0.0;
        dl.opacity = 1.0;
        build_bspline(dl.bspline, cfg.opacity_keyframes);
    }

    void build_bspline(std::optional<dancehap::BSpline> &bs,
                       const std::vector<dancehap::Keyframe> &kfs)
    {
        if (kfs.empty()) {
            bs.reset();
            return;
        }
        std::vector<dancehap::BSplineKeyframe> bkfs;
        bkfs.reserve(kfs.size());
        for (const auto &kf : kfs) {
            dancehap::BSplineKeyframe b;
            b.time = kf.time;
            b.value = kf.value;
            bkfs.push_back(b);
        }
        bs.emplace(std::move(bkfs));
    }
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// OBS callback implementations
// ---------------------------------------------------------------------------

namespace {

const char *composite_get_name(void * /*type_data*/)
{
    return DANCEHAP_COMPOSITE_SOURCE_NAME " v" DANCEHAP_VERSION_STRING;
}

void *composite_create(obs_data_t *settings, obs_source_t *source)
{
    auto *ctx = new CompositeContext();
#ifdef DANCEHAP_HAVE_OBS
    ctx->source = source;
#endif

    // Load show file if path is set
    if (settings) {
        const char *path = obs_data_get_string(settings, "show_file");
        if (path && path[0]) {
            ctx->load_show_file(path);
        }
    }

    blog(LOG_INFO, "[DanceHAP] Composite source created (show_file='%s')",
         ctx->show_file_path.c_str());
    return ctx;
}

void composite_destroy(void *data)
{
    auto *ctx = static_cast<CompositeContext *>(data);
    if (!ctx) return;

    // Release graphics resources
#ifdef DANCEHAP_HAVE_OBS
    if (ctx->composite_effect) {
        obs_enter_graphics();
        gs_effect_destroy(ctx->composite_effect);
        obs_leave_graphics();
    }
#endif

    blog(LOG_INFO, "[DanceHAP] Composite source destroyed");
    delete ctx;
}

uint32_t composite_get_width(void *data)
{
    auto *ctx = static_cast<CompositeContext *>(data);
    if (!ctx) return 0;
    return ctx->width;
}

uint32_t composite_get_height(void *data)
{
    auto *ctx = static_cast<CompositeContext *>(data);
    if (!ctx) return 0;
    return ctx->height;
}

void composite_get_defaults(obs_data_t *settings)
{
    if (!settings) return;
    obs_data_set_default_string(settings, "show_file", "");
    obs_data_set_default_bool(settings, "autoplay", false);
}

obs_properties_t *composite_get_properties(void * /*data*/)
{
    obs_properties_t *props = obs_properties_create();
    if (!props) return nullptr;

    obs_properties_add_path(
        props,
        "show_file",
        "Show File (.dhp)",
        OBS_PATH_FILE,
        "DanceHAP Show (*.dhp)",
        "");

    obs_properties_add_bool(props, "autoplay", "Autoplay");

    return props;
}

void composite_update(void *data, obs_data_t *settings)
{
    auto *ctx = static_cast<CompositeContext *>(data);
    if (!ctx || !settings) return;

    const char *path = obs_data_get_string(settings, "show_file");
    bool autoplay = obs_data_get_bool(settings, "autoplay");

    // Load or reload show file if path changed
    if (path && ctx->show_file_path != path) {
        if (path[0]) {
            ctx->load_show_file(path);
            // Load composite effect shader (graphics context available in update)
            ctx->load_composite_effect();
            if (autoplay && ctx->show_loaded) {
                ctx->play();
            }
        } else {
            // Empty path — unload show
            ctx->stop();
            ctx->show.reset();
            ctx->show_loaded = false;
            ctx->show_file_path.clear();
        }
    }
}

void composite_activate(void *data)
{
    auto *ctx = static_cast<CompositeContext *>(data);
    if (!ctx) return;
    blog(LOG_INFO, "[DanceHAP] Composite source activated");
}

void composite_deactivate(void *data)
{
    auto *ctx = static_cast<CompositeContext *>(data);
    if (!ctx) return;
    blog(LOG_INFO, "[DanceHAP] Composite source deactivated");
}

void composite_video_tick(void *data, float seconds)
{
    auto *ctx = static_cast<CompositeContext *>(data);
    if (!ctx) return;
    ctx->tick(seconds);
}

void composite_video_render(void *data, gs_effect_t * /*effect*/)
{
    // NOTE: OBS passes a NULL effect because of OBS_SOURCE_CUSTOM_DRAW.
    // We use our own composite_effect loaded in update().
    auto *ctx = static_cast<CompositeContext *>(data);
    if (!ctx) return;
    ctx->render();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Build the obs_source_info struct
// ---------------------------------------------------------------------------

struct obs_source_info build_composite_info()
{
    struct obs_source_info info = {};

    info.id           = DANCEHAP_COMPOSITE_SOURCE_ID;
    info.type         = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_VIDEO
                      | OBS_SOURCE_CUSTOM_DRAW
                      | OBS_SOURCE_DO_NOT_DUPLICATE;

    info.get_name       = composite_get_name;
    info.create         = composite_create;
    info.destroy        = composite_destroy;
    info.get_width      = composite_get_width;
    info.get_height     = composite_get_height;
    info.get_defaults   = composite_get_defaults;
    info.get_properties = composite_get_properties;
    info.update         = composite_update;
    info.activate       = composite_activate;
    info.deactivate     = composite_deactivate;
    info.show           = nullptr;
    info.hide           = nullptr;
    info.video_tick     = composite_video_tick;
    info.video_render   = composite_video_render;

    return info;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

const struct obs_source_info *dancehap_composite_get_info(void)
{
    static const struct obs_source_info s_info = build_composite_info();
    return &s_info;
}

void register_dancehap_composite_source(void)
{
    const struct obs_source_info *info = dancehap_composite_get_info();
    blog(LOG_INFO, "[DanceHAP] registering composite source '%s' (v%s)",
         info->id, DANCEHAP_VERSION_STRING);
    obs_register_source(info);
}