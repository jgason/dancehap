// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// dancehap_hotkeys.cpp — OBS hotkey implementation (Phase 3, Étape 7).
// All OBS calls guarded by #ifdef DANCEHAP_HAVE_OBS.

#include "dancehap_hotkeys.hpp"

#ifdef DANCEHAP_HAVE_OBS
#include "obs_compat.hpp"
#endif

void HotkeyManager::register_play_stop(const std::string &source_name)
{
#ifdef DANCEHAP_HAVE_OBS
    if (registered || !source) return;

    // Play hotkey
    std::string play_name = "DanceHAP.Play";
    std::string play_desc = "DanceHAP: Play Show";
    play_hotkey_id = obs_hotkey_register_source(source,
        play_name.c_str(), play_desc.c_str(),
        [](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
            if (pressed && data) {
                // TODO: call composite->play()
            }
        },
        composite_ctx);

    // Stop hotkey
    std::string stop_name = "DanceHAP.Stop";
    std::string stop_desc = "DanceHAP: Stop Show";
    stop_hotkey_id = obs_hotkey_register_source(source,
        stop_name.c_str(), stop_desc.c_str(),
        [](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
            if (pressed && data) {
                // TODO: call composite->stop()
            }
        },
        composite_ctx);

    registered = true;
#endif
}

void HotkeyManager::register_markers(const std::vector<std::string> &marker_names)
{
#ifdef DANCEHAP_HAVE_OBS
    if (!source) return;

    // Unregister old marker hotkeys first (leçon du rapport d'Isidore §4.9)
    for (auto id : marker_hotkey_ids) {
        obs_hotkey_unregister(id);
    }
    marker_hotkey_ids.clear();

    // Register new marker hotkeys (1 per marker).
    // OBS hotkey callbacks are C function pointers — lambdas with captures
    // cannot convert to function pointers. We store the marker index in a
    // static vector indexed by hotkey registration order, and pass the index
    // as the void *data. The callback retrieves the index from data.
    static std::vector<size_t> s_marker_indices;
    s_marker_indices.clear();
    for (size_t i = 0; i < marker_names.size(); i++) {
        std::string name = "DanceHAP.Marker." + std::to_string(i);
        std::string desc = "DanceHAP: Jump to " + marker_names[i];
        s_marker_indices.push_back(i);
        obs_hotkey_id id = obs_hotkey_register_source(source,
            name.c_str(), desc.c_str(),
            [](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
                if (pressed && data) {
                    size_t idx = reinterpret_cast<size_t>(data);
                    // TODO: call composite->jump_to_marker(idx)
                    (void)idx;
                }
            },
            reinterpret_cast<void *>(i));
        marker_hotkey_ids.push_back(id);
    }
#endif
}

void HotkeyManager::unregister_all()
{
#ifdef DANCEHAP_HAVE_OBS
    for (auto id : marker_hotkey_ids) {
        obs_hotkey_unregister(id);
    }
    marker_hotkey_ids.clear();

    if (registered) {
        obs_hotkey_unregister(play_hotkey_id);
        obs_hotkey_unregister(stop_hotkey_id);
        registered = false;
    }
#endif
}