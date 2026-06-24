// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// Unit tests for ai_matte_filter (Phase 2.0 skeleton).
//
// These tests exercise the filter's obs_source_info callbacks through the
// stub OBS API (obs_compat.hpp), verifying:
//   • Filter identity (id, name, type, output flags)
//   • Lifecycle (create/destroy without crash)
//   • Registration (obs_module_load calls obs_register_source for the filter)
//   • Pass-through frame processing (input returned unmodified)
//
// No real OBS instance is required — all calls go through the stub layer.

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "ai_matte_filter.hpp"
#include "obs_compat.hpp"
#include "plugin.hpp"

// ===========================================================================
// Filter identity
// ===========================================================================

class AiMatteFilterTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        obs_stub_reset();
    }
};

TEST_F(AiMatteFilterTest, CanInstantiate)
{
    const obs_source_info *info = ai_matte_filter_get_info();
    ASSERT_NE(info, nullptr);
    ASSERT_NE(info->id, nullptr);
    EXPECT_STRNE(info->id, "");
    EXPECT_STREQ(info->id, AI_MATTE_FILTER_ID);
    EXPECT_EQ(info->type, OBS_SOURCE_TYPE_FILTER);
}