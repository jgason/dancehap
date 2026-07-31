// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// obs_compat.hpp — Compatibility shim for compiling DanceHAP source/filter
// code both WITH real OBS headers (DANCEHAP_HAVE_OBS=1) and WITHOUT (stub
// mode, for local dev and unit tests).
//
// In real OBS mode the standard OBS headers provide every type, macro and
// function.  In stub mode this header provides minimal OBS-compatible
// definitions so that plugin code compiles and unit tests can exercise the
// callback logic without a libobs installation.
//
// The stub structs (obs_data, obs_properties) expose their internals so that
// unit tests can verify behaviour through the standard OBS API.

#pragma once

#include <cstdint>
#include <cstddef>

// ============================================================================
//  Real OBS mode
// ============================================================================
#ifdef DANCEHAP_HAVE_OBS

#include <obs-module.h>
#include <obs-source.h>
#include <obs-data.h>
#include <obs-properties.h>

// obs_register_source is a macro defined by OBS — nothing extra needed.
// NOTE: obs-frontend-api.h (for dock registration) is included by
// dancehap_dock.cpp directly, not here, because it lives in frontend/api/
// which is not always in the include path on all OBS SDK setups.

// ============================================================================
//  Stub mode
// ============================================================================
#else

#include <string>
#include <map>
#include <vector>

// ---- Opaque / concrete types ----------------------------------------------

// Forward-declared opaque types (matching OBS typedefs).
// obs_data and obs_properties are defined below with test-visible members.
struct obs_source;            // opaque — the OBS source handle
struct gs_effect;             // opaque — the graphics effect
struct gs_texture;            // opaque — the graphics texture (Phase 1.3)
typedef struct obs_source  obs_source_t_inner;
typedef struct gs_effect  gs_effect_t_inner;
typedef struct gs_texture gs_texture_t_inner;

// obs_data: simple key-value store.
// Members are public for unit-test inspection (stub mode only).
struct obs_data {
    std::map<std::string, std::string> strings;
    std::map<std::string, bool>        bools;
    std::map<std::string, long long>   ints;
    std::map<std::string, std::string> default_strings;
    std::map<std::string, bool>        default_bools;
    std::map<std::string, long long>   default_ints;
    long ref_count = 1;
};
typedef struct obs_data obs_data;

// obs_properties: ordered property list.
// Members are public for unit-test inspection (stub mode only).
struct obs_properties {
    struct property {
        std::string name;
        std::string description;
        std::string kind;   // "path", "bool", "int", "list"
        std::string filter; // file filter (path properties only)
        int min_val = 0;    // min value (int properties only)
        int max_val = 0;    // max value (int properties only)
        int step_val = 1;   // step value (int properties only)
        bool visible = true; // Phase 2.6: visibility toggle
    };
    std::vector<property> props;
};
typedef struct obs_properties obs_properties;

// Pointer typedefs used in OBS function signatures.
// In OBS: obs_data_t is the struct, obs_data_t * is the handle.
// For stub consistency we mirror this: typedef-name = struct-name.
// (obs_source_t, gs_effect_t, gs_texture_t remain opaque.)
typedef struct obs_data       obs_data_t;
typedef struct obs_properties obs_properties_t;
typedef struct obs_source    obs_source_t;
typedef struct gs_effect     gs_effect_t;
typedef struct gs_texture    gs_texture_t;

// Phase 3: additional opaque graphics types needed for composite source.
struct gs_technique;          // opaque — a technique within an effect
struct gs_eparam;             // opaque — an effect parameter
typedef struct gs_technique  gs_technique_t;
typedef struct gs_eparam     gs_eparam_t;

// Phase 3: obs_source_frame — frame data from async sources (webcam).
// In real OBS this is a full struct with data[], linesize, width, height,
// format, timestamp. In stub mode we expose a minimal version for tests.
struct obs_source_frame {
    uint8_t *data[8]   = {};  // plane pointers (BGRA in [0])
    int       linesize[8] = {};  // row stride per plane
    uint32_t  width      = 0;
    uint32_t  height     = 0;
    int       format     = 0;  // enum video_format (stub: raw int)
    int64_t   timestamp  = 0; // nanoseconds
    long      refs       = 1;
};
typedef struct obs_source_frame obs_source_frame_t;

// Phase 3: hotkey types (minimal stub).
// FIX 5 (Étape 9): obs_hotkey_id is typedef'd as uint64_t here in stub mode.
// In real OBS mode (DANCEHAP_HAVE_OBS), obs-hotkey.h defines its own
// obs_hotkey_id. The two are never visible at the same time (this section
// is inside the #else / stub block), so there is no redefinition conflict.
// obs_compat.hpp does NOT include obs-hotkey.h in stub mode.
typedef uint64_t obs_hotkey_id;
typedef uint64_t obs_hotkey_pair_id;
struct obs_hotkey;
typedef struct obs_hotkey obs_hotkey_t;

// Phase 3: hotkey callback type (matches OBS obs-hotkey.h).
// Real OBS signature: void (*)(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed).
typedef void (*obs_hotkey_func)(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);

// ---- Enums ----------------------------------------------------------------

// Source types (values match OBS obs-source.h)
enum obs_source_type {
    OBS_SOURCE_TYPE_INPUT,
    OBS_SOURCE_TYPE_FILTER,
    OBS_SOURCE_TYPE_TRANSITION,
    OBS_SOURCE_TYPE_SCENE,
};

// Path type for obs_properties_add_path (matches OBS obs-properties.h)
enum obs_path_type {
    OBS_PATH_FILE,
    OBS_PATH_FILE_SAVE,
    OBS_PATH_DIRECTORY,
};

// Combo types and formats for obs_properties_add_list (matches OBS obs-properties.h)
enum obs_combo_type {
    OBS_COMBO_TYPE_INVALID,
    OBS_COMBO_TYPE_EDITABLE,
    OBS_COMBO_TYPE_LIST,
};

enum obs_combo_format {
    OBS_COMBO_FORMAT_INVALID,
    OBS_COMBO_FORMAT_INT,
    OBS_COMBO_FORMAT_FLOAT,
    OBS_COMBO_FORMAT_STRING,
    OBS_COMBO_FORMAT_BOOL,
};

// obs_property is opaque in real OBS; stub uses a lightweight stand-in.
struct obs_property;
typedef struct obs_property obs_property_t;

// ---- Flags (matching OBS obs-source.h 31.x) ------------------------------

#define OBS_SOURCE_VIDEO             (1u << 0)
#define OBS_SOURCE_AUDIO             (1u << 1)
#define OBS_SOURCE_ASYNC             (1u << 2)
#define OBS_SOURCE_CUSTOM_DRAW       (1u << 3)
#define OBS_SOURCE_INTERACTION       (1u << 5)
#define OBS_SOURCE_COMPOSITE         (1u << 6)
#define OBS_SOURCE_DO_NOT_DUPLICATE  (1u << 7)
#define OBS_SOURCE_DEPRECATED        (1u << 8)
#define OBS_SOURCE_DO_NOT_SELF_MONITOR (1u << 9)

// ---- Log levels (matching OBS util/base.h base_level) --------------------

#define LOG_ERROR   100
#define LOG_WARNING 200
#define LOG_INFO    300
#define LOG_DEBUG   400

// ---- Source info struct ---------------------------------------------------
// Mirrors the layout of the real OBS struct for the fields used in Phase 1.1.
// Fields beyond video_render are omitted; obs_register_source in stub mode
// does not care about size beyond recording it.

struct obs_source_info {
    const char *id;
    enum obs_source_type type;
    uint32_t output_flags;

    const char *(*get_name)(void *type_data);
    void *(*create)(obs_data_t *settings, obs_source_t *source);
    void (*destroy)(void *data);
    uint32_t (*get_width)(void *data);
    uint32_t (*get_height)(void *data);

    void (*get_defaults)(obs_data_t *settings);
    obs_properties_t *(*get_properties)(void *data);
    void (*update)(void *data, obs_data_t *settings);
    void (*activate)(void *data);
    void (*deactivate)(void *data);
    void (*show)(void *data);
    void (*hide)(void *data);
    void (*video_tick)(void *data, float seconds);
    void (*video_render)(void *data, gs_effect_t *effect);
};

// ---- Stub function declarations (implemented in obs_stub.cpp) -------------

// obs_data
obs_data_t       *obs_data_create(void);
void              obs_data_release(obs_data_t *data);
void              obs_data_set_default_string(obs_data_t *settings,
                                              const char *name, const char *val);
void              obs_data_set_default_bool(obs_data_t *settings,
                                            const char *name, bool val);
void              obs_data_set_default_int(obs_data_t *settings,
                                           const char *name, long long val);
const char       *obs_data_get_string(obs_data_t *settings, const char *name);
void              obs_data_set_string(obs_data_t *settings, const char *name, const char *val);
bool              obs_data_get_bool(obs_data_t *settings, const char *name);
long long         obs_data_get_int(obs_data_t *settings, const char *name);

// obs_properties
obs_properties_t *obs_properties_create(void);
void              obs_properties_destroy(obs_properties_t *props);
obs_property_t *obs_properties_add_path(obs_properties_t *props,
                                         const char *name,
                                         const char *description,
                                         enum obs_path_type type,
                                         const char *filter,
                                         const char *default_path);
obs_property_t *obs_properties_add_bool(obs_properties_t *props,
                                        const char *name,
                                        const char *description);
obs_property_t *obs_properties_add_int(obs_properties_t *props,
                                       const char *name,
                                       const char *description,
                                       int min_val, int max_val, int step_val);
obs_property_t *obs_properties_add_list(obs_properties_t *props,
                                        const char *name,
                                        const char *description,
                                        enum obs_combo_type type,
                                        enum obs_combo_format format);
void obs_property_list_add_int(obs_property_t *prop,
                               const char *name, long long val);

// Property visibility and modified-callback (Phase 2.6 multi-model UI)
obs_property_t *obs_properties_get(obs_properties_t *props, const char *name);
void obs_property_set_visible(obs_property_t *prop, bool visible);

// Modified callback type — follows OBS API signature.
// Returns true if the properties need a refresh.
typedef bool (*obs_property_modified_cb)(obs_properties_t *props,
                                         obs_property_t *prop,
                                         obs_data_t *settings);
void obs_property_set_modified_callback(obs_property_t *prop,
                                        obs_property_modified_cb callback);

// i18n (stub: returns key as-is)
const char *obs_module_text(const char *key);

// Source registration
void obs_register_source_s(const struct obs_source_info *info, std::size_t size);
#define obs_register_source(info) obs_register_source_s(info, sizeof(struct obs_source_info))

// ---------------------------------------------------------------------------+
// Phase 3: Graphics API stubs (for composite source video_render)
// ---------------------------------------------------------------------------+

// Graphics context enter/leave (for loading effects in update()).
void obs_enter_graphics(void);
void obs_leave_graphics(void);

// Effect loading and parameter management.
gs_effect_t *gs_effect_create_from_file(const char *file, const char *cache_filename);
void         gs_effect_destroy(gs_effect_t *effect);
gs_eparam_t *gs_effect_get_param_by_name(gs_effect_t *effect, const char *name);
void         gs_effect_set_float(gs_eparam_t *param, float val);
void         gs_effect_set_texture(gs_eparam_t *param, gs_texture_t *tex);
gs_technique_t *gs_effect_get_technique(gs_effect_t *effect, const char *name);

// Technique lifecycle.
size_t gs_technique_begin(gs_technique_t *tech);
void   gs_technique_begin_pass(gs_technique_t *tech, size_t pass);
void   gs_technique_end_pass(gs_technique_t *tech);
void   gs_technique_end(gs_technique_t *tech);

// Drawing.
void gs_draw_sprite(gs_texture_t *tex, uint32_t flags, uint32_t width, uint32_t height);

// Blend state (for alpha compositing).
void gs_blend_state_push(void);
void gs_blend_state_pop(void);
void gs_blend_function(int src, int dst);

// Blend enums (matching OBS graphics.h).
#define GS_BLEND_ZERO            0
#define GS_BLEND_ONE             1
#define GS_BLEND_INVSRCALPHA     2
#define GS_BLEND_SRCALPHA        3

// Module file path (for locating .effect files).
const char *obs_module_file(const char *file);

// ---------------------------------------------------------------------------+
// Phase 3: Graphics texture creation (for webcam frames upload)
// ---------------------------------------------------------------------------+

// Texture format enum (subset matching OBS graphics.h).
enum gs_color_format {
    GS_BGRA = 1,
    GS_RGBA = 4,
    GS_R8   = 7,
};

#define GS_ZS_NONE 0

// Create a 2D texture from raw pixel data.
gs_texture_t *gs_texture_create(uint32_t width, uint32_t height,
                                  enum gs_color_format format,
                                  uint32_t levels,
                                  const uint8_t **data,
                                  uint32_t flags);
void           gs_texture_destroy(gs_texture_t *tex);

// Update an existing 2D texture's pixel data in-place (no re-allocation).
// Used to reuse a webcam texture across frames instead of creating a new one
// each frame (texture leak #3 fix, Étape 9).
void           gs_texture_set_image(gs_texture_t *tex,
                                     const uint8_t *data,
                                     uint32_t linesize,
                                     bool flip);

// ---------------------------------------------------------------------------+
// Phase 3: Source lifecycle stubs (for private webcam source, hotkeys)
// ---------------------------------------------------------------------------+

// Create a private (invisible) source — for internal webcam capture.
obs_source_t *obs_source_create_private(const char *id, const char *name,
                                        obs_data_t *settings);
void          obs_source_release(obs_source_t *source);
void          obs_source_addref(obs_source_t *source);

// Async source frame access (correct for async sources like webcams).
obs_source_frame_t *obs_source_get_frame(obs_source_t *source);
void                obs_source_release_frame(obs_source_t *source,
                                             obs_source_frame_t *frame);

// Hotkey registration (tied to a source — auto-unregistered on destroy).
obs_hotkey_id obs_hotkey_register_source(obs_source_t *source,
                                          const char *name,
                                          const char *description,
                                          obs_hotkey_func func,
                                          void *client_data);
void          obs_hotkey_unregister(obs_hotkey_id id);

// Frontend dock API (Phase 3 stub — dock itself comes in Étape 6).
bool obs_frontend_add_dock_by_id(const char *id, const char *title, void *widget);
void obs_frontend_remove_dock(const char *id);

// ---------------------------------------------------------------------------+
// Phase 3 Étape 8: Audio output API stubs
// ---------------------------------------------------------------------------+

// Audio format enum (subset matching OBS audio-io.h).
enum audio_format {
    AUDIO_FORMAT_UNKNOWN = 0,
    AUDIO_FORMAT_U8BIT,
    AUDIO_FORMAT_U8BIT_PLANAR,
    AUDIO_FORMAT_16BIT,
    AUDIO_FORMAT_16BIT_PLANAR,
    AUDIO_FORMAT_32BIT,
    AUDIO_FORMAT_32BIT_PLANAR,
    AUDIO_FORMAT_FLOAT,
    AUDIO_FORMAT_FLOAT_PLANAR,
};

// Speaker layout enum (subset matching OBS audio-io.h).
enum speaker_layout {
    SPEAKERS_UNKNOWN = 0,
    SPEAKERS_MONO,
    SPEAKERS_STEREO,
    SPEAKERS_2POINT1,
    SPEAKERS_4POINT0,
    SPEAKERS_4POINT1,
    SPEAKERS_5POINT1,
    SPEAKERS_7POINT1,
};

// Audio output struct (matches OBS obs-output.h obs_source_audio).
struct obs_source_audio {
    const uint8_t *data[8];
    uint32_t       frames;
    enum speaker_layout speakers;
    enum audio_format   format;
    uint32_t       samples_per_sec;
    int64_t        timestamp;
};

// Push audio data to OBS (called from video_tick to route audio).
void obs_source_output_audio(obs_source_t *source,
                              const struct obs_source_audio *audio);

// High-resolution timer (for audio timestamps).
int64_t os_gettime_ns(void);

// ---- Logging (no-op in stub) ----------------------------------------------

#define blog(level, ...) ((void)0)

// ---- Test helpers (stub mode only) ----------------------------------------

// Reset all stub state (registered sources, etc.). Call before each test.
void obs_stub_reset(void);

// Number of sources registered since last reset.
int obs_stub_registration_count(void);

// Pointer to the info of the most-recently registered source, or nullptr.
const struct obs_source_info *obs_stub_last_registered_source(void);

// Phase 3 extended test helpers.
int obs_stub_graphics_refcount(void);
int obs_stub_private_sources_created(void);
int obs_stub_hotkey_count(void);
int obs_stub_docks_added(void);
void obs_stub_set_module_data_path(const char *path);

// Phase 3 Étapes 5-8 test helpers.
int obs_stub_textures_created(void);
int obs_stub_textures_destroyed(void);
int obs_stub_audio_output_count(void);

#endif // DANCEHAP_HAVE_OBS
