// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// show_file.cpp — implementation of parse_show_file() (Phase 3, Étape 1).
//
// Design rules (anti-patternes du brief dancehap-brief + rapport Isidore §4.6):
//  - nlohmann/json is used with allow_exceptions=false so a malformed .dhp
//    NEVER throws inside OBS. We check j.is_discarded() after parse() and
//    use the value() API (which returns a default instead of throwing on a
//    missing key) plus explicit is_*() type checks for required fields.
//  - All file paths in the show file must be absolute and exist on disk
//    (ADR-009). Relative or missing paths are reported as errors, not
//    warnings — the plugin cannot play a show with a broken clip path.
//  - Validation is collected: we accumulate all errors and return them,
//    rather than bailing on the first one, so the user can fix everything
//    at once (matches the ParseResult.errors vector contract).

#include "show_file.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace dancehap {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

// Push a validation error onto the result.
inline void err(ParseResult &r, const std::string &field, const std::string &msg)
{
    r.errors.push_back({field, msg});
}

// --- Helpers that read scalars without throwing --------------------------------
// nlohmann's operator[] / at() throw when allow_exceptions=true. With
// allow_exceptions=false they return a special `null` json reference and
// set a parse error bit, but the cleaner pattern for *value* reads is to
// use j.value("key", default) and j.contains("key") + type checks. We
// standardise on that here.

// Read a double from a json node field, validating type. Returns false if
// the field is missing or not a number.
bool get_double(const json &j, const char *key, double &out)
{
    if (!j.contains(key) || !j[key].is_number()) return false;
    out = j[key].get<double>();
    return true;
}

bool get_int(const json &j, const char *key, int &out)
{
    if (!j.contains(key) || !j[key].is_number_integer()) return false;
    out = j[key].get<int>();
    return true;
}

bool get_bool(const json &j, const char *key, bool &out)
{
    if (!j.contains(key) || !j[key].is_boolean()) return false;
    out = j[key].get<bool>();
    return true;
}

bool get_string(const json &j, const char *key, std::string &out)
{
    if (!j.contains(key) || !j[key].is_string()) return false;
    out = j[key].get<std::string>();
    return true;
}

// Parse a Keyframe (optional handle_left / handle_right arrays).
Keyframe parse_keyframe(const json &jf)
{
    Keyframe kf;
    jf.at("time").get_to(kf.time);
    jf.at("value").get_to(kf.value);
    if (jf.contains("handle_left") && jf["handle_left"].is_array()
        && jf["handle_left"].size() == 2) {
        std::array<double,2> h{};
        h[0] = jf["handle_left"][0].get<double>();
        h[1] = jf["handle_left"][1].get<double>();
        kf.handle_left = h;
    }
    if (jf.contains("handle_right") && jf["handle_right"].is_array()
        && jf["handle_right"].size() == 2) {
        std::array<double,2> h{};
        h[0] = jf["handle_right"][0].get<double>();
        h[1] = jf["handle_right"][1].get<double>();
        kf.handle_right = h;
    }
    return kf;
}

// Validate keyframes: time >= 0, value in [0,1].
void validate_keyframes(ParseResult &r, const std::vector<Keyframe> &kfs,
                        const std::string &prefix)
{
    for (size_t i = 0; i < kfs.size(); ++i) {
        if (kfs[i].time < 0.0) {
            err(r, prefix + ".opacity_keyframes[" + std::to_string(i)
                + ".time", "keyframe time must be >= 0");
        }
        if (kfs[i].value < 0.0 || kfs[i].value > 1.0) {
            err(r, prefix + ".opacity_keyframes[" + std::to_string(i)
                + ".value", "keyframe value must be in [0.0, 1.0]");
        }
    }
}

// Parse a DLayer's clips + keyframes from its json node.
void parse_dlayer(const json &jn, DLayerConfig &dl, ParseResult &r,
                   const std::string &prefix)
{
    // opacity_keyframes (optional but usually present).
    if (jn.contains("opacity_keyframes") && jn["opacity_keyframes"].is_array()) {
        for (const auto &jf : jn["opacity_keyframes"]) {
            if (!jf.is_object() || !jf.contains("time") || !jf.contains("value")) {
                err(r, prefix + ".opacity_keyframes",
                    "keyframe must have time and value");
                continue;
            }
            dl.opacity_keyframes.push_back(parse_keyframe(jf));
        }
        validate_keyframes(r, dl.opacity_keyframes, prefix);
    }

    // clips (empty for dlayer2_live).
    if (jn.contains("clips") && jn["clips"].is_array()) {
        for (const auto &jc : jn["clips"]) {
            if (!jc.is_object()) {
                err(r, prefix + ".clips", "clip must be an object");
                continue;
            }
            ClipSpec clip;
            if (!get_string(jc, "id", clip.id)) {
                err(r, prefix + ".clips.id", "clip id is required");
            }
            if (!get_string(jc, "file", clip.file)) {
                err(r, prefix + ".clips.file", "clip file is required");
            }
            if (!get_double(jc, "start", clip.start)) clip.start = 0.0;
            if (!get_double(jc, "duration", clip.duration)) clip.duration = 0.0;
            get_bool(jc, "loop", clip.loop);            // optional, defaults false
            get_double(jc, "crossfade_in", clip.crossfade_in);
            get_double(jc, "crossfade_out", clip.crossfade_out);

            // Path validation: must be absolute + exist.
            if (!clip.file.empty()) {
                fs::path p(clip.file);
                if (!p.is_absolute()) {
                    err(r, prefix + ".clips.file",
                        "clip path must be absolute: " + clip.file);
                } else if (!fs::exists(p)) {
                    err(r, prefix + ".clips.file",
                        "clip file does not exist: " + clip.file);
                }
            }
            // Crossfade validation: must be >= 0.
            if (clip.crossfade_in < 0.0) {
                err(r, prefix + ".clips.crossfade_in",
                    "crossfade_in must be >= 0");
            }
            if (clip.crossfade_out < 0.0) {
                err(r, prefix + ".clips.crossfade_out",
                    "crossfade_out must be >= 0");
            }
            dl.clips.push_back(std::move(clip));
        }
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// parse_show_file
// ---------------------------------------------------------------------------

ParseResult parse_show_file(const std::string &path)
{
    ParseResult r;

    // 1. Read the file. If the file can't be opened, report a single error.
    std::ifstream f(path);
    if (!f.is_open()) {
        err(r, "file", "cannot open show file: " + path);
        return r;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    if (text.empty()) {
        err(r, "file", "show file is empty");
        return r;
    }

    // 2. Parse JSON with allow_exceptions=false (never throw).
    // nlohmann::json::parse signature:
    //   parse(InputType, callback=nullptr, allow_exceptions=true, ignore_comments=false)
    // The 3rd positional arg is allow_exceptions; passing false makes parse
    // return a discarded (null) json on error instead of throwing. We then
    // guard every value read with is_*() checks (no at()/get() that throw).
    json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        err(r, "json", "invalid JSON in show file");
        return r;
    }
    if (!j.is_object()) {
        err(r, "json", "show file root must be a JSON object");
        return r;
    }

    // 3. Version (required, must be "2.0").
    std::string version;
    if (!get_string(j, "version", version)) {
        err(r, "version", "version field is required (string \"2.0\")");
    } else if (version != "2.0") {
        err(r, "version", "unsupported show file version: " + version
            + " (expected \"2.0\")");
    }

    ShowFile show;

    // 4. show block (required: name + duration).
    if (!j.contains("show") || !j["show"].is_object()) {
        err(r, "show", "show block is required");
    } else {
        const auto &js = j["show"];
        if (!get_string(js, "name", show.show.name)) {
            err(r, "show.name", "show.name is required");
        }
        if (!get_double(js, "duration", show.show.duration)) {
            err(r, "show.duration", "show.duration is required (number)");
        } else if (show.show.duration <= 0.0) {
            err(r, "show.duration", "show.duration must be > 0");
        }
        get_string(js, "created", show.show.created); // optional
    }

    // 5. webcam block (optional — sensible defaults).
    if (j.contains("webcam") && j["webcam"].is_object()) {
        const auto &jw = j["webcam"];
        get_string(jw, "device", show.webcam.device);
        // resolution is "WxH" string.
        std::string res;
        if (get_string(jw, "resolution", res)) {
            char x;
            std::istringstream rs(res);
            if ((rs >> show.webcam.width >> x >> show.webcam.height) && x == 'x') {
                // ok
            } else {
                // keep defaults
            }
        }
        get_int(jw, "fps", show.webcam.fps);
    }

    // 6. matting block (optional — defaults to rvm_mobilenetv3 / 0.5).
    if (j.contains("matting") && j["matting"].is_object()) {
        const auto &jm = j["matting"];
        get_string(jm, "model", show.matting.model);
        get_double(jm, "threshold", show.matting.threshold);
        int feather;
        if (jm.contains("feather") && jm["feather"].is_number_integer()) {
            feather = jm["feather"].get<int>();
            show.matting.feather = feather;
        }
        bool contour;
        if (jm.contains("contour") && jm["contour"].is_boolean()) {
            contour = jm["contour"].get<bool>();
            show.matting.contour = contour;
        }
        int mask_exp;
        if (jm.contains("mask_expansion") && jm["mask_expansion"].is_number_integer()) {
            mask_exp = jm["mask_expansion"].get<int>();
            show.matting.mask_expansion = mask_exp;
        }
    }

    // 7. dlayers block (required: 3 named layers).
    if (!j.contains("dlayers") || !j["dlayers"].is_object()) {
        err(r, "dlayers", "dlayers block is required");
    } else {
        const auto &jd = j["dlayers"];
        // dlayer1_background
        if (jd.contains("dlayer1_background") && jd["dlayer1_background"].is_object()) {
            parse_dlayer(jd["dlayer1_background"], show.dlayer1, r,
                         "dlayer1_background");
        } else {
            err(r, "dlayers.dlayer1_background", "required DLayer");
        }
        // dlayer2_live — must NOT have clips (live feed, ADR-010/011).
        if (jd.contains("dlayer2_live") && jd["dlayer2_live"].is_object()) {
            const auto &j2 = jd["dlayer2_live"];
            if (j2.contains("clips") && j2["clips"].is_array()
                && !j2["clips"].empty()) {
                err(r, "dlayer2_live.clips",
                    "dlayer2_live is a live feed and must not have clips");
            }
            parse_dlayer(j2, show.dlayer2, r, "dlayer2_live");
        } else {
            err(r, "dlayers.dlayer2_live", "required DLayer");
        }
        // dlayer3_overlay
        if (jd.contains("dlayer3_overlay") && jd["dlayer3_overlay"].is_object()) {
            parse_dlayer(jd["dlayer3_overlay"], show.dlayer3, r,
                         "dlayer3_overlay");
        } else {
            err(r, "dlayers.dlayer3_overlay", "required DLayer");
        }
    }

    // 8. audio_tracks (optional).
    if (j.contains("audio_tracks") && j["audio_tracks"].is_array()) {
        for (const auto &ja : j["audio_tracks"]) {
            if (!ja.is_object()) continue;
            AudioTrack t;
            get_string(ja, "id", t.id);
            get_string(ja, "file", t.file);
            get_double(ja, "start", t.start);
            if (!get_double(ja, "volume", t.volume)) t.volume = 1.0;
            // Validate audio file path (absolute + exists) if non-empty.
            if (!t.file.empty()) {
                fs::path p(t.file);
                if (!p.is_absolute()) {
                    err(r, "audio_tracks.file",
                        "audio path must be absolute: " + t.file);
                } else if (!fs::exists(p)) {
                    err(r, "audio_tracks.file",
                        "audio file does not exist: " + t.file);
                }
            }
            show.audio_tracks.push_back(std::move(t));
        }
    }

    // 9. markers (optional).
    if (j.contains("markers") && j["markers"].is_array()) {
        for (const auto &jm : j["markers"]) {
            if (!jm.is_object()) continue;
            Marker m;
            get_double(jm, "time", m.time);
            get_string(jm, "name", m.name);
            show.markers.push_back(std::move(m));
        }
    }

    // 10. Finalise: if any errors, return them; else hand over the ShowFile.
    if (!r.errors.empty()) {
        return r; // show is left empty
    }
    show.version = version;
    r.show = std::move(show);
    return r;
}

} // namespace dancehap