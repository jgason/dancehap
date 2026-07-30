// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// show_file.hpp — C++ structs mapping the DanceHAP show file format (.dhp)
// and the parse_show_file() entry point. Phase 3, Étape 1 (ADR-009).
//
// The .dhp is a JSON document describing a complete stage performance:
// 3 DLayers (HAP background + live webcam matting + HAP overlay), audio
// tracks, and markers. parse_show_file() validates it (version, required
// fields, absolute+existing file paths, keyframe ranges, DLayer2 has no
// clips) and NEVER throws — it uses nlohmann/json with allow_exceptions=false
// so a malformed show file can never crash OBS.

#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace dancehap {

// ---------------------------------------------------------------------------
// Sub-structures
// ---------------------------------------------------------------------------

/// A single HAP clip on a DLayer timeline (DLayers 1 and 3).
struct ClipSpec {
    std::string id;
    std::string file;          ///< absolute path to the .mov HAP file
    double start = 0.0;        ///< seconds, start time on the show timeline
    double duration = 0.0;     ///< seconds, length of the clip
    bool loop = false;         ///< loop the clip when it ends before `start+duration`
    double crossfade_in = 0.0; ///< seconds, fade-in duration (0 = hard cut)
    double crossfade_out = 0.0;///< seconds, fade-out duration (0 = hard cut)
};

/// One opacity keyframe (B-spline control point, ADR-012).
struct Keyframe {
    double time = 0.0;   ///< seconds
    double value = 0.0;  ///< 0.0–1.0
    std::optional<std::array<double,2>> handle_left;  ///< [dx, dy] tangent (optional)
    std::optional<std::array<double,2>> handle_right; ///< [dx, dy] tangent (optional)
};

/// A DLayer = timeline of clips + opacity keyframes (ADR-010).
/// DLayer 2 (live) leaves `clips` empty.
struct DLayerConfig {
    std::vector<ClipSpec> clips;
    std::vector<Keyframe> opacity_keyframes;
};

/// Static matting configuration (one config for the whole show, ADR-010).
/// `model` is one of the 7 model keys from Phase 2.6 (rvm_mobilenetv3,
/// mediapipe_selfie, ...). Optional fields use std::optional.
struct MattingConfig {
    std::string model = "rvm_mobilenetv3";
    double threshold = 0.5;
    std::optional<int> feather;
    std::optional<bool> contour;
    std::optional<int> mask_expansion;
};

/// Internal webcam capture config (ADR-011).
struct WebcamConfig {
    std::string device = "default";
    int width = 1280;
    int height = 720;
    int fps = 30;
};

/// A standalone audio track referenced by the show (WAV/MP3/FLAC/MOV).
struct AudioTrack {
    std::string id;
    std::string file;   ///< absolute path
    double start = 0.0; ///< seconds, start on the show timeline
    double volume = 1.0; ///< 0.0–1.0
};

/// A navigation marker on the timeline (jump target for hotkeys).
struct Marker {
    double time = 0.0;
    std::string name;
};

/// Show metadata block.
struct Show {
    std::string name;
    double duration = 0.0;
    std::string created;
};

// ---------------------------------------------------------------------------
// Top-level show file
// ---------------------------------------------------------------------------

struct ShowFile {
    std::string version;          ///< "2.0"
    Show show;
    WebcamConfig webcam;
    MattingConfig matting;
    DLayerConfig dlayer1;        ///< background
    DLayerConfig dlayer2;        ///< live webcam + matting (no clips)
    DLayerConfig dlayer3;        ///< overlay
    std::vector<AudioTrack> audio_tracks;
    std::vector<Marker> markers;
};

// ---------------------------------------------------------------------------
// Error reporting (no exceptions)
// ---------------------------------------------------------------------------

/// A single validation error: which field is wrong and why.
struct ParseError {
    std::string field;   ///< dotted path, e.g. "dlayer1.clips[0].file"
    std::string message;
};

/// Result of parse_show_file(): either a valid ShowFile or a list of errors.
struct ParseResult {
    std::optional<ShowFile> show;
    std::vector<ParseError> errors;

    /// True when parsing succeeded (a ShowFile is present).
    explicit operator bool() const { return show.has_value(); }
};

/// Parse and validate a .dhp file. NEVER throws.
/// @param path  absolute or relative path to the .dhp file.
/// @return ParseResult — `.show` set on success, `.errors` filled on failure.
ParseResult parse_show_file(const std::string &path);

} // namespace dancehap

// Convenience aliases in the global namespace (matching the rest of the codebase
// which uses unqualified types in stub mode). The tests reference ShowFile,
// ParseError, ParseResult, parse_show_file directly.
using dancehap::ParseError;
using dancehap::ParseResult;
using dancehap::ShowFile;
using dancehap::parse_show_file;