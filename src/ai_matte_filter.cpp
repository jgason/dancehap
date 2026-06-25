// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// ai_matte_filter.cpp — OBS filter that applies AI-based matting to a webcam
// source.
//
// Phase 2.0-2.5: skeleton + MatteEngine + Properties UI.
// Phase 2.5b (v0.4.7-v0.4.8): async worker thread for inference + model load.
// Phase 2.5c (v0.4.9): REWRITE using the correct OBS filter pattern.
//
// History of bugs fixed in this version:
//   v0.4.7: async inference + model load (worker thread).
//   v0.4.8: lazy-init engine (don't load onnxruntime.dll at filter creation).
//   v0.4.9: use texrender + stagesurface + process_filter_begin/end pattern
//           (instead of obs_source_get_frame which is for async sources).
//           This fixes:
//             - Frame deformation (stretch/squeeze) — we were memcpy'ing
//               without respecting the frame's linesize.
//             - Freeze — obs_source_get_frame on a sync filter source causes
//               a deadlock with the OBS graphics thread.
//
// The correct pattern for a synchronous video filter in OBS (following
// obs-backgroundremoval by royshil) is:
//
//   video_render (render thread):
//     1. gs_texrender_begin(texrender, w, h) → render target source into it
//     2. gs_stage_texture(stagesurface, texrender_texture) → GPU→CPU copy
//     3. gs_stagesurface_map → get video_data + linesize
//     4. Copy frame data to inputBGRA (respecting linesize!)
//     5. gs_stagesurface_unmap
//     6. obs_source_process_filter_begin(source, GS_RGBA, ...)
//     7. Create alpha texture from backgroundMask
//     8. gs_effect_set_texture + draw with shader
//     9. obs_source_process_filter_end
//
//   video_tick (video thread):
//     1. Try-lock inputBGRALock (don't block the render thread)
//     2. If inputBGRA available → clone it
//     3. Run inference → produce backgroundMask
//     4. Store backgroundMask under outputLock
//
// Reference: github.com/royshil/obs-backgroundremoval/blob/main/src/
//   - background-filter.cpp (video_tick + video_render)
//   - obs-utils/obs-utils.cpp (getRGBAFromStageSurface)
//   - FilterData.hpp (struct layout)

#include "ai_matte_filter.hpp"
#include "dancehap/version.h"
#include "matte_engine.hpp"

#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <condition_variable>
#include <atomic>

// ---------------------------------------------------------------------------
// Per-instance context
// ---------------------------------------------------------------------------

namespace dancehap {

/// Apply an alpha mask to a BGRA frame.
/// Multiplies each pixel's alpha channel by the corresponding mask value.
/// Assumes packed BGRA (linesize == width * 4).
std::vector<uint8_t> apply_alpha_mask_to_bgra(
    const uint8_t *bgra_data, uint32_t width, uint32_t height,
    const std::vector<float> &mask)
{
    std::vector<uint8_t> out;
    if (!bgra_data || width == 0 || height == 0) return out;

    size_t pixel_count = static_cast<size_t>(width) * height;
    out.resize(pixel_count * 4);

    bool mask_valid = (mask.size() == pixel_count);

    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t *src = bgra_data + static_cast<size_t>(row) * width * 4;
        uint8_t *dst = out.data() + static_cast<size_t>(row) * width * 4;
        for (uint32_t col = 0; col < width; ++col) {
            size_t pi = static_cast<size_t>(row) * width + col;
            dst[col * 4 + 0] = src[col * 4 + 0];  // B
            dst[col * 4 + 1] = src[col * 4 + 1];  // G
            dst[col * 4 + 2] = src[col * 4 + 2];  // R
            float a = mask_valid ? mask[pi] : 1.0f;
            if (a < 0.0f) a = 0.0f;
            if (a > 1.0f) a = 1.0f;
            dst[col * 4 + 3] = static_cast<uint8_t>(
                static_cast<float>(src[col * 4 + 3]) * a);
        }
    }
    return out;
}

} // namespace dancehap

// ---------------------------------------------------------------------------
// Per-instance context (OBS filter with async inference)
// ---------------------------------------------------------------------------

namespace {

struct ai_matte_context {
    bool active = false;
    bool matte_enabled = false;       // user toggle
    std::string model_path;           // ONNX model path (current setting)
    std::string loaded_model_path;    // ONNX model path (actually loaded)

#ifdef DANCEHAP_HAVE_ONNXRUNTIME
    // LAZY-INIT: engine created only when user enables matte.
    std::unique_ptr<dancehap::MatteEngine> engine;
    dancehap::MatteModelConfig config;
    dancehap::ExecutionProvider desired_ep = dancehap::ExecutionProvider::Auto;
    bool engine_created = false;
#endif

#ifdef DANCEHAP_HAVE_OBS
    obs_source_t *source = nullptr;
    gs_texrender_t *texrender = nullptr;
    gs_stagesurf_t *stagesurface = nullptr;
    uint32_t stagesurface_w = 0;
    uint32_t stagesurface_h = 0;
#endif

    // Input buffer (render thread writes, worker reads)
    // Packed BGRA, width*height*4 bytes, no padding (linesize already handled)
    std::vector<uint8_t> inputBGRA;
    uint32_t input_w = 0;
    uint32_t input_h = 0;
    std::mutex inputBGRALock;

    // Output mask (worker writes, render thread reads)
    std::vector<float> outputMask;
    uint32_t mask_w = 0;
    uint32_t mask_h = 0;
    bool mask_ready = false;
    std::mutex outputLock;

    // ---- Worker thread (async inference + model load) -------------------

    std::thread worker;
    std::mutex worker_mtx;
    std::condition_variable cv;
    std::atomic<bool> stop{false};

    // Model load request
    std::string pending_model_path;
    bool model_load_requested = false;
    bool engine_create_requested = false;

#ifdef DANCEHAP_HAVE_ONNXRUNTIME
    void worker_loop()
    {
        while (!stop.load()) {
            std::unique_lock<std::mutex> lock(worker_mtx);
            cv.wait(lock, [this] {
                return stop.load() || model_load_requested
                    || engine_create_requested;
            });

            if (stop.load()) break;

            // --- Handle engine creation ---
            if (engine_create_requested) {
                engine_create_requested = false;
                lock.unlock();

                if (!engine_created) {
                    blog(LOG_INFO, "[DanceHAP] MatteEngine: creating engine (async)");
                    engine = std::make_unique<dancehap::MatteEngine>();
                    engine->setConfig(config);
                    engine->setDesiredProvider(desired_ep);
                    engine_created = true;
                    blog(LOG_INFO, "[DanceHAP] MatteEngine: engine created (async)");
                }
                continue;
            }

            // --- Handle model load ---
            if (model_load_requested) {
                std::string path = pending_model_path;
                model_load_requested = false;
                lock.unlock();

                if (engine && !path.empty()) {
                    blog(LOG_INFO, "[DanceHAP] MatteEngine: loading model '%s' (async)",
                         path.c_str());
                    if (!engine->loadModel(path)) {
                        blog(LOG_WARNING, "[DanceHAP] MatteEngine: failed to load '%s'",
                             path.c_str());
                    } else {
                        loaded_model_path = path;
                        blog(LOG_INFO, "[DanceHAP] MatteEngine: model loaded (async)");
                    }
                }
                continue;
            }
        }
    }

    void start_worker()
    {
        if (worker.joinable()) return;
        stop.store(false);
        worker = std::thread([this] { worker_loop(); });
    }

    void stop_worker()
    {
        stop.store(true);
        cv.notify_all();
        if (worker.joinable()) worker.join();
    }
#endif // DANCEHAP_HAVE_ONNXRUNTIME
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

    // Create texrender (will be resized on first render)
    ctx->texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
#endif
    blog(LOG_INFO, "[DanceHAP] ai_matte_filter created (v%s)",
         DANCEHAP_VERSION_STRING);
    return ctx;
}

void ai_matte_destroy(void *data)
{
    auto *ctx = static_cast<ai_matte_context *>(data);
    if (!ctx) return;
#ifdef DANCEHAP_HAVE_ONNXRUNTIME
    ctx->stop_worker();
#endif
#ifdef DANCEHAP_HAVE_OBS
    if (ctx->stagesurface) {
        gs_stagesurface_destroy(ctx->stagesurface);
        ctx->stagesurface = nullptr;
    }
    if (ctx->texrender) {
        gs_texrender_destroy(ctx->texrender);
        ctx->texrender = nullptr;
    }
#endif
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

    obs_properties_add_bool(props, "matte_enable",
        obs_module_text("DanceHAP.Matte.Enable"));

    obs_properties_add_path(props, "matte_model_path",
        obs_module_text("DanceHAP.Matte.ModelPath"),
        OBS_PATH_FILE, "ONNX model (*.onnx)", nullptr);

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

    int res = (quality == 0) ? 192 : (quality == 2) ? 512 : 256;

#ifdef DANCEHAP_HAVE_ONNXRUNTIME
    dancehap::MatteModelConfig cfg;
    cfg.input_width  = res;
    cfg.input_height = res;
    ctx->config = cfg;

    dancehap::ExecutionProvider ep =
        (provider == 1) ? dancehap::ExecutionProvider::DirectML :
        (provider == 2) ? dancehap::ExecutionProvider::CoreML :
        (provider == 3) ? dancehap::ExecutionProvider::CPU :
                          dancehap::ExecutionProvider::Auto;
    ctx->desired_ep = ep;

    if (ctx->engine_created && ctx->engine) {
        ctx->engine->setConfig(cfg);
        ctx->engine->setDesiredProvider(ep);
    }

    if (enable && !ctx->model_path.empty()) {
        if (!ctx->engine_created) {
            ctx->start_worker();
            {
                std::lock_guard<std::mutex> lock(ctx->worker_mtx);
                ctx->engine_create_requested = true;
            }
            ctx->cv.notify_one();
            blog(LOG_INFO, "[DanceHAP] MatteEngine: engine create requested (async)");
        }

        bool need_load = !ctx->engine_created
            || ctx->model_path != ctx->loaded_model_path;
        if (ctx->engine_created && ctx->engine
            && ctx->engine->isReady()
            && ctx->model_path == ctx->loaded_model_path) {
            need_load = false;
        }
        if (need_load) {
            {
                std::lock_guard<std::mutex> lock(ctx->worker_mtx);
                ctx->pending_model_path = ctx->model_path;
                ctx->model_load_requested = true;
            }
            ctx->cv.notify_one();
            blog(LOG_INFO, "[DanceHAP] MatteEngine: model load requested (async)");
        }
    }
#endif
}

#ifdef DANCEHAP_HAVE_OBS
/// Capture the target source's video into ctx->inputBGRA.
/// Called from video_render (render thread).
/// Follows the getRGBAFromStageSurface pattern from obs-backgroundremoval.
static bool captureTargetToInputBGRA(ai_matte_context *ctx)
{
    obs_source_t *target = obs_filter_get_target(ctx->source);
    if (!target) return false;

    uint32_t width = obs_source_get_base_width(target);
    uint32_t height = obs_source_get_base_height(target);
    if (width == 0 || height == 0) return false;

    // Render the target into our texrender
    gs_texrender_reset(ctx->texrender);
    if (!gs_texrender_begin(ctx->texrender, width, height)) {
        return false;
    }
    struct vec4 background;
    vec4_zero(&background);
    gs_clear(GS_CLEAR_COLOR, &background, 0.0f, 0);
    gs_ortho(0.0f, static_cast<float>(width), 0.0f,
             static_cast<float>(height), -100.0f, 100.0f);
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
    obs_source_video_render(target);
    gs_blend_state_pop();
    gs_texrender_end(ctx->texrender);

    // Stage the texture (GPU → CPU copy)
    gs_texture_t *tex = gs_texrender_get_texture(ctx->texrender);
    if (!tex) return false;

    // Recreate stagesurface if dimensions changed
    if (ctx->stagesurface &&
        (ctx->stagesurface_w != width || ctx->stagesurface_h != height)) {
        gs_stagesurface_destroy(ctx->stagesurface);
        ctx->stagesurface = nullptr;
    }
    if (!ctx->stagesurface) {
        ctx->stagesurface = gs_stagesurface_create(width, height, GS_BGRA);
        ctx->stagesurface_w = width;
        ctx->stagesurface_h = height;
    }

    gs_stage_texture(ctx->stagesurface, tex);

    // Map the staged surface to get CPU access
    uint8_t *video_data;
    uint32_t linesize;
    if (!gs_stagesurface_map(ctx->stagesurface, &video_data, &linesize)) {
        return false;
    }

    // Copy frame data respecting linesize → packed BGRA
    {
        std::lock_guard<std::mutex> lock(ctx->inputBGRALock);
        ctx->inputBGRA.resize(static_cast<size_t>(width) * height * 4);
        for (uint32_t row = 0; row < height; ++row) {
            memcpy(ctx->inputBGRA.data() + static_cast<size_t>(row) * width * 4,
                   video_data + static_cast<size_t>(row) * linesize,
                   static_cast<size_t>(width) * 4);
        }
        ctx->input_w = width;
        ctx->input_h = height;
    }

    gs_stagesurface_unmap(ctx->stagesurface);
    return true;
}
#endif

void ai_matte_video_tick(void *data, float /*seconds*/)
{
    auto *ctx = static_cast<ai_matte_context *>(data);
    if (!ctx) return;

#ifdef DANCEHAP_HAVE_OBS
    if (!ctx->matte_enabled) return;
#endif

#ifdef DANCEHAP_HAVE_ONNXRUNTIME
    // Try-lock inputBGRALock — don't block the render thread
    std::vector<uint8_t> imageBGRA;
    uint32_t w, h;
    {
        std::unique_lock<std::mutex> lock(ctx->inputBGRALock, std::try_to_lock);
        if (!lock.owns_lock()) return;
        if (ctx->inputBGRA.empty()) return;
        imageBGRA = ctx->inputBGRA;  // copy
        w = ctx->input_w;
        h = ctx->input_h;
    }

    if (!ctx->engine_created || !ctx->engine || !ctx->engine->isReady())
        return;
    if (w == 0 || h == 0) return;

    // Run inference
    dancehap::ImageFrame img;
    img.width  = static_cast<int>(w);
    img.height = static_cast<int>(h);
    img.data_rgba = imageBGRA.data();

    dancehap::MatteMask mask = ctx->engine->infer(img);

    if (mask.width == static_cast<int>(w) && mask.height == static_cast<int>(h)) {
        std::lock_guard<std::mutex> lock(ctx->outputLock);
        ctx->outputMask = std::move(mask.alpha);
        ctx->mask_w = w;
        ctx->mask_h = h;
        ctx->mask_ready = true;
    }
#endif
}

void ai_matte_video_render(void *data, gs_effect_t *effect)
{
    auto *ctx = static_cast<ai_matte_context *>(data);
    if (!ctx) return;

#ifdef DANCEHAP_HAVE_OBS
    // 1. Capture the target source video into inputBGRA (for next tick's inference)
    if (ctx->matte_enabled) {
        captureTargetToInputBGRA(ctx);
    }

    // 2. If matte is disabled or no mask ready, pass through the target
    obs_source_t *target = obs_filter_get_target(ctx->source);
    if (!ctx->matte_enabled || !ctx->mask_ready || !target) {
        obs_source_skip_video_filter(ctx->source);
        return;
    }

    // 3. Apply the mask via process_filter_begin/end with a shader effect.
    //    For now, we use the default effect which supports alpha blending.
    //    A custom effect shader would go here in a future iteration.
    //
    //    The mask is in outputMask (float 0-1, w*h elements).
    //    We create an alpha texture from it and let OBS blend it.
    uint32_t mask_w, mask_h;
    std::vector<uint8_t> alpha_bytes;
    {
        std::lock_guard<std::mutex> lock(ctx->outputLock);
        if (ctx->outputMask.empty()) {
            obs_source_skip_video_filter(ctx->source);
            return;
        }
        mask_w = ctx->mask_w;
        mask_h = ctx->mask_h;
        alpha_bytes.resize(ctx->outputMask.size());
        for (size_t i = 0; i < ctx->outputMask.size(); ++i) {
            float v = ctx->outputMask[i];
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            alpha_bytes[i] = static_cast<uint8_t>(v * 255.0f);
        }
    }

    // Create alpha texture (R8, single channel)
    const uint8_t *alpha_ptr = alpha_bytes.data();
    gs_texture_t *alphaTexture = gs_texture_create(
        mask_w, mask_h, GS_R8, 1, &alpha_ptr, 0);
    if (!alphaTexture) {
        blog(LOG_WARNING, "[DanceHAP] Failed to create alpha texture");
        obs_source_skip_video_filter(ctx->source);
        return;
    }

    // Begin filter rendering
    if (!obs_source_process_filter_begin(ctx->source, GS_RGBA,
                                          OBS_ALLOW_DIRECT_RENDERING)) {
        gs_texture_destroy(alphaTexture);
        obs_source_skip_video_filter(ctx->source);
        return;
    }

    // Use the effect passed by OBS (default effect with Draw technique).
    gs_eparam_t *imageParam = gs_effect_get_param_by_name(effect, "image");
    gs_effect_set_texture(imageParam, alphaTexture);

    // Draw the target through the filter with alpha blending.
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

    obs_source_process_filter_end(ctx->source, effect, 0, 0);

    gs_blend_state_pop();
    gs_texture_destroy(alphaTexture);
#endif
}

// ---------------------------------------------------------------------------
// Build the obs_source_info struct
// ---------------------------------------------------------------------------

struct obs_source_info build_ai_matte_info()
{
    struct obs_source_info info = {};

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

const void *ai_matte_filter_process_frame(const void *input_data,
                                           uint32_t width,
                                           uint32_t height,
                                           uint32_t *out_width,
                                           uint32_t *out_height)
{
    if (out_width)  *out_width  = width;
    if (out_height) *out_height = height;
    return input_data;
}