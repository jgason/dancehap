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
        std::string kind;   // "path", "bool", "int"
        std::string filter; // file filter (path properties only)
        int min_val = 0;    // min value (int properties only)
        int max_val = 0;    // max value (int properties only)
        int step_val = 1;   // step value (int properties only)
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
typedef struct obs_source     obs_source_t;
typedef struct gs_effect      gs_effect_t;
typedef struct gs_texture     gs_texture_t;

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
bool              obs_data_get_bool(obs_data_t *settings, const char *name);
long long         obs_data_get_int(obs_data_t *settings, const char *name);

// obs_properties
obs_properties_t *obs_properties_create(void);
void              obs_properties_destroy(obs_properties_t *props);
obs_properties_t *obs_properties_add_path(obs_properties_t *props,
                                          const char *name,
                                          const char *description,
                                          enum obs_path_type type,
                                          const char *filter,
                                          const char *default_path);
obs_properties_t *obs_properties_add_bool(obs_properties_t *props,
                                          const char *name,
                                          const char *description);
obs_properties_t *obs_properties_add_int(obs_properties_t *props,
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

// i18n (stub: returns key as-is)
const char *obs_module_text(const char *key);

// Source registration
void obs_register_source_s(const struct obs_source_info *info, std::size_t size);
#define obs_register_source(info) obs_register_source_s(info, sizeof(struct obs_source_info))

// ---- Logging (no-op in stub) ----------------------------------------------

#define blog(level, ...) ((void)0)

// ---- Test helpers (stub mode only) ----------------------------------------

// Reset all stub state (registered sources, etc.). Call before each test.
void obs_stub_reset(void);

// Number of sources registered since last reset.
int obs_stub_registration_count(void);

// Pointer to the info of the most-recently registered source, or nullptr.
const struct obs_source_info *obs_stub_last_registered_source(void);

#endif // DANCEHAP_HAVE_OBS
