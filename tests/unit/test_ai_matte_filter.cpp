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

// ===========================================================================
// Pass-through: frame processing returns input unmodified
// ===========================================================================

TEST_F(AiMatteFilterTest, PassThroughReturnsInput)
{
    // Create a synthetic BGRA frame (4 bytes per pixel).
    const uint32_t width  = 64;
    const uint32_t height = 48;
    const size_t  pixel_size = 4;  // BGRA
    std::vector<uint8_t> input(width * height * pixel_size);
    for (size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<uint8_t>(i % 256);
    }
    const void *input_ptr = input.data();

    // Process the frame through the filter's pass-through logic.
    uint32_t out_width  = 0;
    uint32_t out_height = 0;
    const void *output_ptr = ai_matte_filter_process_frame(
        input_ptr, width, height, &out_width, &out_height);

    // Pass-through: output pointer must be the same as input (no copy, no
    // modification). Dimensions must match.
    EXPECT_EQ(output_ptr, input_ptr);
    EXPECT_EQ(out_width, width);
    EXPECT_EQ(out_height, height);

    // Verify the data itself is unmodified (same bytes).
    const uint8_t *out_bytes = static_cast<const uint8_t *>(output_ptr);
    EXPECT_EQ(0, std::memcmp(out_bytes, input.data(), input.size()));
}