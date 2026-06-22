// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// Unit tests for hap_clip_source (Phase 1.1).
//
// These tests exercise the source's obs_source_info callbacks through the
// stub OBS API (obs_compat.hpp), verifying:
//   • Source identity (id, name, type, output flags)
//   • Default settings (path, loop, autoplay)
//   • Property view (file path, loop, autoplay)
//   • Lifecycle (create/destroy without crash)
//   • Registration (obs_module_load calls obs_register_source)
//
// No real OBS instance is required — all calls go through the stub layer.

#include <gtest/gtest.h>

#include <string>

#include "hap_clip_source.hpp"
#include "obs_compat.hpp"
#include "plugin.hpp"

// ---------------------------------------------------------------------------
// Helper: find a property by name in a stub obs_properties
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

// ===========================================================================
// Source identity
// ===========================================================================

class HapClipSourceTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        obs_stub_reset();
    }
};

TEST_F(HapClipSourceTest, SourceIdIsCorrect)
{
    const obs_source_info *info = hap_clip_source_get_info();
    ASSERT_NE(info, nullptr);
    ASSERT_NE(info->id, nullptr);
    EXPECT_STREQ(info->id, HAP_CLIP_SOURCE_ID);
}

TEST_F(HapClipSourceTest, SourceTypeIsInput)
{
    const obs_source_info *info = hap_clip_source_get_info();
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->type, OBS_SOURCE_TYPE_INPUT);
}

TEST_F(HapClipSourceTest, OutputFlagsIncludeVideoAndAudio)
{
    const obs_source_info *info = hap_clip_source_get_info();
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->output_flags & OBS_SOURCE_VIDEO);
    EXPECT_TRUE(info->output_flags & OBS_SOURCE_AUDIO);
}

TEST_F(HapClipSourceTest, OutputFlagsIncludeNoDuplicate)
{
    const obs_source_info *info = hap_clip_source_get_info();
    ASSERT_NE(info, nullptr);
    // CUSTOM_DRAW is intentionally NOT set: we want OBS to wrap our
    // video_render in the default-effect Draw technique. See the comment
    // in hap_clip_source.cpp for the full rationale.
    EXPECT_FALSE(info->output_flags & OBS_SOURCE_CUSTOM_DRAW);
    EXPECT_TRUE(info->output_flags & OBS_SOURCE_DO_NOT_DUPLICATE);
}

TEST_F(HapClipSourceTest, GetNameReturnsDisplayName)
{
    const obs_source_info *info = hap_clip_source_get_info();
    ASSERT_NE(info, nullptr);
    ASSERT_NE(info->get_name, nullptr);
    const char *name = info->get_name(nullptr);
    ASSERT_NE(name, nullptr);
    // The name embeds the build version so the operator can confirm the
    // deployed DLL at a glance in OBS. It must start with the base display
    // name and include a version suffix "vX.Y.Z".
    std::string s(name);
    EXPECT_NE(s.find(HAP_CLIP_SOURCE_NAME), std::string::npos)
        << "name='" << s << "' missing base name '" << HAP_CLIP_SOURCE_NAME << "'";
    EXPECT_NE(s.find("v"), std::string::npos)
        << "name='" << s << "' missing version suffix";
}

// ===========================================================================
// Default settings
// ===========================================================================

TEST_F(HapClipSourceTest, DefaultsPathIsEmptyLoopTrueAutoplayFalse)
{
    const obs_source_info *info = hap_clip_source_get_info();
    ASSERT_NE(info, nullptr);
    ASSERT_NE(info->get_defaults, nullptr);

    obs_data_t *settings = obs_data_create();
    info->get_defaults(settings);

    EXPECT_STREQ(obs_data_get_string(settings, "path"), "");
    EXPECT_TRUE(obs_data_get_bool(settings, "loop"));
    EXPECT_FALSE(obs_data_get_bool(settings, "autoplay"));

    obs_data_release(settings);
}

// ===========================================================================
// Properties
// ===========================================================================

TEST_F(HapClipSourceTest, PropertiesHasPathLoopAutoplay)
{
    const obs_source_info *info = hap_clip_source_get_info();
    ASSERT_NE(info, nullptr);
    ASSERT_NE(info->get_properties, nullptr);

    obs_properties_t *props = info->get_properties(nullptr);
    ASSERT_NE(props, nullptr);

    // Should have exactly 3 properties: path, loop, autoplay.
    EXPECT_EQ(props->props.size(), 3u);

    ASSERT_NE(find_property(props, "path"), nullptr);
    EXPECT_EQ(find_property(props, "path")->kind, "path");

    ASSERT_NE(find_property(props, "loop"), nullptr);
    EXPECT_EQ(find_property(props, "loop")->kind, "bool");

    ASSERT_NE(find_property(props, "autoplay"), nullptr);
    EXPECT_EQ(find_property(props, "autoplay")->kind, "bool");

    obs_properties_destroy(props);
}

// Phase 1.5.a Étape 1: file dialog filter must restrict to .mov HAP clips.
TEST_F(HapClipSourceTest, GetPropertiesReturnsHapMovFilter)
{
    const obs_source_info *info = hap_clip_source_get_info();
    ASSERT_NE(info, nullptr);
    ASSERT_NE(info->get_properties, nullptr);

    obs_properties_t *props = info->get_properties(nullptr);
    ASSERT_NE(props, nullptr);

    const obs_properties::property *path_prop = find_property(props, "path");
    ASSERT_NE(path_prop, nullptr);
    EXPECT_EQ(path_prop->kind, "path");

    // The filter must include *.mov (the canonical HAP container).
    EXPECT_NE(path_prop->filter.find("*.mov"), std::string::npos)
        << "path filter should include *.mov; got: '" << path_prop->filter << "'";

    // The filter must NOT include *.mp4 (HAP .mp4 is rare; restricting to
    // .mov avoids confusion with regular H.264 .mp4 files).
    EXPECT_EQ(path_prop->filter.find("*.mp4"), std::string::npos)
        << "path filter should NOT include *.mp4; got: '" << path_prop->filter << "'";

    obs_properties_destroy(props);
}

// ===========================================================================
// Lifecycle
// ===========================================================================

TEST_F(HapClipSourceTest, CreateReturnsNonNullAndDestroyDoesNotCrash)
{
    const obs_source_info *info = hap_clip_source_get_info();
    ASSERT_NE(info, nullptr);
    ASSERT_NE(info->create, nullptr);
    ASSERT_NE(info->destroy, nullptr);

    obs_data_t *settings = obs_data_create();
    info->get_defaults(settings);

    void *data = info->create(settings, nullptr);
    EXPECT_NE(data, nullptr);

    info->destroy(data);
    SUCCEED(); // no crash

    obs_data_release(settings);
}

TEST_F(HapClipSourceTest, UpdateChangesSettingsWithoutCrash)
{
    const obs_source_info *info = hap_clip_source_get_info();
    ASSERT_NE(info, nullptr);
    ASSERT_NE(info->update, nullptr);

    obs_data_t *settings = obs_data_create();
    info->get_defaults(settings);

    void *data = info->create(settings, nullptr);
    ASSERT_NE(data, nullptr);

    // Change loop to false, autoplay to true
    settings->bools["loop"] = false;
    settings->bools["autoplay"] = true;
    info->update(data, settings);

    // No crash is the main assertion. The context struct is internal,
    // but we can verify the update path by checking no assertion fires.
    info->destroy(data);
    SUCCEED();

    obs_data_release(settings);
}

TEST_F(HapClipSourceTest, ActivateDeactivateDoNotCrash)
{
    const obs_source_info *info = hap_clip_source_get_info();
    ASSERT_NE(info, nullptr);

    obs_data_t *settings = obs_data_create();
    void *data = info->create(settings, nullptr);
    ASSERT_NE(data, nullptr);

    ASSERT_NE(info->activate, nullptr);
    ASSERT_NE(info->deactivate, nullptr);
    info->activate(data);
    info->deactivate(data);
    info->activate(data);
    info->deactivate(data);

    info->destroy(data);
    obs_data_release(settings);
    SUCCEED();
}

// ===========================================================================
// Video tick / render (safe no-ops)
// ===========================================================================

TEST_F(HapClipSourceTest, VideoTickAndRenderDoNotCrash)
{
    const obs_source_info *info = hap_clip_source_get_info();
    ASSERT_NE(info, nullptr);
    ASSERT_NE(info->video_tick, nullptr);
    ASSERT_NE(info->video_render, nullptr);

    obs_data_t *settings = obs_data_create();
    void *data = info->create(settings, nullptr);
    ASSERT_NE(data, nullptr);

    info->video_tick(data, 0.016f);  // ~60fps
    info->video_render(data, nullptr);

    info->destroy(data);
    obs_data_release(settings);
    SUCCEED();
}

// ===========================================================================
// Registration via obs_module_load
// ===========================================================================

TEST_F(HapClipSourceTest, ModuleLoadRegistersSource)
{
    // Before load: no registrations.
    EXPECT_EQ(obs_stub_registration_count(), 0);
    EXPECT_EQ(obs_stub_last_registered_source(), nullptr);

    // Trigger module load.
    EXPECT_TRUE(obs_module_load());

    // After load: exactly 1 registration, and it's our source.
    EXPECT_EQ(obs_stub_registration_count(), 1);
    const obs_source_info *registered = obs_stub_last_registered_source();
    ASSERT_NE(registered, nullptr);
    EXPECT_STREQ(registered->id, HAP_CLIP_SOURCE_ID);
}

TEST_F(HapClipSourceTest, RegisterHapClipSourceCallsObsRegisterSource)
{
    EXPECT_EQ(obs_stub_registration_count(), 0);

    register_hap_clip_source();

    EXPECT_EQ(obs_stub_registration_count(), 1);
    const obs_source_info *info = obs_stub_last_registered_source();
    ASSERT_NE(info, nullptr);
    EXPECT_STREQ(info->id, HAP_CLIP_SOURCE_ID);
    EXPECT_EQ(info->type, OBS_SOURCE_TYPE_INPUT);
}

// ===========================================================================
// Phase 1.4: ClipPlayer integration tests
// ===========================================================================

TEST_F(HapClipSourceTest, VideoTickWithLoadedClipDoesNotCrash)
{
    const obs_source_info *info = hap_clip_source_get_info();
    ASSERT_NE(info, nullptr);

    // Create with a valid clip path.
    obs_data_t *settings = obs_data_create();
    info->get_defaults(settings);
    settings->strings["path"] = DANCEHAP_TEST_ASSET;

    void *data = info->create(settings, nullptr);
    ASSERT_NE(data, nullptr);

    // Tick + render should work with the ClipPlayer loaded.
    for (int i = 0; i < 30; ++i) {
        info->video_tick(data, 0.016f);  // ~60 Hz
    }
    info->video_render(data, nullptr);

    info->destroy(data);
    obs_data_release(settings);
    SUCCEED();
}

TEST_F(HapClipSourceTest, PathChangeReloadsPlayerWithoutCrash)
{
    const obs_source_info *info = hap_clip_source_get_info();
    ASSERT_NE(info, nullptr);

    // Create with empty path (default).
    obs_data_t *settings = obs_data_create();
    info->get_defaults(settings);
    void *data = info->create(settings, nullptr);
    ASSERT_NE(data, nullptr);

    // Tick with no clip — safe no-op.
    info->video_tick(data, 0.016f);

    // Update with a valid clip path.
    settings->strings["path"] = DANCEHAP_TEST_ASSET;
    info->update(data, settings);

    // Now tick should exercise the ClipPlayer.
    info->video_tick(data, 0.016f);
    info->video_render(data, nullptr);

    info->destroy(data);
    obs_data_release(settings);
    SUCCEED();
}

TEST_F(HapClipSourceTest, DimensionsReportedWhenClipLoaded)
{
    const obs_source_info *info = hap_clip_source_get_info();
    ASSERT_NE(info, nullptr);
    ASSERT_NE(info->get_width, nullptr);
    ASSERT_NE(info->get_height, nullptr);

    // No clip → dimensions 0.
    obs_data_t *settings = obs_data_create();
    info->get_defaults(settings);
    void *data = info->create(settings, nullptr);
    EXPECT_EQ(info->get_width(data), 0u);
    EXPECT_EQ(info->get_height(data), 0u);
    info->destroy(data);

    // With clip → 256x256 (stub metadata for sample_hapa_5s.mov).
    settings->strings["path"] = DANCEHAP_TEST_ASSET;
    data = info->create(settings, nullptr);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(info->get_width(data), 256u);
    EXPECT_EQ(info->get_height(data), 256u);

    info->destroy(data);
    obs_data_release(settings);
}
