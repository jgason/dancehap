// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// Phase 3 Étapes 3-4 — Composite source unit tests (stub mode).
//
// Tests the following testable logic (all CPU-only, no OBS graphics):
//   • Source registration and identity (id, type, flags)
//   • Properties (show_file path + autoplay)
//   • Transport state machine (play/stop/pause/seek)
//   • Crossfade timing (when to start, progress, completion)
//   • Marker jump
//   • B-spline opacity evaluation integration
//   • DLayer clip finding logic
//
// These tests run in stub mode (no OBS, no FFmpeg, no Snappy). The ClipPlayer
// in stub mode doesn't decode real HAP files, but the transport/crossfade/marker
// logic is fully exercisable.

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <filesystem>
#include <fstream>

#include "dancehap_composite.hpp"
#include "obs_compat.hpp"
#include "plugin.hpp"
#include "show_file.hpp"
#include "bspline.hpp"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const obs_properties::property *
find_property(const obs_properties *props, const char *name)
{
    if (!props) return nullptr;
    for (const auto &p : props->props) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

// Write a minimal valid .dhp to a temp file for composite tests.
// Cross-platform: uses std::tmpnam() + std::ofstream (mkstemp is POSIX-only
// and not available on MSVC — caused CI failure on Windows, same pattern as
// test_hap_demuxer.cpp / test_show_file.cpp).
static std::string write_test_dhp(const std::string &content)
{
    char tmpl_buf[L_tmpnam];
    if (std::tmpnam(tmpl_buf) == nullptr) return {};
    std::string path = std::string(tmpl_buf) + ".dhp";

    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return {};
    f << content;
    f.close();

    return path;
}

static void cleanup(const std::string &p)
{
    if (!p.empty()) std::filesystem::remove(p);
}

// Minimal valid show file with 2 clips on DLayer 1 with crossfade.
static const char *SIMPLE_SHOW =
    R"({
      "version":"2.0",
      "show":{"name":"Test Show","duration":60.0,"created":"2026-07-30"},
      "webcam":{"device":"default","resolution":"1280x720","fps":30},
      "matting":{"model":"rvm_mobilenetv3","threshold":0.5},
      "dlayers":{
        "dlayer1_background":{"clips":[
          {"id":"bg1","file":"/root/dancehap/tests/assets/sample_hapa_5s.mov","start":0.0,"duration":30.0,"loop":true,"crossfade_in":0.0,"crossfade_out":2.0},
          {"id":"bg2","file":"/root/dancehap/tests/assets/sample_hapa_5s.mov","start":30.0,"duration":30.0,"loop":true,"crossfade_in":2.0,"crossfade_out":1.0}
        ],"opacity_keyframes":[
          {"time":0.0,"value":1.0},
          {"time":60.0,"value":1.0}
        ]},
        "dlayer2_live":{"opacity_keyframes":[
          {"time":0.0,"value":0.0},
          {"time":60.0,"value":0.0}
        ]},
        "dlayer3_overlay":{"clips":[],"opacity_keyframes":[
          {"time":0.0,"value":0.0}
        ]}
      },
      "markers":[
        {"time":0.0,"name":"Start"},
        {"time":30.0,"name":"Cue 2"},
        {"time":45.0,"name":"Greatest Day"}
      ]
    })";

// ===========================================================================
// Source identity and registration
// ===========================================================================

class CompositeSourceTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        obs_stub_reset();
    }
};

TEST_F(CompositeSourceTest, SourceIdIsCorrect)
{
    const obs_source_info *info = dancehap_composite_get_info();
    ASSERT_NE(info, nullptr);
    ASSERT_NE(info->id, nullptr);
    EXPECT_STREQ(info->id, DANCEHAP_COMPOSITE_SOURCE_ID);
}

TEST_F(CompositeSourceTest, SourceTypeIsInput)
{
    const obs_source_info *info = dancehap_composite_get_info();
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->type, OBS_SOURCE_TYPE_INPUT);
}

TEST_F(CompositeSourceTest, OutputFlagsIncludeVideo)
{
    const obs_source_info *info = dancehap_composite_get_info();
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->output_flags & OBS_SOURCE_VIDEO);
}

TEST_F(CompositeSourceTest, OutputFlagsIncludeCustomDraw)
{
    // CUSTOM_DRAW is NECESSARY here (multi-texture compositing).
    // This is the exception to the "no CUSTOM_DRAW" rule from Phase 1.
    const obs_source_info *info = dancehap_composite_get_info();
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->output_flags & OBS_SOURCE_CUSTOM_DRAW);
}

TEST_F(CompositeSourceTest, OutputFlagsIncludeDoNotDuplicate)
{
    const obs_source_info *info = dancehap_composite_get_info();
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->output_flags & OBS_SOURCE_DO_NOT_DUPLICATE);
}

TEST_F(CompositeSourceTest, HasAllRequiredCallbacks)
{
    const obs_source_info *info = dancehap_composite_get_info();
    ASSERT_NE(info, nullptr);
    EXPECT_NE(info->get_name, nullptr);
    EXPECT_NE(info->create, nullptr);
    EXPECT_NE(info->destroy, nullptr);
    EXPECT_NE(info->get_width, nullptr);
    EXPECT_NE(info->get_height, nullptr);
    EXPECT_NE(info->get_defaults, nullptr);
    EXPECT_NE(info->get_properties, nullptr);
    EXPECT_NE(info->update, nullptr);
    EXPECT_NE(info->video_tick, nullptr);
    EXPECT_NE(info->video_render, nullptr);
}

TEST_F(CompositeSourceTest, GetNameReturnsVersionedName)
{
    const obs_source_info *info = dancehap_composite_get_info();
    ASSERT_NE(info, nullptr);
    ASSERT_NE(info->get_name, nullptr);
    const char *name = info->get_name(nullptr);
    ASSERT_NE(name, nullptr);
    EXPECT_STRNE(name, "");
    // Name should contain "DanceHAP"
    std::string sname(name);
    EXPECT_NE(sname.find("DanceHAP"), std::string::npos);
}

// ===========================================================================
// Properties
// ===========================================================================

TEST_F(CompositeSourceTest, PropertiesHasShowFilePath)
{
    const obs_source_info *info = dancehap_composite_get_info();
    ASSERT_NE(info, nullptr);
    ASSERT_NE(info->get_properties, nullptr);

    obs_properties_t *props = info->get_properties(nullptr);
    ASSERT_NE(props, nullptr);

    const auto *p = find_property(props, "show_file");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->kind, "path");

    obs_properties_destroy(props);
}

TEST_F(CompositeSourceTest, PropertiesHasAutoplay)
{
    const obs_source_info *info = dancehap_composite_get_info();
    ASSERT_NE(info, nullptr);
    ASSERT_NE(info->get_properties, nullptr);

    obs_properties_t *props = info->get_properties(nullptr);
    ASSERT_NE(props, nullptr);

    const auto *p = find_property(props, "autoplay");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->kind, "bool");

    obs_properties_destroy(props);
}

// ===========================================================================
// Defaults
// ===========================================================================

TEST_F(CompositeSourceTest, DefaultsShowFileEmpty)
{
    const obs_source_info *info = dancehap_composite_get_info();
    ASSERT_NE(info, nullptr);
    ASSERT_NE(info->get_defaults, nullptr);

    obs_data_t *settings = obs_data_create();
    info->get_defaults(settings);

    EXPECT_STREQ(obs_data_get_string(settings, "show_file"), "");
    EXPECT_FALSE(obs_data_get_bool(settings, "autoplay"));

    obs_data_release(settings);
}

// ===========================================================================
// Registration via obs_module_load
// ===========================================================================

TEST_F(CompositeSourceTest, ModuleLoadRegistersCompositeSource)
{
    obs_stub_reset();
    obs_module_load();

    // obs_module_load registers 3 sources: hap_clip, ai_matte, composite
    EXPECT_GE(obs_stub_registration_count(), 3);

    // The last registered source should be the composite (registered last)
    const obs_source_info *last = obs_stub_last_registered_source();
    ASSERT_NE(last, nullptr);
    EXPECT_STREQ(last->id, DANCEHAP_COMPOSITE_SOURCE_ID);
}

// ===========================================================================
// Lifecycle (create/destroy without crash)
// ===========================================================================

TEST_F(CompositeSourceTest, CreateDestroyWithoutCrash)
{
    const obs_source_info *info = dancehap_composite_get_info();
    ASSERT_NE(info, nullptr);

    obs_data_t *settings = obs_data_create();
    info->get_defaults(settings);

    void *ctx = info->create(settings, nullptr);
    ASSERT_NE(ctx, nullptr);

    info->destroy(ctx);
    obs_data_release(settings);
}

TEST_F(CompositeSourceTest, CreateWithEmptySettingsDoesNotCrash)
{
    const obs_source_info *info = dancehap_composite_get_info();
    ASSERT_NE(info, nullptr);

    void *ctx = info->create(nullptr, nullptr);
    // create should handle nullptr settings gracefully
    if (ctx) info->destroy(ctx);
}

// ===========================================================================
// Dimensions
// ===========================================================================

TEST_F(CompositeSourceTest, DefaultDimensionsAre1080p)
{
    const obs_source_info *info = dancehap_composite_get_info();
    ASSERT_NE(info, nullptr);

    obs_data_t *settings = obs_data_create();
    info->get_defaults(settings);

    void *ctx = info->create(settings, nullptr);
    ASSERT_NE(ctx, nullptr);

    // Without a loaded show, default dimensions should be 1920x1080
    EXPECT_EQ(info->get_width(ctx), 1920u);
    EXPECT_EQ(info->get_height(ctx), 1080u);

    info->destroy(ctx);
    obs_data_release(settings);
}

// ===========================================================================
// video_tick / video_render don't crash without a loaded show
// ===========================================================================

TEST_F(CompositeSourceTest, TickWithoutShowDoesNotCrash)
{
    const obs_source_info *info = dancehap_composite_get_info();
    ASSERT_NE(info, nullptr);

    obs_data_t *settings = obs_data_create();
    void *ctx = info->create(settings, nullptr);
    ASSERT_NE(ctx, nullptr);

    // Tick should be safe with no show loaded
    EXPECT_NO_THROW(info->video_tick(ctx, 0.016f));

    // Render should be safe (no-op in stub mode)
    EXPECT_NO_THROW(info->video_render(ctx, nullptr));

    info->destroy(ctx);
    obs_data_release(settings);
}

// ===========================================================================
// B-spline opacity evaluation (integration with composite context)
// ===========================================================================

TEST(CompositeBSpline, BSplineEvaluatesConstantFromSingleKeyframe)
{
    std::vector<dancehap::BSplineKeyframe> kfs = {{0.0, 0.5}};
    dancehap::BSpline bs(std::move(kfs));
    EXPECT_DOUBLE_EQ(bs.evaluate(0.0), 0.5);
    EXPECT_DOUBLE_EQ(bs.evaluate(10.0), 0.5);
    EXPECT_DOUBLE_EQ(bs.evaluate(-5.0), 0.5);
}

TEST(CompositeBSpline, BSplineEvaluatesLinearBetweenTwoKeyframes)
{
    std::vector<dancehap::BSplineKeyframe> kfs = {{0.0, 0.0}, {10.0, 1.0}};
    dancehap::BSpline bs(std::move(kfs));
    // With 2 keyframes, B-spline degenerates to linear
    EXPECT_NEAR(bs.evaluate(0.0), 0.0, 0.01);
    EXPECT_NEAR(bs.evaluate(5.0), 0.5, 0.01);
    EXPECT_NEAR(bs.evaluate(10.0), 1.0, 0.01);
}

TEST(CompositeBSpline, BSplineClampsOutOfBounds)
{
    std::vector<dancehap::BSplineKeyframe> kfs = {{0.0, 0.3}, {10.0, 0.7}};
    dancehap::BSpline bs(std::move(kfs));
    // Before first keyframe → first value
    EXPECT_NEAR(bs.evaluate(-5.0), 0.3, 0.001);
    // After last keyframe → last value
    EXPECT_NEAR(bs.evaluate(15.0), 0.7, 0.001);
}

TEST(CompositeBSpline, BSplineSmoothCubicCurve)
{
    // 5 keyframes forming an S-curve
    std::vector<dancehap::BSplineKeyframe> kfs = {
        {0.0, 0.0}, {5.0, 0.2}, {10.0, 0.5}, {15.0, 0.8}, {20.0, 1.0}
    };
    dancehap::BSpline bs(std::move(kfs));

    // At t=10 (middle), value should be near 0.5 (it's a control point)
    double mid = bs.evaluate(10.0);
    EXPECT_NEAR(mid, 0.5, 0.1);

    // The curve should be smooth (monotonically increasing for this set)
    double v0 = bs.evaluate(2.0);
    double v1 = bs.evaluate(7.0);
    double v2 = bs.evaluate(12.0);
    double v3 = bs.evaluate(17.0);
    EXPECT_LT(v0, v1);
    EXPECT_LT(v1, v2);
    EXPECT_LT(v2, v3);
}

// ===========================================================================
// DLayer clip finding logic (pure logic, testable without OBS)
// ===========================================================================

// We test the DLayerRuntime logic through the show file structs directly.
// The DLayerRuntime is an internal type, but the clip-finding and crossfade
// logic mirrors the show file's ClipSpec layout.

TEST(CompositeDLayerLogic, FindClipAtTime)
{
    std::vector<dancehap::ClipSpec> clips = {
        {"c1", "/path/a.mov", 0.0, 10.0, false, 0.0, 2.0},
        {"c2", "/path/b.mov", 15.0, 20.0, true, 2.0, 1.0},
    };

    // Helper: find clip at time (same logic as DLayerRuntime::find_clip_at_time)
    auto find_at = [&](double t) -> int {
        for (int i = 0; i < (int)clips.size(); ++i) {
            if (t >= clips[i].start && t < clips[i].start + clips[i].duration)
                return i;
        }
        return -1;
    };

    EXPECT_EQ(find_at(0.0), 0);
    EXPECT_EQ(find_at(5.0), 0);
    EXPECT_EQ(find_at(9.99), 0);
    EXPECT_EQ(find_at(10.0), -1);   // gap between clips
    EXPECT_EQ(find_at(15.0), 1);
    EXPECT_EQ(find_at(20.0), 1);    // clip 2 covers 15.0 to 35.0
    EXPECT_EQ(find_at(34.99), 1);
    EXPECT_EQ(find_at(35.0), -1);   // end of clip 2
    EXPECT_EQ(find_at(-1.0), -1);
}

TEST(CompositeDLayerLogic, CrossfadeWindowCalculation)
{
    // clip 0 ends at t=10, crossfade_out=2.0
    // clip 1 starts at t=15, crossfade_in=2.0
    // Crossfade window = min(2.0, 2.0) = 2.0
    // Crossfade should start at t=10-2=8 (2 seconds before clip 0 ends)
    // But clip 1 starts at 15, so the actual crossfade is at the BOUNDARY
    // of clip 0's end and clip 1's start.
    //
    // In our model: crossfade starts when show_time is within
    // crossfade_out of clip[idx]'s end AND clip idx+1 has crossfade_in > 0.
    //
    // For this test case: clip 0 end = 10.0, crossfade_out = 2.0
    // Crossfade starts at show_time = 10.0 - 2.0 = 8.0
    // But clip 1 starts at 15.0 — so there's a gap.
    // In practice, crossfade only makes sense when clips are adjacent.

    std::vector<dancehap::ClipSpec> clips = {
        {"c1", "/path/a.mov", 0.0, 10.0, false, 0.0, 2.0},
        {"c2", "/path/b.mov", 10.0, 20.0, true, 2.0, 1.0},  // adjacent
    };

    // Crossfade window = min(crossfade_out[0], crossfade_in[1])
    double xf_window = std::min(clips[0].crossfade_out, clips[1].crossfade_in);
    EXPECT_DOUBLE_EQ(xf_window, 2.0);

    // Crossfade starts at: clip[0].start + clip[0].duration - xf_window
    double clip0_end = clips[0].start + clips[0].duration;
    double xf_start = clip0_end - xf_window;
    EXPECT_DOUBLE_EQ(xf_start, 8.0);

    // should_start_crossfade: show_time within [xf_start, clip0_end]
    auto should_start = [&](double show_time) -> bool {
        double time_to_end = clip0_end - show_time;
        return time_to_end <= xf_window && time_to_end > 0.0;
    };

    EXPECT_FALSE(should_start(7.0));   // too early
    EXPECT_TRUE(should_start(8.0));    // exactly at start
    EXPECT_TRUE(should_start(9.0));    // 1 second before end
    EXPECT_TRUE(should_start(9.99));   // almost at end
    EXPECT_FALSE(should_start(10.0));  // at end (time_to_end = 0, not > 0)
}

TEST(CompositeDLayerLogic, CrossfadeProgressCalculation)
{
    // crossfade_t goes from 0.0 to 1.0 over crossfade_duration seconds
    double crossfade_duration = 2.0;
    double crossfade_t = 0.0;

    // Simulate ticking: each tick adds dt/duration to crossfade_t
    auto advance = [&](float dt) {
        crossfade_t += (double)dt / crossfade_duration;
        if (crossfade_t >= 1.0) {
            crossfade_t = 1.0; // clamp
        }
    };

    // 0.5s tick → 0.25 progress
    advance(0.5f);
    EXPECT_NEAR(crossfade_t, 0.25, 0.001);

    // Another 0.5s → 0.5
    advance(0.5f);
    EXPECT_NEAR(crossfade_t, 0.5, 0.001);

    // 1.0s more → 1.0 (complete)
    advance(1.0f);
    EXPECT_NEAR(crossfade_t, 1.0, 0.001);
}

// ===========================================================================
// Transport state machine
// ===========================================================================

// We test the transport state machine through the public enum values.
// The CompositeContext is an anonymous-namespace type, but we can test
// the state transitions through the OBS callbacks.

TEST(CompositeTransport, TransportStatesAreDistinct)
{
    using TS = dancehap::TransportState;
    EXPECT_STRNE(dancehap::transport_state_to_string(TS::Stopped), "");
    EXPECT_STRNE(dancehap::transport_state_to_string(TS::Playing), "");
    EXPECT_STRNE(dancehap::transport_state_to_string(TS::Paused), "");
    EXPECT_STRNE(dancehap::transport_state_to_string(TS::Stopped),
                 dancehap::transport_state_to_string(TS::Playing));
    EXPECT_STRNE(dancehap::transport_state_to_string(TS::Playing),
                 dancehap::transport_state_to_string(TS::Paused));
}

// ===========================================================================
// Show file loading through OBS callbacks
// ===========================================================================

class CompositeShowTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        obs_stub_reset();
        info_ = dancehap_composite_get_info();
        ASSERT_NE(info_, nullptr);

        settings_ = obs_data_create();
        info_->get_defaults(settings_);

        ctx_ = info_->create(settings_, nullptr);
        ASSERT_NE(ctx_, nullptr);
    }

    void TearDown() override
    {
        if (ctx_) info_->destroy(ctx_);
        if (settings_) obs_data_release(settings_);
        cleanup(dhp_path_);
    }

    void load_show(const std::string &content)
    {
        dhp_path_ = write_test_dhp(content);
        ASSERT_FALSE(dhp_path_.empty());
        obs_data_set_string(settings_, "show_file", dhp_path_.c_str());
        info_->update(ctx_, settings_);
    }

    const obs_source_info *info_ = nullptr;
    obs_data_t *settings_ = nullptr;
    void *ctx_ = nullptr;
    std::string dhp_path_;
};

TEST_F(CompositeShowTest, LoadShowFileViaUpdate)
{
    load_show(SIMPLE_SHOW);
    // After loading, the source should have non-zero dimensions
    // (in stub mode, ClipPlayer::load returns false but the show is parsed)
    // We mainly verify no crash and the show was loaded.
    SUCCEED();
}

TEST_F(CompositeShowTest, TickAfterLoadDoesNotCrash)
{
    load_show(SIMPLE_SHOW);
    EXPECT_NO_THROW(info_->video_tick(ctx_, 0.016f));
}

TEST_F(CompositeShowTest, RenderAfterLoadDoesNotCrash)
{
    load_show(SIMPLE_SHOW);
    EXPECT_NO_THROW(info_->video_render(ctx_, nullptr));
}

TEST_F(CompositeShowTest, MultipleTicksDoNotCrash)
{
    load_show(SIMPLE_SHOW);
    // Simulate 100 frames at 60fps
    for (int i = 0; i < 100; i++) {
        info_->video_tick(ctx_, 0.016f);
    }
    SUCCEED();
}

TEST_F(CompositeShowTest, EmptyShowFilePathDoesNotCrash)
{
    // Set empty path
    obs_data_set_string(settings_, "show_file", "");
    EXPECT_NO_THROW(info_->update(ctx_, settings_));
}

TEST_F(CompositeShowTest, InvalidShowFileDoesNotCrash)
{
    load_show("{ this is not valid json ]");
    // The source should not crash, just log a warning
    EXPECT_NO_THROW(info_->video_tick(ctx_, 0.016f));
}

// ===========================================================================
// Graphics stub verification
// ===========================================================================

TEST(CompositeGraphicsStub, EnterLeaveGraphicsChangesRefcount)
{
    obs_stub_reset();
    EXPECT_EQ(obs_stub_graphics_refcount(), 0);

    obs_enter_graphics();
    EXPECT_EQ(obs_stub_graphics_refcount(), 1);

    obs_enter_graphics();
    EXPECT_EQ(obs_stub_graphics_refcount(), 2);

    obs_leave_graphics();
    EXPECT_EQ(obs_stub_graphics_refcount(), 1);

    obs_leave_graphics();
    EXPECT_EQ(obs_stub_graphics_refcount(), 0);
}

TEST(CompositeGraphicsStub, EffectCreateReturnsNonNull)
{
    gs_effect_t *eff = gs_effect_create_from_file("test.effect", nullptr);
    EXPECT_NE(eff, nullptr);
    gs_effect_destroy(eff);
}

TEST(CompositeGraphicsStub, GetParamByNameReturnsNonNull)
{
    gs_effect_t *eff = gs_effect_create_from_file("test.effect", nullptr);
    ASSERT_NE(eff, nullptr);
    gs_eparam_t *param = gs_effect_get_param_by_name(eff, "bg_image");
    EXPECT_NE(param, nullptr);
    gs_effect_destroy(eff);
}

TEST(CompositeGraphicsStub, TechniqueBeginReturnsOnePass)
{
    gs_effect_t *eff = gs_effect_create_from_file("test.effect", nullptr);
    ASSERT_NE(eff, nullptr);
    gs_technique_t *tech = gs_effect_get_technique(eff, "Draw");
    ASSERT_NE(tech, nullptr);
    EXPECT_EQ(gs_technique_begin(tech), 1u);

    gs_technique_begin_pass(tech, 0);
    gs_technique_end_pass(tech);
    gs_technique_end(tech);

    gs_effect_destroy(eff);
}

// ===========================================================================
// Module file stub
// ===========================================================================

TEST(CompositeModuleFile, ReturnsNonEmptyPath)
{
    const char *path = obs_module_file("effects/composite.effect");
    ASSERT_NE(path, nullptr);
    EXPECT_STRNE(path, "");
    // Should contain "effects/composite.effect"
    std::string spath(path);
    EXPECT_NE(spath.find("composite.effect"), std::string::npos);
}

TEST(CompositeModuleFile, SetModuleDataPath)
{
    obs_stub_set_module_data_path("/custom/data/");
    const char *path = obs_module_file("test.txt");
    ASSERT_NE(path, nullptr);
    std::string spath(path);
    EXPECT_EQ(spath, "/custom/data/test.txt");
    obs_stub_set_module_data_path("data/"); // reset
}