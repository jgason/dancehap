// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// DanceHAP — OBS plugin module entry point.
//
// Phase 1.1: obs_module_load() registers the hap_clip_source.
//
// When DANCEHAP_HAVE_OBS is defined, real OBS headers are used and the
// locale / module-name macros work. Otherwise the stub layer (obs_compat.hpp)
// defines the entry points and types so it compiles standalone.

#ifdef DANCEHAP_HAVE_OBS
#  include <obs-module.h>
   OBS_DECLARE_MODULE()
   OBS_MODULE_USE_DEFAULT_LOCALE("dancehap", "en-US")
#else
#  include "plugin.hpp"
#endif

#include "dancehap/version.h"
#include "hap_clip_source.hpp"
#include "ai_matte_filter.hpp"

// ---------------------------------------------------------------------------
// Module lifecycle
// ---------------------------------------------------------------------------

/// Called by OBS when the plugin is loaded. Returns true on success.
/// Registers the hap_clip_source (Phase 1.1) and ai_matte_filter (Phase 2.0).
bool obs_module_load(void)
{
#ifdef DANCEHAP_HAVE_OBS
    blog(LOG_INFO, "[DanceHAP] module loaded (v%s)", DANCEHAP_VERSION_STRING);
#endif
    register_hap_clip_source();
    register_ai_matte_filter();
    return true;
}

/// Called by OBS when the plugin is unloaded (OBS shutting down).
void obs_module_unload(void)
{
    // Phase 1.0: no resources to release.
}

/// Called by OBS after all modules have been loaded.
void obs_module_post_load(void)
{
    // Phase 1.0: no-op.
}

// ---------------------------------------------------------------------------
// Locale — in OBS 31+ these are defined by OBS_MODULE_USE_DEFAULT_LOCALE
// macro. Only define them in stub mode.
// ---------------------------------------------------------------------------

#ifndef DANCEHAP_HAVE_OBS
void obs_module_set_locale(const char * /*locale*/)
{
    // Stub mode only.
}

void obs_module_free_locale(void)
{
    // Stub mode only.
}
#endif

// ---------------------------------------------------------------------------
// Module metadata
// ---------------------------------------------------------------------------

const char *obs_module_name(void)
{
    return "DanceHAP";
}

const char *obs_module_description(void)
{
    return "Immersive dance scenes: HAP video clips + real-time webcam "
           "matting + alpha overlays. Phase 1.0 skeleton.";
}

// NOTE: obs_module_ver() is provided by the OBS_DECLARE_MODULE macro in
// real-OBS builds (libobs/obs-module.h). In stub mode it is declared in
// plugin.hpp and must be defined here.
#ifndef DANCEHAP_HAVE_OBS
unsigned int obs_module_ver(void)
{
    return DANCEHAP_VERSION_UINT32;
}
#endif
