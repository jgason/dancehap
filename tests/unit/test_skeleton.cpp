// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// Phase 1.0 skeleton unit tests.
// These verify the plugin entry points exist, return expected values,
// and don't crash. Real source/filter tests arrive in Phase 1.1+.

#include <gtest/gtest.h>

#include "plugin.hpp"
#include "dancehap/version.h"

// --- Module lifecycle ----------------------------------------------------

TEST(DanceHAPSkeleton, ModuleLoadReturnsTrue)
{
    EXPECT_TRUE(obs_module_load());
}

TEST(DanceHAPSkeleton, ModuleUnloadDoesNotCrash)
{
    obs_module_unload();        // should not throw
    SUCCEED();
}

TEST(DanceHAPSkeleton, ModulePostLoadDoesNotCrash)
{
    obs_module_post_load();     // should not throw
    SUCCEED();
}

// --- Locale stubs --------------------------------------------------------

TEST(DanceHAPSkeleton, SetLocaleDoesNotCrash)
{
    obs_module_set_locale("en-US");
    obs_module_set_locale("fr-FR");
    obs_module_set_locale(nullptr);
    SUCCEED();
}

TEST(DanceHAPSkeleton, FreeLocaleDoesNotCrash)
{
    obs_module_free_locale();
    SUCCEED();
}

// --- Module metadata -----------------------------------------------------

TEST(DanceHAPSkeleton, ModuleNameIsDanceHAP)
{
    const char *name = obs_module_name();
    ASSERT_NE(name, nullptr);
    EXPECT_STREQ(name, "DanceHAP");
}

TEST(DanceHAPSkeleton, ModuleDescriptionIsNotNull)
{
    const char *desc = obs_module_description();
    ASSERT_NE(desc, nullptr);
    EXPECT_GT(strlen(desc), 0u);
}

TEST(DanceHAPSkeleton, ModuleVersionMatchesCMake)
{
    const unsigned int ver = obs_module_ver();
    // 0.1.0 packed as major<<24 | minor<<16 | patch
    const unsigned int expected =
        (DANCEHAP_VERSION_MAJOR << 24) |
        (DANCEHAP_VERSION_MINOR << 16) |
        DANCEHAP_VERSION_PATCH;
    EXPECT_EQ(ver, expected);
}

// --- Version header ------------------------------------------------------

TEST(DanceHAPSkeleton, VersionStringIsNotEmpty)
{
    static_assert(sizeof(DANCEHAP_VERSION_STRING) > 1,
                  "DANCEHAP_VERSION_STRING must not be empty");
    SUCCEED();
}
