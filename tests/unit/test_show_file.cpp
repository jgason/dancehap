// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// Phase 3 Étape 1 — Show file parser unit tests (TDD RED-GREEN-REFACTOR).
// Tests written FIRST; show_file.hpp/.cpp must make them pass.
//
// These tests validate parse_show_file(path) against .dhp fixtures in
// tests/assets/. They run in stub mode (no OBS, no FFmpeg) — the parser
// only depends on nlohmann/json and std::filesystem.

#include <gtest/gtest.h>

#include "show_file.hpp"

#include <cstdio>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

// Build the absolute path to a .dhp fixture under tests/assets.
std::string asset(const char *name)
{
    std::string dir = DANCEHAP_TEST_ASSETS_DIR;
    return dir + "/" + name;
}

// Write a temp .dhp with arbitrary content (for invalid-content tests).
// mkstemp requires the template to END with exactly 6 'X' chars, so we
// create the file then rename it to the .dhp suffix.
std::string write_temp(const std::string &content)
{
    char tmpl[] = "/tmp/dancehap_test_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return {};
    FILE *fp = fdopen(fd, "w");
    if (!fp) { close(fd); return {}; }
    std::fputs(content.c_str(), fp);
    std::fclose(fp);
    std::string final = std::string(tmpl) + ".dhp";
    rename(tmpl, final.c_str());
    return final;
}

void cleanup(const std::string &p)
{
    if (!p.empty()) std::filesystem::remove(p);
}

} // namespace

// ---------------------------------------------------------------------------
// ParseError structure
// ---------------------------------------------------------------------------

TEST(ShowFile, ParseErrorHasFields)
{
    ParseError e{"show.name", "missing field"};
    EXPECT_EQ(e.field, "show.name");
    EXPECT_EQ(e.message, "missing field");
}

TEST(ShowFile, ParseResultHoldsErrorsWhenFailed)
{
    ParseResult r;
    // Default-constructed ParseResult: no show, no errors
    EXPECT_FALSE(r);                        // operator bool → false (no show)
    EXPECT_TRUE(r.errors.empty());          // no errors yet
    // Now simulate a failed parse
    r.errors.push_back(ParseError{"show.name", "missing"});
    EXPECT_FALSE(r);                        // still false (no show)
    EXPECT_FALSE(r.errors.empty());         // errors present
}

TEST(ShowFile, ParseResultHoldsShowWhenOk)
{
    ParseResult r;
    r.show = ShowFile{};
    EXPECT_TRUE(r);
    EXPECT_TRUE(r.show.has_value());
}

// ---------------------------------------------------------------------------
// Non-existent / unreadable files
// ---------------------------------------------------------------------------

TEST(ShowFile, NonExistentFileReturnsError)
{
    auto r = parse_show_file(asset("does_not_exist.dhp"));
    EXPECT_FALSE(r);
    EXPECT_FALSE(r.errors.empty());
    EXPECT_EQ(r.errors[0].field, "file");
}

TEST(ShowFile, MalformedJsonReturnsError)
{
    std::string p = write_temp("{ this is not valid json ]");
    auto r = parse_show_file(p);
    EXPECT_FALSE(r);
    EXPECT_FALSE(r.errors.empty());
    EXPECT_EQ(r.errors[0].field, "json");
    cleanup(p);
}

TEST(ShowFile, EmptyFileReturnsError)
{
    std::string p = write_temp("");
    auto r = parse_show_file(p);
    EXPECT_FALSE(r);
    EXPECT_FALSE(r.errors.empty());
    cleanup(p);
}

// ---------------------------------------------------------------------------
// Version validation
// ---------------------------------------------------------------------------

TEST(ShowFile, InvalidVersionReturnsError)
{
    auto r = parse_show_file(asset("show_invalid.dhp"));
    EXPECT_FALSE(r);
    ASSERT_FALSE(r.errors.empty());
    // Error must mention version.
    bool has_version_err = false;
    for (const auto &e : r.errors) {
        if (e.field.find("version") != std::string::npos) has_version_err = true;
    }
    EXPECT_TRUE(has_version_err);
}

TEST(ShowFile, UnknownVersionReturnsError)
{
    std::string p = write_temp(R"({"version":"9.9","show":{"name":"x","duration":1.0}})");
    auto r = parse_show_file(p);
    EXPECT_FALSE(r);
    bool has_version_err = false;
    for (const auto &e : r.errors) {
        if (e.field.find("version") != std::string::npos) has_version_err = true;
    }
    EXPECT_TRUE(has_version_err);
    cleanup(p);
}

// ---------------------------------------------------------------------------
// Required fields
// ---------------------------------------------------------------------------

TEST(ShowFile, MissingShowReturnsError)
{
    std::string p = write_temp(R"({"version":"2.0"})");
    auto r = parse_show_file(p);
    EXPECT_FALSE(r);
    bool has_show_err = false;
    for (const auto &e : r.errors) {
        if (e.field.find("show") != std::string::npos) has_show_err = true;
    }
    EXPECT_TRUE(has_show_err);
    cleanup(p);
}

TEST(ShowFile, MissingShowNameReturnsError)
{
    std::string p = write_temp(R"({"version":"2.0","show":{"duration":10.0}})");
    auto r = parse_show_file(p);
    EXPECT_FALSE(r);
    bool has_name_err = false;
    for (const auto &e : r.errors) {
        if (e.field.find("name") != std::string::npos) has_name_err = true;
    }
    EXPECT_TRUE(has_name_err);
    cleanup(p);
}

TEST(ShowFile, MissingShowDurationReturnsError)
{
    std::string p = write_temp(R"({"version":"2.0","show":{"name":"x"}})");
    auto r = parse_show_file(p);
    EXPECT_FALSE(r);
    bool has_dur_err = false;
    for (const auto &e : r.errors) {
        if (e.field.find("duration") != std::string::npos) has_dur_err = true;
    }
    EXPECT_TRUE(has_dur_err);
    cleanup(p);
}

TEST(ShowFile, NegativeDurationReturnsError)
{
    std::string p = write_temp(R"({"version":"2.0","show":{"name":"x","duration":-1.0}})");
    auto r = parse_show_file(p);
    EXPECT_FALSE(r);
    bool has_dur_err = false;
    for (const auto &e : r.errors) {
        if (e.field.find("duration") != std::string::npos) has_dur_err = true;
    }
    EXPECT_TRUE(has_dur_err);
    cleanup(p);
}

// ---------------------------------------------------------------------------
// Valid simple show
// ---------------------------------------------------------------------------

TEST(ShowFile, ParsesSimpleShowSuccessfully)
{
    auto r = parse_show_file(asset("show_simple.dhp"));
    EXPECT_TRUE(r) << "errors: " << (r.errors.empty() ? "" : r.errors[0].message);
    ASSERT_TRUE(r.show.has_value());
    const auto &s = *r.show;
    EXPECT_EQ(s.version, "2.0");
    EXPECT_EQ(s.show.name, "Simple Show");
    EXPECT_DOUBLE_EQ(s.show.duration, 60.0);
}

TEST(ShowFile, SimpleShowWebcamParsed)
{
    auto r = parse_show_file(asset("show_simple.dhp"));
    ASSERT_TRUE(r);
    const auto &w = r.show->webcam;
    EXPECT_EQ(w.device, "default");
    EXPECT_EQ(w.width, 1280);
    EXPECT_EQ(w.height, 720);
    EXPECT_EQ(w.fps, 30);
}

TEST(ShowFile, SimpleShowMattingParsed)
{
    auto r = parse_show_file(asset("show_simple.dhp"));
    ASSERT_TRUE(r);
    const auto &m = r.show->matting;
    EXPECT_EQ(m.model, "rvm_mobilenetv3");
    EXPECT_DOUBLE_EQ(m.threshold, 0.5);
}

TEST(ShowFile, SimpleShowDLayer1ClipsParsed)
{
    auto r = parse_show_file(asset("show_simple.dhp"));
    ASSERT_TRUE(r);
    const auto &d1 = r.show->dlayer1;
    ASSERT_EQ(d1.clips.size(), 1u);
    EXPECT_EQ(d1.clips[0].id, "bg1");
    EXPECT_EQ(d1.clips[0].file, "/root/dancehap/tests/assets/sample_hapa_5s.mov");
    EXPECT_DOUBLE_EQ(d1.clips[0].start, 0.0);
    EXPECT_DOUBLE_EQ(d1.clips[0].duration, 30.0);
    EXPECT_TRUE(d1.clips[0].loop);
    EXPECT_DOUBLE_EQ(d1.clips[0].crossfade_in, 0.0);
    EXPECT_DOUBLE_EQ(d1.clips[0].crossfade_out, 1.0);
}

TEST(ShowFile, SimpleShowOpacityKeyframesParsed)
{
    auto r = parse_show_file(asset("show_simple.dhp"));
    ASSERT_TRUE(r);
    ASSERT_EQ(r.show->dlayer1.opacity_keyframes.size(), 2u);
    EXPECT_DOUBLE_EQ(r.show->dlayer1.opacity_keyframes[0].time, 0.0);
    EXPECT_DOUBLE_EQ(r.show->dlayer1.opacity_keyframes[0].value, 1.0);
    EXPECT_DOUBLE_EQ(r.show->dlayer1.opacity_keyframes[1].time, 60.0);
    EXPECT_DOUBLE_EQ(r.show->dlayer1.opacity_keyframes[1].value, 1.0);
}

TEST(ShowFile, SimpleShowDLayer2LiveHasNoClips)
{
    auto r = parse_show_file(asset("show_simple.dhp"));
    ASSERT_TRUE(r);
    EXPECT_TRUE(r.show->dlayer2.clips.empty());
}

TEST(ShowFile, SimpleShowMarkersParsed)
{
    auto r = parse_show_file(asset("show_simple.dhp"));
    ASSERT_TRUE(r);
    ASSERT_EQ(r.show->markers.size(), 2u);
    EXPECT_DOUBLE_EQ(r.show->markers[0].time, 0.0);
    EXPECT_EQ(r.show->markers[0].name, "Start");
    EXPECT_DOUBLE_EQ(r.show->markers[1].time, 30.0);
    EXPECT_EQ(r.show->markers[1].name, "Cue 2");
}

// ---------------------------------------------------------------------------
// Full show — handles, multiple clips, audio tracks, matting extras
// ---------------------------------------------------------------------------

TEST(ShowFile, ParsesFullShowSuccessfully)
{
    auto r = parse_show_file(asset("show_full.dhp"));
    EXPECT_TRUE(r);
    ASSERT_TRUE(r.show.has_value());
    EXPECT_EQ(r.show->version, "2.0");
    EXPECT_EQ(r.show->show.name, "Full Show Take That Circus");
    EXPECT_DOUBLE_EQ(r.show->show.duration, 3600.0);
}

TEST(ShowFile, FullShowMattingExtrasParsed)
{
    auto r = parse_show_file(asset("show_full.dhp"));
    ASSERT_TRUE(r);
    const auto &m = r.show->matting;
    EXPECT_EQ(m.model, "mediapipe_selfie");
    EXPECT_DOUBLE_EQ(m.threshold, 0.7);
    ASSERT_TRUE(m.feather.has_value());
    EXPECT_EQ(*m.feather, 2);
    ASSERT_TRUE(m.contour.has_value());
    EXPECT_TRUE(*m.contour);
    ASSERT_TRUE(m.mask_expansion.has_value());
    EXPECT_EQ(*m.mask_expansion, 0);
}

TEST(ShowFile, FullShowMultipleClipsParsed)
{
    auto r = parse_show_file(asset("show_full.dhp"));
    ASSERT_TRUE(r);
    ASSERT_EQ(r.show->dlayer1.clips.size(), 2u);
    EXPECT_EQ(r.show->dlayer1.clips[1].id, "bg_shine");
    EXPECT_DOUBLE_EQ(r.show->dlayer1.clips[1].start, 30.0);
    EXPECT_DOUBLE_EQ(r.show->dlayer1.clips[1].crossfade_in, 1.0);
    EXPECT_DOUBLE_EQ(r.show->dlayer1.clips[1].crossfade_out, 2.0);
}

TEST(ShowFile, FullShowKeyframeHandlesParsed)
{
    auto r = parse_show_file(asset("show_full.dhp"));
    ASSERT_TRUE(r);
    const auto &kfs = r.show->dlayer1.opacity_keyframes;
    ASSERT_GE(kfs.size(), 2u);
    // First keyframe has handle_right only.
    ASSERT_TRUE(kfs[0].handle_right.has_value());
    EXPECT_DOUBLE_EQ((*kfs[0].handle_right)[0], 0.1);
    EXPECT_DOUBLE_EQ((*kfs[0].handle_right)[1], 0.0);
    EXPECT_FALSE(kfs[0].handle_left.has_value());
    // Second keyframe has both handles.
    ASSERT_TRUE(kfs[1].handle_left.has_value());
    ASSERT_TRUE(kfs[1].handle_right.has_value());
}

TEST(ShowFile, FullShowAudioTracksParsed)
{
    auto r = parse_show_file(asset("show_full.dhp"));
    ASSERT_TRUE(r);
    ASSERT_EQ(r.show->audio_tracks.size(), 2u);
    EXPECT_EQ(r.show->audio_tracks[0].id, "track_main");
    EXPECT_DOUBLE_EQ(r.show->audio_tracks[0].start, 0.0);
    EXPECT_DOUBLE_EQ(r.show->audio_tracks[0].volume, 0.8);
    EXPECT_EQ(r.show->audio_tracks[1].id, "track_cue");
    EXPECT_DOUBLE_EQ(r.show->audio_tracks[1].start, 30.0);
    EXPECT_DOUBLE_EQ(r.show->audio_tracks[1].volume, 1.0);
}

TEST(ShowFile, FullShowMarkersParsed)
{
    auto r = parse_show_file(asset("show_full.dhp"));
    ASSERT_TRUE(r);
    ASSERT_EQ(r.show->markers.size(), 3u);
    EXPECT_EQ(r.show->markers[2].name, "Greatest Day");
    EXPECT_DOUBLE_EQ(r.show->markers[2].time, 45.0);
}

TEST(ShowFile, FullShowOverlayClipParsed)
{
    auto r = parse_show_file(asset("show_full.dhp"));
    ASSERT_TRUE(r);
    ASSERT_EQ(r.show->dlayer3.clips.size(), 1u);
    EXPECT_FALSE(r.show->dlayer3.clips[0].loop);
}

// ---------------------------------------------------------------------------
// Path validation
// ---------------------------------------------------------------------------

TEST(ShowFile, NonExistentClipFileReturnsError)
{
    std::string p = write_temp(R"({
      "version":"2.0",
      "show":{"name":"x","duration":10.0},
      "dlayers":{"dlayer1_background":{"clips":[
        {"id":"c1","file":"/nonexistent/clip.mov","start":0.0,"duration":5.0}
      ],"opacity_keyframes":[{"time":0.0,"value":1.0}]},
      "dlayer2_live":{"opacity_keyframes":[{"time":0.0,"value":0.0}]},
      "dlayer3_overlay":{"clips":[],"opacity_keyframes":[{"time":0.0,"value":0.0}]}
      }
    })");
    auto r = parse_show_file(p);
    EXPECT_FALSE(r);
    bool has_path_err = false;
    for (const auto &e : r.errors) {
        if (e.field.find("file") != std::string::npos) has_path_err = true;
    }
    EXPECT_TRUE(has_path_err);
    cleanup(p);
}

TEST(ShowFile, RelativeClipPathReturnsError)
{
    std::string p = write_temp(R"({
      "version":"2.0",
      "show":{"name":"x","duration":10.0},
      "dlayers":{"dlayer1_background":{"clips":[
        {"id":"c1","file":"relative/clip.mov","start":0.0,"duration":5.0}
      ],"opacity_keyframes":[{"time":0.0,"value":1.0}]},
      "dlayer2_live":{"opacity_keyframes":[{"time":0.0,"value":0.0}]},
      "dlayer3_overlay":{"clips":[],"opacity_keyframes":[{"time":0.0,"value":0.0}]}
      }
    })");
    auto r = parse_show_file(p);
    EXPECT_FALSE(r);
    bool has_path_err = false;
    for (const auto &e : r.errors) {
        if (e.field.find("file") != std::string::npos) has_path_err = true;
    }
    EXPECT_TRUE(has_path_err);
    cleanup(p);
}

// ---------------------------------------------------------------------------
// Keyframe validation
// ---------------------------------------------------------------------------

TEST(ShowFile, KeyframeValueOutOfRangeReturnsError)
{
    std::string p = write_temp(R"({
      "version":"2.0",
      "show":{"name":"x","duration":10.0},
      "dlayers":{"dlayer1_background":{"clips":[],"opacity_keyframes":[
        {"time":0.0,"value":1.5}
      ]},
      "dlayer2_live":{"opacity_keyframes":[{"time":0.0,"value":0.0}]},
      "dlayer3_overlay":{"clips":[],"opacity_keyframes":[{"time":0.0,"value":0.0}]}
      }
    })");
    auto r = parse_show_file(p);
    EXPECT_FALSE(r);
    bool has_kf_err = false;
    for (const auto &e : r.errors) {
        if (e.field.find("value") != std::string::npos) has_kf_err = true;
    }
    EXPECT_TRUE(has_kf_err);
    cleanup(p);
}

TEST(ShowFile, KeyframeNegativeTimeReturnsError)
{
    std::string p = write_temp(R"({
      "version":"2.0",
      "show":{"name":"x","duration":10.0},
      "dlayers":{"dlayer1_background":{"clips":[],"opacity_keyframes":[
        {"time":-1.0,"value":0.5}
      ]},
      "dlayer2_live":{"opacity_keyframes":[{"time":0.0,"value":0.0}]},
      "dlayer3_overlay":{"clips":[],"opacity_keyframes":[{"time":0.0,"value":0.0}]}
      }
    })");
    auto r = parse_show_file(p);
    EXPECT_FALSE(r);
    bool has_kf_err = false;
    for (const auto &e : r.errors) {
        if (e.field.find("time") != std::string::npos) has_kf_err = true;
    }
    EXPECT_TRUE(has_kf_err);
    cleanup(p);
}

TEST(ShowFile, KeyframeNegativeCrossfadeReturnsError)
{
    std::string p = write_temp(R"({
      "version":"2.0",
      "show":{"name":"x","duration":10.0},
      "dlayers":{"dlayer1_background":{"clips":[
        {"id":"c1","file":"/root/dancehap/tests/assets/sample_hapa_5s.mov","start":0.0,"duration":5.0,"crossfade_in":-1.0}
      ],"opacity_keyframes":[{"time":0.0,"value":1.0}]},
      "dlayer2_live":{"opacity_keyframes":[{"time":0.0,"value":0.0}]},
      "dlayer3_overlay":{"clips":[],"opacity_keyframes":[{"time":0.0,"value":0.0}]}
      }
    })");
    auto r = parse_show_file(p);
    EXPECT_FALSE(r);
    bool has_cf_err = false;
    for (const auto &e : r.errors) {
        if (e.field.find("crossfade") != std::string::npos) has_cf_err = true;
    }
    EXPECT_TRUE(has_cf_err);
    cleanup(p);
}

// ---------------------------------------------------------------------------
// DLayer2 must not have clips (it's a live feed)
// ---------------------------------------------------------------------------

TEST(ShowFile, DLayer2WithClipsReturnsError)
{
    std::string p = write_temp(R"({
      "version":"2.0",
      "show":{"name":"x","duration":10.0},
      "dlayers":{
        "dlayer1_background":{"clips":[],"opacity_keyframes":[{"time":0.0,"value":1.0}]},
        "dlayer2_live":{"clips":[{"id":"c1","file":"/root/dancehap/tests/assets/sample_hapa_5s.mov","start":0.0,"duration":5.0}],"opacity_keyframes":[{"time":0.0,"value":0.0}]},
        "dlayer3_overlay":{"clips":[],"opacity_keyframes":[{"time":0.0,"value":0.0}]}
      }
    })");
    auto r = parse_show_file(p);
    EXPECT_FALSE(r);
    bool has_dlayer2_err = false;
    for (const auto &e : r.errors) {
        if (e.field.find("dlayer2") != std::string::npos) has_dlayer2_err = true;
    }
    EXPECT_TRUE(has_dlayer2_err);
    cleanup(p);
}

// ---------------------------------------------------------------------------
// Defaults for optional fields
// ---------------------------------------------------------------------------

TEST(ShowFile, MissingMattingSectionUsesDefaults)
{
    std::string p = write_temp(R"({
      "version":"2.0",
      "show":{"name":"x","duration":10.0},
      "dlayers":{
        "dlayer1_background":{"clips":[],"opacity_keyframes":[{"time":0.0,"value":1.0}]},
        "dlayer2_live":{"opacity_keyframes":[{"time":0.0,"value":0.0}]},
        "dlayer3_overlay":{"clips":[],"opacity_keyframes":[{"time":0.0,"value":0.0}]}
      }
    })");
    auto r = parse_show_file(p);
    EXPECT_TRUE(r) << (r.errors.empty() ? "" : r.errors[0].message);
    ASSERT_TRUE(r.show.has_value());
    EXPECT_EQ(r.show->matting.model, "rvm_mobilenetv3");
    EXPECT_DOUBLE_EQ(r.show->matting.threshold, 0.5);
    cleanup(p);
}

TEST(ShowFile, MissingWebcamSectionUsesDefaults)
{
    std::string p = write_temp(R"({
      "version":"2.0",
      "show":{"name":"x","duration":10.0},
      "dlayers":{
        "dlayer1_background":{"clips":[],"opacity_keyframes":[{"time":0.0,"value":1.0}]},
        "dlayer2_live":{"opacity_keyframes":[{"time":0.0,"value":0.0}]},
        "dlayer3_overlay":{"clips":[],"opacity_keyframes":[{"time":0.0,"value":0.0}]}
      }
    })");
    auto r = parse_show_file(p);
    EXPECT_TRUE(r);
    ASSERT_TRUE(r.show.has_value());
    EXPECT_EQ(r.show->webcam.device, "default");
    EXPECT_EQ(r.show->webcam.width, 1280);
    EXPECT_EQ(r.show->webcam.height, 720);
    EXPECT_EQ(r.show->webcam.fps, 30);
    cleanup(p);
}

TEST(ShowFile, ClipDefaultsLoopFalseAndCrossfadeZero)
{
    std::string p = write_temp(R"({
      "version":"2.0",
      "show":{"name":"x","duration":10.0},
      "dlayers":{
        "dlayer1_background":{"clips":[
          {"id":"c1","file":"/root/dancehap/tests/assets/sample_hapa_5s.mov","start":0.0,"duration":5.0}
        ],"opacity_keyframes":[{"time":0.0,"value":1.0}]},
        "dlayer2_live":{"opacity_keyframes":[{"time":0.0,"value":0.0}]},
        "dlayer3_overlay":{"clips":[],"opacity_keyframes":[{"time":0.0,"value":0.0}]}
      }
    })");
    auto r = parse_show_file(p);
    EXPECT_TRUE(r);
    ASSERT_TRUE(r.show.has_value());
    ASSERT_EQ(r.show->dlayer1.clips.size(), 1u);
    EXPECT_FALSE(r.show->dlayer1.clips[0].loop);
    EXPECT_DOUBLE_EQ(r.show->dlayer1.clips[0].crossfade_in, 0.0);
    EXPECT_DOUBLE_EQ(r.show->dlayer1.clips[0].crossfade_out, 0.0);
    cleanup(p);
}

// ---------------------------------------------------------------------------
// No crash guarantee — malformed input never throws
// ---------------------------------------------------------------------------

TEST(ShowFile, GarbageInputDoesNotThrow)
{
    std::string p = write_temp(R"({{{""""}}})");
    EXPECT_NO_THROW({
        auto r = parse_show_file(p);
        (void)r;
    });
    cleanup(p);
}

TEST(ShowFile, TruncatedJsonDoesNotThrow)
{
    std::string p = write_temp(R"({"version":"2.0","show":{"name":)");
    EXPECT_NO_THROW({
        auto r = parse_show_file(p);
        (void)r;
    });
    cleanup(p);
}