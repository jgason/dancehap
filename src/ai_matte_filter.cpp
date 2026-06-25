// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// ai_matte_filter.cpp — OBS filter that applies AI-based matting to a webcam
// source.
//
// Phase 2.0: skeleton — OBS_SOURCE_TYPE_FILTER, pass-through render.
// Phase 2.2: MatteEngine ONNX inference integrated in video_tick.
// Phase 2.5: Properties UI (enable, model path, provider, quality).
// Phase 2.5b (v0.4.7): ASYNC inference + model load on a worker thread.
//   Fixes:
//     (1) Freeze on Enable — loadModel() was synchronous in update(), blocking
//         the OBS graphics thread for several seconds (DirectML EP init).
//     (2) Freeze during playback — engine.infer() was synchronous in
//         video_tick, blocking the render thread ~30 ms/frame at 256px.
//     (3) Matte invisible — video_render did a pure pass-through and never
//         used processed_frame. Now video_tick applies the processed alpha
//         in-place on the OBS frame before render.
//
// Architecture:
//   video_tick (render thread):
//     1. Copy OBS frame → input_buffer (non-blocking, ~0.1 ms for 720p).
//     2. Notify worker thread.
//     3. If output_buffer ready and same dims → memcpy to frame->data[0].
//     4. release_frame.
//   worker thread:
//     1. Wait on condition_variable (input ready OR model load requested).
//     2. If model load requested → engine.loadModel() (blocking, off-thread).
//     3. If input ready → preprocess + infer + postprocess → output_buffer.
//   video_render (render thread):
//     obs_source_default_render(target) — renders the frame that video_tick
//     may have modified in-place. No CUSTOM_DRAW needed.
//
// Reference: obs-studio/plugins/obs-filters/ (e.g. noise-filter.c, mask-filter.c)

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
// Per-instance context (with worker thread for async inference)
// ---------------------------------------------------------------------------

namespace {

struct ai_matte_context {
    bool active = false;
    bool matte_enabled = false;       // user toggle (Phase 2.5 properties)
    std::string model_path;           // ONNX model path (current setting)
    std::string loaded_model_path;    // ONNX model path (actually loaded)

#ifdef DANCEHAP_HAVE_ONNXRUNTIME
    // LAZY-INIT: the MatteEngine is NOT constructed at filter creation.
    // Constructing it triggers OrtGetApiBase() → onnxruntime.dll load
    // (13.5 MB) on the OBS graphics thread → freeze. Instead, we create
    // the engine only when the user actually enables the matte AND picks
    // a model, inside the worker thread.
    std::unique_ptr<dancehap::MatteEngine> engine;

    // Config + provider are stored here until the engine is lazily created.
    dancehap::MatteModelConfig config;
    dancehap::ExecutionProvider desired_ep = dancehap::ExecutionProvider::Auto;
    bool engine_created = false;
#endif

#ifdef DANCEHAP_HAVE_OBS
    obs_source_t *source = nullptr;   // the filter's own OBS source handle
#endif

    // ---- Worker thread (async inference + model load) -------------------

    std::thread worker;
    std::mutex mtx;                   // protects all shared buffers below
    std::condition_variable cv;
    std::atomic<bool> stop{false};

    // Input: latest frame copied from OBS (render thread writes, worker reads)
    std::vector<uint8_t> input_buffer;
    uint32_t input_w = 0;
    uint32_t input_h = 0;
    bool input_ready = false;

    // Output: last processed frame (worker writes, render thread reads)
    std::vector<uint8_t> output_buffer;
    uint32_t output_w = 0;
    uint32_t output_h = 0;
    bool output_ready = false;

    // Model load request (render thread sets, worker executes)
    std::string pending_model_path;
    bool model_load_requested = false;
    // Engine creation request (first enable) — worker creates engine off-thread
    bool engine_create_requested = false;

#ifdef DANCEHAP_HAVE_ONNXRUNTIME
    /// Worker thread main loop.
    void worker_loop()
    {
        while (!stop.load()) {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [this] {
                return stop.load() || input_ready || model_load_requested
                    || engine_create_requested;
            });

            if (stop.load()) break;

            // --- Handle engine creation (first enable, off-thread) ---
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

            // --- Handle model load request (blocking, on worker thread) ---
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
                continue;  // re-loop to check for new work
            }

            // --- Handle inference request ---
            if (input_ready) {
                std::vector<uint8_t> input = std::move(input_buffer);
                uint32_t w = input_w;
                uint32_t h = input_h;
                input_ready = false;
                lock.unlock();

                // Run inference only if engine is ready.
                if (engine && engine->isReady() && w > 0 && h > 0) {
                    dancehap::ImageFrame img;
                    img.width  = static_cast<int>(w);
                    img.height = static_cast<int>(h);
                    img.data_rgba = input.data();

                    dancehap::MatteMask mask = engine->infer(img);

                    // Apply mask to the BGRA frame.
                    std::vector<uint8_t> processed =
                        dancehap::apply_alpha_mask_to_bgra(
                            input.data(), w, h, mask.alpha);

                    // Publish to output buffer.
                    {
                        std::lock_guard<std::mutex> olock(mtx);
                        output_buffer = std::move(processed);
                        output_w = w;
                        output_h = h;
                        output_ready = true;
                    }
                }
            }
        }
    }

    void start_worker()
    {
        if (worker.joinable()) return;  // already running
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
#endif
    // LAZY-INIT: do NOT start the worker thread here, and do NOT construct
    // the MatteEngine. Both happen only when the user enables the matte
    // (see ai_matte_update). This keeps filter creation instant and
    // prevents onnxruntime.dll from being loaded until needed.
    blog(LOG_INFO, "[DanceHAP] ai_matte_filter created (lazy init, v%s)",
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
    // Update engine config (non-blocking, just sets a struct).
    dancehap::MatteModelConfig cfg;
    cfg.input_width  = res;
    cfg.input_height = res;
    ctx->config = cfg;  // store for lazy engine creation

    dancehap::ExecutionProvider ep =
        (provider == 1) ? dancehap::ExecutionProvider::DirectML :
        (provider == 2) ? dancehap::ExecutionProvider::CoreML :
        (provider == 3) ? dancehap::ExecutionProvider::CPU :
                          dancehap::ExecutionProvider::Auto;
    ctx->desired_ep = ep;  // store for lazy engine creation

    // If engine already exists, update its config.
    if (ctx->engine_created && ctx->engine) {
        ctx->engine->setConfig(cfg);
        ctx->engine->setDesiredProvider(ep);
    }

    // Request async model load if enabled and path changed.
    // The worker thread handles the actual loadModel() call so the OBS
    // graphics thread never blocks.
    if (enable && !ctx->model_path.empty()) {
        // Start worker + create engine lazily on first enable.
        if (!ctx->engine_created) {
            ctx->start_worker();
            {
                std::lock_guard<std::mutex> lock(ctx->mtx);
                ctx->engine_create_requested = true;
            }
            ctx->cv.notify_one();
            blog(LOG_INFO, "[DanceHAP] MatteEngine: engine create requested (async)");
        }

        bool need_load = !ctx->engine_created
            || ctx->model_path != ctx->loaded_model_path;
        // If engine exists and is ready and path matches, nothing to do.
        if (ctx->engine_created && ctx->engine
            && ctx->engine->isReady()
            && ctx->model_path == ctx->loaded_model_path) {
            need_load = false;
        }
        if (need_load) {
            {
                std::lock_guard<std::mutex> lock(ctx->mtx);
                ctx->pending_model_path = ctx->model_path;
                ctx->model_load_requested = true;
            }
            ctx->cv.notify_one();
            blog(LOG_INFO, "[DanceHAP] MatteEngine: model load requested (async)");
        }
    }
    // If matte disabled, just stop feeding input — engine stays loaded.
#endif
}

void ai_matte_video_tick(void *data, float /*seconds*/)
{
    auto *ctx = static_cast<ai_matte_context *>(data);
    if (!ctx) return;

#ifdef DANCEHAP_HAVE_OBS
    if (!ctx->matte_enabled) return;

    obs_source_t *target = obs_filter_get_target(ctx->source);
    if (!target) return;

    obs_source_frame *frame = obs_source_get_frame(target);
    if (!frame) return;

    uint32_t w = frame->width;
    uint32_t h = frame->height;

#ifdef DANCEHAP_HAVE_ONNXRUNTIME
    if (w > 0 && h > 0 && frame->data[0]) {
        // 1. Copy frame → input buffer (non-blocking, ~0.1 ms for 720p).
        size_t size = static_cast<size_t>(w) * h * 4;
        {
            std::lock_guard<std::mutex> lock(ctx->mtx);
            ctx->input_buffer.resize(size);
            std::memcpy(ctx->input_buffer.data(), frame->data[0], size);
            ctx->input_w = w;
            ctx->input_h = h;
            ctx->input_ready = true;
        }
        ctx->cv.notify_one();

        // 2. If output is ready and dims match → apply in-place on the OBS
        //    frame so video_render (pass-through) shows the matted result.
        {
            std::lock_guard<std::mutex> lock(ctx->mtx);
            if (ctx->output_ready &&
                ctx->output_w == w && ctx->output_h == h) {
                std::memcpy(frame->data[0], ctx->output_buffer.data(), size);
            }
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
    // Pass-through render. The frame data was modified in-place by
    // video_tick (if a processed output was ready), so the default render
    // shows the matted frame. No CUSTOM_DRAW needed — OBS wraps this in
    // the default effect's Draw technique.
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