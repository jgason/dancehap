// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// tests/unit/test_matte_engine.cpp
//
// Phase 2.1/2.2: MatteEngine stub behavior + preprocess/postprocess helpers.

#include "matte_engine.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace dancehap {
namespace {

// ---------------------------------------------------------------------------
// Stub behavior (Phase 2.1)
// ---------------------------------------------------------------------------

TEST(MatteEngineTest, CanInstantiate)
{
    MatteEngine engine;
    EXPECT_FALSE(engine.isReady());
}

TEST(MatteEngineTest, LoadModelReturnsFalseInStub)
{
    MatteEngine engine;
    EXPECT_FALSE(engine.loadModel("any_model.onnx"));
    EXPECT_FALSE(engine.isReady());
}

TEST(MatteEngineTest, InferReturnsEmptyInStub)
{
    MatteEngine engine;
    ImageFrame input{256, 256, nullptr};
    MatteMask mask = engine.infer(input);
    EXPECT_EQ(mask.width, 0);
    EXPECT_EQ(mask.height, 0);
    EXPECT_TRUE(mask.alpha.empty());
}

// ---------------------------------------------------------------------------
// Preprocess helper (Phase 2.2)
// ---------------------------------------------------------------------------

TEST(MatteEngineTest, PreprocessConvertsRgbaToRgbNormalized)
{
    // 1x1 white RGBA pixel → R=G=B=1.0, normalized = (1.0 - mean) / std.
    uint8_t white[4] = {255, 255, 255, 255};
    ImageFrame input{1, 1, white};

    auto out = preprocess_rgba_to_rgb_normalized(input, 1, 1);
    ASSERT_EQ(out.size(), static_cast<size_t>(3));

    // CHW layout: R plane, G plane, B plane (1 sample each).
    float expected_r = (1.0f - 0.485f) / 0.229f;
    float expected_g = (1.0f - 0.456f) / 0.224f;
    float expected_b = (1.0f - 0.406f) / 0.225f;

    EXPECT_NEAR(out[0], expected_r, 0.01f);
    EXPECT_NEAR(out[1], expected_g, 0.01f);
    EXPECT_NEAR(out[2], expected_b, 0.01f);
}

TEST(MatteEngineTest, PreprocessHandlesZeroPixel)
{
    // 1x1 black RGBA pixel → R=G=B=0, normalized = (0 - mean) / std.
    uint8_t black[4] = {0, 0, 0, 255};
    ImageFrame input{1, 1, black};

    auto out = preprocess_rgba_to_rgb_normalized(input, 1, 1);
    ASSERT_EQ(out.size(), static_cast<size_t>(3));

    EXPECT_NEAR(out[0], -0.485f / 0.229f, 0.01f);
    EXPECT_NEAR(out[1], -0.456f / 0.224f, 0.01f);
    EXPECT_NEAR(out[2], -0.406f / 0.225f, 0.01f);
}

TEST(MatteEngineTest, PreprocessResizesToTargetDims)
{
    // 4x4 red RGBA → resize to 2x2. Output should have 3*2*2 = 12 elements.
    std::vector<uint8_t> red(4 * 4 * 4, 0);
    for (size_t i = 0; i < 16; ++i) {
        red[i * 4 + 0] = 255;  // R
    }
    ImageFrame input{4, 4, red.data()};

    auto out = preprocess_rgba_to_rgb_normalized(input, 2, 2);
    EXPECT_EQ(out.size(), static_cast<size_t>(3 * 2 * 2));
}

// ---------------------------------------------------------------------------
// Postprocess helper (Phase 2.2)
// ---------------------------------------------------------------------------

TEST(MatteEngineTest, PostprocessPhaToMask)
{
    // 1x1 pha = 0.75 → mask alpha = 0.75.
    float pha[1] = {0.75f};
    MatteMask mask = postprocess_pha_to_mask(pha, 1, 1, 1, 1);

    EXPECT_EQ(mask.width, 1);
    EXPECT_EQ(mask.height, 1);
    ASSERT_EQ(mask.alpha.size(), static_cast<size_t>(1));
    EXPECT_NEAR(mask.alpha[0], 0.75f, 0.01f);
}

TEST(MatteEngineTest, PostprocessClampsToValidRange)
{
    // Values outside [0,1] should be clamped.
    float pha[2] = {-0.5f, 1.5f};
    MatteMask mask = postprocess_pha_to_mask(pha, 2, 1, 2, 1);

    ASSERT_EQ(mask.alpha.size(), static_cast<size_t>(2));
    EXPECT_NEAR(mask.alpha[0], 0.0f, 0.001f);
    EXPECT_NEAR(mask.alpha[1], 1.0f, 0.001f);
}

TEST(MatteEngineTest, PostprocessResizesMask)
{
    // 1x1 pha = 0.5 → resize to 4x4 mask, all values should be ~0.5.
    float pha[1] = {0.5f};
    MatteMask mask = postprocess_pha_to_mask(pha, 1, 1, 4, 4);

    EXPECT_EQ(mask.width, 4);
    EXPECT_EQ(mask.height, 4);
    ASSERT_EQ(mask.alpha.size(), static_cast<size_t>(16));
    for (float v : mask.alpha) {
        EXPECT_NEAR(v, 0.5f, 0.01f);
    }
}

} // namespace
} // namespace dancehap