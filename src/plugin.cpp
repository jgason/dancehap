// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// DanceHAP — OBS plugin module entry point (Phase 1.0 skeleton).
//
// Phase 1.0: obs_module_load() returns true but registers nothing.
// Phase 1.1 will register the hap_clip_source via obs_source_info.
//
// When DANCEHAP_HAVE_OBS is defined, real OBS headers are used and the
// locale / module-name macros work. Otherwise the skeleton defines the
// entry points directly (see plugin.hpp) so it compiles standalone.

#ifdef DANCEHAP_HAVE_OBS
#  include <obs-module.h>
   OBS_DECLARE_MODULE()
   OBS_MODULE_USE_DEFAULT_LOCALE("dancehap")
#else
#  include "plugin.hpp"
#endif

#include "dancehap/version.h"

// ---------------------------------------------------------------------------
// Module lifecycle
// ---------------------------------------------------------------------------

/// Called by OBS when the plugin is loaded. Returns true on success.
/// Phase 1.0: always succeeds, registers no sources/filters yet.
bool obs_module_load(void)
{
#ifdef DANCEHAP_HAVE_OBS
    blog(LOG_INFO, "[DanceHAP] module loaded (v%s)", DANCEHAP_VERSION_STRING);
#endif
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
// Locale (stub for skeleton — real i18n in Phase 5)
// ---------------------------------------------------------------------------

void obs_module_set_locale(const char * /*locale*/)
{
    // Phase 1.0: no locale resources.
}

void obs_module_free_locale(void)
{
    // Phase 1.0: no locale resources.
}

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

unsigned int obs_module_ver(void)
{
    return DANCEHAP_VERSION_UINT32;
}
