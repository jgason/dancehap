// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// hap_clip_source.hpp — OBS source that plays HAP video clips (video + audio).
//
// Phase 1.1: registration + property view. Decoding arrives in 1.2-1.4.
// See ARCHITECTURE.md §4.1 and docs/plans/PLAN-PHASE-1.md étape 1.1.

#pragma once

#include "obs_compat.hpp"

// Unique OBS source identifier.
#define HAP_CLIP_SOURCE_ID   "dancehap_hap_clip"

// Human-readable name shown in OBS's Add → Source menu.
#define HAP_CLIP_SOURCE_NAME "DanceHAP Clip"

#ifdef __cplusplus
extern "C" {
#endif

/// Register the hap_clip_source with OBS.
/// Called from obs_module_load().
void register_hap_clip_source(void);

/// Return a pointer to the static obs_source_info struct.
/// Available in both real-OBS and stub modes; in stub mode the struct
/// members are directly inspectable by unit tests.
const struct obs_source_info *hap_clip_source_get_info(void);

#ifdef __cplusplus
} // extern "C"
#endif
