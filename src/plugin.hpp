// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// DanceHAP — OBS plugin module entry point (Phase 1.0 skeleton).
//
// This file defines the OBS module interface. When DANCEHAP_HAVE_OBS is
// defined (Phase 1.1+), it includes <obs-module.h> and uses the official
// OBS_DECLARE_MODULE() / OBS_MODULE_USE_DEFAULT_LOCALE() macros. When
// not defined (skeleton build), it provides equivalent forward declarations
// so the MODULE compiles without any OBS dependency installed.
//
// See: docs/plans/PLAN-PHASE-1.md, étape 1.0.

#ifndef DANCEHAP_PLUGIN_H
#define DANCEHAP_PLUGIN_H

#ifdef __cplusplus
extern "C" {
#endif

// --- OBS module interface (symbols resolved by OBS via dlopen/LoadLibrary) ---
// These are the minimal entry points OBS calls when loading the plugin .dll/.so.
bool     obs_module_load(void);
void     obs_module_unload(void);
void     obs_module_post_load(void);
void     obs_module_set_locale(const char *locale);
void     obs_module_free_locale(void);
const char *obs_module_name(void);
const char *obs_module_description(void);
unsigned int obs_module_ver(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // DANCEHAP_PLUGIN_H
