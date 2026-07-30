// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// dancehap_composite.hpp — OBS source composite for DanceHAP Phase 3.
//
// Phase 3, Étapes 3-4 (ADR-010, ADR-013):
//   A single OBS source (OBS_SOURCE_TYPE_INPUT) that composites 3 DLayers
//   in real-time from a show file (.dhp):
//     DLayer 1 — HAP background timeline (clips + crossfade + B-spline opacity)
//     DLayer 2 — LIVE webcam + matting (placeholder for Étape 5)
//     DLayer 3 — HAP overlay timeline (clips + crossfade + B-spline opacity)
//
// Flags: OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_DO_NOT_DUPLICATE
//   CUSTOM_DRAW is NECESSARY here (multi-texture compositing) — OBS passes
//   a NULL effect to video_render. We load our own gs_effect_t via
//   gs_effect_create_from_file() in update() (with obs_enter_graphics()),
//   JAMAIS dans create() (no graphics context at create time — leçon bug 9).

#pragma once

#include "obs_compat.hpp"

// Unique OBS source identifier for the composite source.
#define DANCEHAP_COMPOSITE_SOURCE_ID   "dancehap_composite"

// Human-readable name shown in OBS's Add → Source menu.
#define DANCEHAP_COMPOSITE_SOURCE_NAME "DanceHAP Show"

// ---------------------------------------------------------------------------
// Transport state machine (exposed for unit tests)
// ---------------------------------------------------------------------------

namespace dancehap {

enum class TransportState {
    Stopped,   // No show loaded or explicitly stopped
    Playing,   // Timeline advancing
    Paused,    // Timeline frozen but show is loaded
};

inline const char *transport_state_to_string(TransportState s)
{
    switch (s) {
    case TransportState::Stopped: return "Stopped";
    case TransportState::Playing: return "Playing";
    case TransportState::Paused:  return "Paused";
    }
    return "Unknown";
}

} // namespace dancehap

#ifdef __cplusplus
extern "C" {
#endif

/// Register the dancehap_composite source with OBS.
/// Called from obs_module_load().
void register_dancehap_composite_source(void);

/// Return a pointer to the static obs_source_info struct.
/// Available in both real-OBS and stub modes; in stub mode the struct
/// members are directly inspectable by unit tests.
const struct obs_source_info *dancehap_composite_get_info(void);

#ifdef __cplusplus
} // extern "C"
#endif