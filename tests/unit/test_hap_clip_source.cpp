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

TEST_F(HapClipSourceTest, OutputFlagsIncludeCustomDrawAndNoDuplicate)
{
    const obs_source_info *info = hap_clip_source_get_info();
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->output_flags & OBS_SOURCE_CUSTOM_DRAW);
    EXPECT_TRUE(info->output_flags & OBS_SOURCE_DO_NOT_DUPLICATE);
}

TEST_F(HapClipSourceTest, GetNameReturnsDisplayName)
{
    const obs_source_info *info = hap_clip_source_get_info();
    ASSERT_NE(info, nullptr);
    ASSERT_NE(info->get_name, nullptr);
    const char *name = info->get_name(nullptr);
    ASSERT_NE(name, nullptr);
    EXPECT_STREQ(name, HAP_CLIP_SOURCE_NAME);
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
