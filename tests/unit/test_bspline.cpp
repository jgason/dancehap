// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// Phase 3 Étape 2 — B-spline unit tests (TDD).

#include "bspline.hpp"
#include <gtest/gtest.h>
#include <vector>

using dancehap::BSpline;
using dancehap::BSplineKeyframe;

// --- Empty / single keyframe ---

TEST(BSplineTest, EmptyReturnsZero)
{
    BSpline spline({});
    EXPECT_DOUBLE_EQ(spline.evaluate(0.0), 0.0);
    EXPECT_DOUBLE_EQ(spline.evaluate(100.0), 0.0);
    EXPECT_TRUE(spline.empty());
}

TEST(BSplineTest, SingleKeyframeReturnsConstant)
{
    BSpline spline({{ {0.0, 0.5} }});
    EXPECT_DOUBLE_EQ(spline.evaluate(0.0), 0.5);
    EXPECT_DOUBLE_EQ(spline.evaluate(10.0), 0.5);
    EXPECT_EQ(spline.size(), 1u);
}

// --- Two keyframes: linear interpolation ---

TEST(BSplineTest, TwoKeyframesLinearInterpolation)
{
    BSpline spline({{ {0.0, 0.0}, {10.0, 1.0} }});
    EXPECT_NEAR(spline.evaluate(0.0), 0.0, 1e-6);
    EXPECT_NEAR(spline.evaluate(5.0), 0.5, 1e-6);
    EXPECT_NEAR(spline.evaluate(10.0), 1.0, 1e-6);
}

TEST(BSplineTest, TwoKeyframesClampedOutOfRange)
{
    BSpline spline({{ {5.0, 0.2}, {15.0, 0.8} }});
    // Before first keyframe
    EXPECT_NEAR(spline.evaluate(0.0), 0.2, 1e-6);
    EXPECT_NEAR(spline.evaluate(3.0), 0.2, 1e-6);
    // After last keyframe
    EXPECT_NEAR(spline.evaluate(20.0), 0.8, 1e-6);
}

// --- Three keyframes: falls back to linear (B-spline underdetermined) ---

TEST(BSplineTest, ThreeKeyframesInterpolation)
{
    BSpline spline({{ {0.0, 0.0}, {5.0, 1.0}, {10.0, 0.0} }});
    // At keyframe times, should return keyframe values
    EXPECT_NEAR(spline.evaluate(0.0), 0.0, 1e-6);
    EXPECT_NEAR(spline.evaluate(10.0), 0.0, 1e-6);
    // Mid should be between 0 and 1
    double mid = spline.evaluate(5.0);
    EXPECT_GE(mid, 0.0);
    EXPECT_LE(mid, 1.0);
}

// --- Four+ keyframes: B-spline cubic ---

TEST(BSplineTest, FourKeyframesPassesNearEndpoints)
{
    std::vector<BSplineKeyframe> kfs = {
        {0.0, 0.0}, {3.0, 0.3}, {6.0, 0.7}, {10.0, 1.0}
    };
    BSpline spline(kfs);
    // Clamped B-spline passes through endpoints
    EXPECT_NEAR(spline.evaluate(0.0), 0.0, 1e-6);
    EXPECT_NEAR(spline.evaluate(10.0), 1.0, 1e-6);
    // Interior values should be smooth between keyframes
    double v = spline.evaluate(5.0);
    EXPECT_GT(v, 0.0);
    EXPECT_LT(v, 1.0);
}

TEST(BSplineTest, FiveKeyframesSmoothCurve)
{
    std::vector<BSplineKeyframe> kfs = {
        {0.0, 1.0}, {2.0, 0.0}, {4.0, 1.0}, {6.0, 0.0}, {8.0, 1.0}
    };
    BSpline spline(kfs);
    // Endpoints
    EXPECT_NEAR(spline.evaluate(0.0), 1.0, 1e-6);
    EXPECT_NEAR(spline.evaluate(8.0), 1.0, 1e-6);
    // Should oscillate smoothly
    double v2 = spline.evaluate(2.0);
    double v4 = spline.evaluate(4.0);
    double v6 = spline.evaluate(6.0);
    EXPECT_GE(v2, -0.1); // B-spline can overshoot slightly
    EXPECT_LE(v2, 0.5);
    EXPECT_GE(v4, 0.5);
    EXPECT_LE(v6, 0.5);
}

// --- Edge cases ---

TEST(BSplineTest, AllSameValue)
{
    BSpline spline({{ {0.0, 0.5}, {5.0, 0.5}, {10.0, 0.5}, {15.0, 0.5} }});
    EXPECT_NEAR(spline.evaluate(7.0), 0.5, 1e-6);
}

TEST(BSplineTest, DescendingValues)
{
    BSpline spline({{ {0.0, 1.0}, {3.0, 0.5}, {6.0, 0.0}, {10.0, 0.0} }});
    EXPECT_NEAR(spline.evaluate(0.0), 1.0, 1e-6);
    EXPECT_NEAR(spline.evaluate(10.0), 0.0, 1e-6);
    double mid = spline.evaluate(5.0);
    EXPECT_GE(mid, -0.1);
    EXPECT_LE(mid, 0.5);
}

TEST(BSplineTest, ClampedToFirstAndLast)
{
    BSpline spline({{ {10.0, 0.3}, {20.0, 0.6}, {30.0, 0.9}, {40.0, 1.0} }});
    // Before first
    EXPECT_NEAR(spline.evaluate(0.0), 0.3, 1e-6);
    EXPECT_NEAR(spline.evaluate(5.0), 0.3, 1e-6);
    // After last
    EXPECT_NEAR(spline.evaluate(50.0), 1.0, 1e-6);
}