// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// obs_stub.cpp — Stub implementations of OBS API functions.
// Compiled ONLY when DANCEHAP_HAVE_OBS is not defined (stub mode).
// Provides functional-enough implementations of obs_data, obs_properties,
// and source-registration so that plugin code compiles and unit tests can
// verify behaviour through the standard OBS API surface.

#include "obs_compat.hpp"

#ifndef DANCEHAP_HAVE_OBS

#include <chrono>

// ---------------------------------------------------------------------------
// Stub global state (for test verification)
// ---------------------------------------------------------------------------

namespace {

// Registration tracking
int g_registration_count = 0;
const obs_source_info *g_last_source = nullptr;

// Phase 3 stub state (declared here so obs_stub_reset can zero them).
// These are defined and used in the Phase 3 stub sections below.
int g_graphics_refcount = 0;
int g_private_sources_created = 0;
int g_sources_released = 0;
int g_hotkey_count = 0;
uint64_t g_next_hotkey_id = 1;
int g_docks_added = 0;
std::string g_module_data_path = "data/";

// Phase 3 Étapes 5-8 stub state (declared here so obs_stub_reset can zero them).
int g_textures_created = 0;
int g_textures_destroyed = 0;
int g_audio_output_count = 0;

} // anonymous namespace

// ---------------------------------------------------------------------------
// obs_data stub
// ---------------------------------------------------------------------------

obs_data_t *obs_data_create(void)
{
    return new obs_data();
}

void obs_data_release(obs_data_t *data)
{
    if (!data) return;
    if (--data->ref_count <= 0) {
        delete data;
    }
}

void obs_data_set_default_string(obs_data_t *settings,
                                  const char *name, const char *val)
{
    if (!settings || !name) return;
    settings->default_strings[name] = val ? val : "";
}

void obs_data_set_default_bool(obs_data_t *settings,
                                const char *name, bool val)
{
    if (!settings || !name) return;
    settings->default_bools[name] = val;
}

void obs_data_set_default_int(obs_data_t *settings,
                               const char *name, long long val)
{
    if (!settings || !name) return;
    settings->default_ints[name] = val;
}

const char *obs_data_get_string(obs_data_t *settings, const char *name)
{
    if (!settings || !name) return "";
    // Explicit value takes priority, then default, then empty string.
    auto it = settings->strings.find(name);
    if (it != settings->strings.end()) return it->second.c_str();
    auto dit = settings->default_strings.find(name);
    if (dit != settings->default_strings.end()) return dit->second.c_str();
    return "";
}

void obs_data_set_string(obs_data_t *settings, const char *name, const char *val)
{
    if (!settings || !name) return;
    settings->strings[name] = val ? val : "";
}

bool obs_data_get_bool(obs_data_t *settings, const char *name)
{
    if (!settings || !name) return false;
    auto it = settings->bools.find(name);
    if (it != settings->bools.end()) return it->second;
    auto dit = settings->default_bools.find(name);
    if (dit != settings->default_bools.end()) return dit->second;
    return false;
}

long long obs_data_get_int(obs_data_t *settings, const char *name)
{
    if (!settings || !name) return 0;
    auto it = settings->ints.find(name);
    if (it != settings->ints.end()) return it->second;
    auto dit = settings->default_ints.find(name);
    if (dit != settings->default_ints.end()) return dit->second;
    return 0;
}

// ---------------------------------------------------------------------------
// obs_properties stub
// ---------------------------------------------------------------------------

obs_properties_t *obs_properties_create(void)
{
    return new obs_properties();
}

void obs_properties_destroy(obs_properties_t *props)
{
    delete props;
}

obs_property_t *obs_properties_add_path(obs_properties_t *props,
                                           const char *name,
                                           const char *description,
                                           enum obs_path_type /*type*/,
                                           const char *filter,
                                           const char * /*default_path*/)
{
    if (!props || !name) return nullptr;
    obs_properties::property p;
    p.name = name;
    p.description = description ? description : "";
    p.kind = "path";
    p.filter = filter ? filter : "";
    props->props.push_back(std::move(p));
    return reinterpret_cast<obs_property_t *>(&props->props.back());
}

obs_property_t *obs_properties_add_bool(obs_properties_t *props,
                                           const char *name,
                                           const char *description)
{
    if (!props || !name) return nullptr;
    obs_properties::property p;
    p.name = name;
    p.description = description ? description : "";
    p.kind = "bool";
    props->props.push_back(std::move(p));
    return reinterpret_cast<obs_property_t *>(&props->props.back());
}

obs_property_t *obs_properties_add_int(obs_properties_t *props,
                                          const char *name,
                                          const char *description,
                                          int min_val, int max_val, int step_val)
{
    if (!props || !name) return nullptr;
    obs_properties::property p;
    p.name = name;
    p.description = description ? description : "";
    p.kind = "int";
    p.min_val = min_val;
    p.max_val = max_val;
    p.step_val = step_val;
    props->props.push_back(std::move(p));
    return reinterpret_cast<obs_property_t *>(&props->props.back());
}

// obs_properties_add_list — stub returns props (property tracking is minimal).
obs_property_t *obs_properties_add_list(obs_properties_t *props,
                                         const char *name,
                                         const char *description,
                                         enum obs_combo_type /*type*/,
                                         enum obs_combo_format /*format*/)
{
    if (!props || !name) return nullptr;
    obs_properties::property p;
    p.name = name;
    p.description = description ? description : "";
    p.kind = "list";
    props->props.push_back(std::move(p));
    // Return a dummy non-null pointer (the last property address suffices).
    return reinterpret_cast<obs_property_t *>(&props->props.back());
}

// obs_property_list_add_int — stub no-op (returns the property).
void obs_property_list_add_int(obs_property_t * /*prop*/,
                                const char * /*name*/, long long /*val*/)
{
    // Stub: no-op. Real OBS appends an entry; we don't track in stub.
}

// obs_properties_get — find a property by name. Returns nullptr if not found.
obs_property_t *obs_properties_get(obs_properties_t *props, const char *name)
{
    if (!props || !name) return nullptr;
    for (auto &p : props->props) {
        if (p.name == name) {
            return reinterpret_cast<obs_property_t *>(&p);
        }
    }
    return nullptr;
}

// obs_property_set_visible — toggle property visibility (stub tracks in struct).
void obs_property_set_visible(obs_property_t *prop, bool visible)
{
    if (!prop) return;
    auto *p = reinterpret_cast<obs_properties::property *>(prop);
    p->visible = visible;
}

// obs_property_set_modified_callback — stub no-op (callback not invoked in stub).
void obs_property_set_modified_callback(obs_property_t * /*prop*/,
                                        obs_property_modified_cb /*callback*/)
{
    // Stub: no-op. Real OBS stores the callback for later invocation.
}

// obs_module_text — stub returns the key as-is (no i18n in stub mode).
const char *obs_module_text(const char *key)
{
    return key ? key : "";
}

// ---------------------------------------------------------------------------
// Source registration stub
// ---------------------------------------------------------------------------

void obs_register_source_s(const struct obs_source_info *info, std::size_t /*size*/)
{
    if (!info) return;
    g_registration_count++;
    g_last_source = info;
}

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

void obs_stub_reset(void)
{
    g_registration_count = 0;
    g_last_source = nullptr;
    // Phase 3 reset
    g_graphics_refcount = 0;
    g_private_sources_created = 0;
    g_sources_released = 0;
    g_hotkey_count = 0;
    g_next_hotkey_id = 1;
    g_docks_added = 0;
    g_module_data_path = "data/";
    // Phase 3 Étapes 5-8 reset
    g_textures_created = 0;
    g_textures_destroyed = 0;
    g_audio_output_count = 0;
}

int obs_stub_registration_count(void)
{
    return g_registration_count;
}

const struct obs_source_info *obs_stub_last_registered_source(void)
{
    return g_last_source;
}

// ---------------------------------------------------------------------------+
// Phase 3: Graphics API stubs
// ---------------------------------------------------------------------------+
// These are no-op/lightweight stubs that allow the composite source code to
// compile and run in stub mode. In real OBS, these are provided by libobs
// graphics subsystem.

// (g_graphics_refcount is declared in the anonymous namespace above.)

void obs_enter_graphics(void)
{
    ++g_graphics_refcount;
}

void obs_leave_graphics(void)
{
    if (g_graphics_refcount > 0)
        --g_graphics_refcount;
}

// Stub effect: just a non-null sentinel pointer. In stub mode we never
// actually load a shader file — gs_effect_create_from_file returns a
// fake handle. Tests that check "effect loaded" verify non-null.
static gs_effect_t *g_fake_effect = nullptr;

gs_effect_t *gs_effect_create_from_file(const char *file, const char * /*cache*/)
{
    // Return a stable non-null pointer (same pointer each call so tests
    // can compare). We don't actually parse the .effect file in stub mode.
    if (!g_fake_effect)
        g_fake_effect = reinterpret_cast<gs_effect_t *>(0xDEAD);
    (void)file; // unused in stub
    return g_fake_effect;
}

void gs_effect_destroy(gs_effect_t *effect)
{
    if (effect == g_fake_effect) {
        g_fake_effect = nullptr;
    }
}

gs_eparam_t *gs_effect_get_param_by_name(gs_effect_t *effect, const char *name)
{
    if (!effect || !name) return nullptr;
    // Return a fake non-null pointer. In real OBS this is a handle to the
    // uniform parameter. In stub mode we don't need to track values.
    return reinterpret_cast<gs_eparam_t *>(0xBEEF);
}

void gs_effect_set_float(gs_eparam_t * /*param*/, float /*val*/)
{
    // Stub: no-op. Real OBS sets the uniform value.
}

void gs_effect_set_texture(gs_eparam_t * /*param*/, gs_texture_t * /*tex*/)
{
    // Stub: no-op.
}

gs_technique_t *gs_effect_get_technique(gs_effect_t *effect, const char * /*name*/)
{
    if (!effect) return nullptr;
    // Return a fake non-null technique pointer.
    return reinterpret_cast<gs_technique_t *>(0xCAFE);
}

size_t gs_technique_begin(gs_technique_t * /*tech*/)
{
    return 1; // one pass
}

void gs_technique_begin_pass(gs_technique_t * /*tech*/, size_t /*pass*/)
{
    // Stub: no-op.
}

void gs_technique_end_pass(gs_technique_t * /*tech*/)
{
    // Stub: no-op.
}

void gs_technique_end(gs_technique_t * /*tech*/)
{
    // Stub: no-op.
}

void gs_draw_sprite(gs_texture_t * /*tex*/, uint32_t /*flags*/,
                    uint32_t /*width*/, uint32_t /*height*/)
{
    // Stub: no-op. Real OBS draws a textured quad.
}

void gs_blend_state_push(void)
{
    // Stub: no-op.
}

void gs_blend_state_pop(void)
{
    // Stub: no-op.
}

void gs_blend_function(int /*src*/, int /*dst*/)
{
    // Stub: no-op (g_blend_src/dst removed — not needed in stub mode).
}

// ---------------------------------------------------------------------------+
// Phase 3: Module file path stub
// ---------------------------------------------------------------------------+

// (g_module_data_path is declared in the anonymous namespace above.)

const char *obs_module_file(const char *file)
{
    if (!file) return "";
    static thread_local std::string result;
    result = g_module_data_path + file;
    return result.c_str();
}

// Test helper: set the module data path (for tests that want a specific dir).
void obs_stub_set_module_data_path(const char *path)
{
    g_module_data_path = path ? path : "data/";
}

// ---------------------------------------------------------------------------+
// Phase 3 Étape 5: Texture creation stubs (for webcam frame upload)
// ---------------------------------------------------------------------------+

// g_textures_created / g_textures_destroyed are declared in the anonymous
// namespace at the top of this file (so obs_stub_reset can zero them).

gs_texture_t *gs_texture_create(uint32_t width, uint32_t height,
                                  enum gs_color_format /*format*/,
                                  uint32_t /*levels*/,
                                  const uint8_t **data,
                                  uint32_t /*flags*/)
{
    if (data) {
        // Verify non-null data (caller passed a real pixel buffer).
        (void)data; // stub: we don't copy the data, just return a sentinel.
    }
    ++g_textures_created;
    // Return a unique fake non-null pointer each call.
    static uint64_t counter = 0x1000;
    counter += 0x100;
    return reinterpret_cast<gs_texture_t *>(counter);
}

void gs_texture_destroy(gs_texture_t *tex)
{
    if (tex) {
        ++g_textures_destroyed;
    }
}

void gs_texture_set_image(gs_texture_t *tex,
                          const uint8_t * /*data*/,
                          uint32_t /*linesize*/,
                          bool /*flip*/)
{
    // Stub: no-op. In real OBS this updates the texture's pixel buffer in-place
    // without re-allocating GPU memory. We just verify the texture handle is
    // valid (non-null) so the leak-fix code path is exercised in stub mode.
    if (tex) {
        // Texture reused — no new allocation. This lets tests that count
        // g_textures_created verify the leak fix works (1 create per dimension
        // change, not 1 per frame).
    }
}

// ---------------------------------------------------------------------------+
// Phase 3: Source lifecycle stubs (private sources, frames, hotkeys)
// ---------------------------------------------------------------------------+

// (g_private_sources_created, g_sources_released are in the anon namespace.)

obs_source_t *obs_source_create_private(const char *id, const char * /*name*/,
                                        obs_data_t * /*settings*/)
{
    // In stub mode, we return a fake source pointer. Tests can check that
    // create_private was called. We return nullptr to indicate "no webcam
    // available in stub mode" — the composite source handles this gracefully.
    (void)id;
    ++g_private_sources_created;
    return nullptr; // stub: no real webcam
}

void obs_source_release(obs_source_t *source)
{
    if (source) {
        ++g_sources_released;
    }
}

void obs_source_addref(obs_source_t * /*source*/)
{
    // Stub: no-op.
}

obs_source_frame_t *obs_source_get_frame(obs_source_t *source)
{
    if (!source) return nullptr;
    // Stub: no frames available from a fake source.
    return nullptr;
}

void obs_source_release_frame(obs_source_t * /*source*/,
                              obs_source_frame_t *frame)
{
    // Stub: no-op. Real OBS decrements ref count.
    (void)frame;
}

// ---------------------------------------------------------------------------+
// Phase 3: Hotkey stubs
// ---------------------------------------------------------------------------+

// (g_hotkey_count, g_next_hotkey_id are in the anonymous namespace.)

obs_hotkey_id obs_hotkey_register_source(obs_source_t * /*source*/,
                                          const char * /*name*/,
                                          const char * /*description*/,
                                          obs_hotkey_func /*func*/,
                                          void * /*client_data*/)
{
    ++g_hotkey_count;
    return g_next_hotkey_id++;
}

void obs_hotkey_unregister(obs_hotkey_id /*id*/)
{
    // Stub: no-op.
    if (g_hotkey_count > 0) --g_hotkey_count;
}

// ---------------------------------------------------------------------------+
// Phase 3: Frontend dock stubs
// ---------------------------------------------------------------------------+

// (g_docks_added is in the anonymous namespace.)

bool obs_frontend_add_dock_by_id(const char * /*id*/, const char * /*title*/,
                                  void * /*widget*/)
{
    ++g_docks_added;
    return true;
}

void obs_frontend_remove_dock(const char * /*id*/)
{
    // Stub: no-op.
}

// ---------------------------------------------------------------------------+
// Phase 3 Étape 8: Audio output stubs
// ---------------------------------------------------------------------------+

// g_audio_output_count is declared in the anonymous namespace at the top
// of this file (so obs_stub_reset can zero it).

void obs_source_output_audio(obs_source_t *source,
                              const struct obs_source_audio *audio)
{
    if (!source || !audio) return;
    // Stub: count calls so tests can verify audio routing happened.
    if (audio->frames > 0) {
        ++g_audio_output_count;
    }
}

int64_t os_gettime_ns(void)
{
    // Stub: return a monotonically increasing fake timestamp.
    // Uses C++ steady_clock for a real monotonic clock that's testable.
    static auto start = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - start);
    return static_cast<int64_t>(ns.count());
}

// ---------------------------------------------------------------------------+
// Phase 3: Test helpers (extended)
// ---------------------------------------------------------------------------+

int obs_stub_graphics_refcount(void) { return g_graphics_refcount; }
int obs_stub_private_sources_created(void) { return g_private_sources_created; }
int obs_stub_hotkey_count(void) { return g_hotkey_count; }
int obs_stub_docks_added(void) { return g_docks_added; }

int obs_stub_textures_created(void) { return g_textures_created; }
int obs_stub_textures_destroyed(void) { return g_textures_destroyed; }
int obs_stub_audio_output_count(void) { return g_audio_output_count; }

// Reset Phase 3 stub state (called from obs_stub_reset above — also reset here
// so the extended state is zeroed alongside the base state).
// This is called from within obs_stub_reset() via a mechanism below.

#endif // !DANCEHAP_HAVE_OBS
