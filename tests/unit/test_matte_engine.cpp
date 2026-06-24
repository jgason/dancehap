// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// tests/unit/test_matte_engine.cpp
//
// Phase 2.1: stub behavior tests for MatteEngine.

#include "matte_engine.hpp"

#include <gtest/gtest.h>

namespace dancehap {
namespace {

// ---------------------------------------------------------------------------
// CanInstantiate
// ---------------------------------------------------------------------------

TEST(MatteEngineTest, CanInstantiate)
{
    MatteEngine engine;
    // Engine exists and is not ready (no model loaded).
    EXPECT_FALSE(engine.isReady());
}

// ---------------------------------------------------------------------------
// LoadModelReturnsFalseInStub
// ---------------------------------------------------------------------------

TEST(MatteEngineTest, LoadModelReturnsFalseInStub)
{
    MatteEngine engine;
    // In stub mode, loadModel always returns false.
    EXPECT_FALSE(engine.loadModel("any_model.onnx"));
    EXPECT_FALSE(engine.isReady());
}

// ---------------------------------------------------------------------------
// InferReturnsEmptyInStub
// ---------------------------------------------------------------------------

TEST(MatteEngineTest, InferReturnsEmptyInStub)
{
    MatteEngine engine;
    ImageFrame input{256, 256, nullptr};
    MatteMask mask = engine.infer(input);
    // In stub mode, infer returns an empty mask.
    EXPECT_EQ(mask.width, 0);
    EXPECT_EQ(mask.height, 0);
    EXPECT_TRUE(mask.alpha.empty());
}

} // namespace
} // namespace dancehap