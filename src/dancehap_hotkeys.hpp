// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// dancehap_hotkeys.hpp — OBS hotkey registration for DanceHAP (Phase 3, Étape 7).
// Registers Play/Stop + 1 hotkey per marker, tied to the composite source.
// All OBS calls guarded by #ifdef DANCEHAP_HAVE_OBS — stub mode = no-op.

#pragma once

#include <string>
#include <vector>

#ifdef DANCEHAP_HAVE_OBS
struct obs_source;
struct obs_hotkey;
using obs_hotkey_id = int;
#endif

/// Manages hotkey lifecycle for a DanceHAP composite source.
/// Play/Stop are registered at create(), markers at load_show_file().
struct HotkeyManager {
#ifdef DANCEHAP_HAVE_OBS
    std::vector<obs_hotkey_id> marker_hotkey_ids;
    obs_hotkey_id play_hotkey_id = 0;
    obs_hotkey_id stop_hotkey_id = 0;
    obs_source *source = nullptr;
    void *composite_ctx = nullptr;
#endif
    bool registered = false;

    /// Register Play/Stop hotkeys (call at source create).
    /// No-op in stub mode.
    void register_play_stop(const std::string &source_name);

    /// Register marker hotkeys from the show file (call at show load).
    /// Unregisters old marker hotkeys first.
    /// No-op in stub mode.
    void register_markers(const std::vector<std::string> &marker_names);

    /// Unregister all hotkeys (call at source destroy).
    void unregister_all();

    /// Check if hotkeys are registered.
    bool is_registered() const { return registered; }
};