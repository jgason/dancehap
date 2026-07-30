// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// bspline.cpp — B-spline cubique clamped (de Boor algorithm, ADR-012).
// Implementation ~80 lignes, sans dépendance externe.

#include "bspline.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>

namespace dancehap {

BSpline::BSpline(std::vector<BSplineKeyframe> keyframes)
    : keyframes_(std::move(keyframes))
{
    if (keyframes_.size() >= 2) {
        build_knots();
    }
}

void BSpline::build_knots()
{
    // Clamped cubic B-spline: degree p=3, n+1 control points (n = size-1).
    // Knot vector has m+1 = n+p+2 entries.
    // First p+1 knots = 0, last p+1 knots = 1 (clamped).
    // Interior knots uniformly spaced.
    const int p = 3;
    const int n = static_cast<int>(keyframes_.size()) - 1;
    const int m = n + p + 1; // last knot index
    const int num_knots = m + 1;

    // Normalize keyframe times to [0, 1] for the parameter domain.
    double t0 = keyframes_.front().time;
    double t1 = keyframes_.back().time;
    double range = t1 - t0;
    if (range <= 0.0) range = 1.0; // guard against degenerate

    knots_.assign(num_knots, 0.0);

    // Clamped: first p+1 knots = 0
    for (int i = 0; i <= p; ++i)
        knots_[i] = 0.0;

    // Interior knots: uniform
    int num_interior = n - p; // = (m+1) - 2*(p+1)
    for (int i = 1; i <= num_interior; ++i)
        knots_[p + i] = static_cast<double>(i) / (num_interior + 1);

    // Clamped: last p+1 knots = 1
    for (int i = num_knots - p - 1; i < num_knots; ++i)
        knots_[i] = 1.0;
}

size_t BSpline::find_span(double u) const
{
    // Binary search for span. u in [0,1].
    const int p = 3;
    const int n = static_cast<int>(keyframes_.size()) - 1;

    // Clamp u to [0, 1]
    if (u >= 1.0) return n; // last valid span
    if (u <= 0.0) return p;

    // Binary search: find i such that knots_[i] <= u < knots_[i+1]
    int lo = p, hi = n;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (knots_[mid] <= u)
            lo = mid;
        else
            hi = mid;
    }
    return static_cast<size_t>(lo);
}

double BSpline::de_boor(size_t span, double u) const
{
    // de Boor's algorithm: evaluate B-spline at parameter u.
    // span = knot span index, p = degree.
    const int p = 3;
    double d[4]; // p+1 control point values (degree 3)

    // Copy the relevant control point values
    for (int j = 0; j <= p; ++j) {
        int idx = static_cast<int>(span) - p + j;
        if (idx >= 0 && idx < static_cast<int>(keyframes_.size()))
            d[j] = keyframes_[idx].value;
        else
            d[j] = 0.0;
    }

    // de Boor recursion
    for (int r = 1; r <= p; ++r) {
        for (int j = p; j >= r; --j) {
            int i = static_cast<int>(span) - p + j;
            double knot_i = (i >= 0 && i < static_cast<int>(knots_.size())) ? knots_[i] : 0.0;
            double knot_ipr = (i + p - r + 1 >= 0 && i + p - r + 1 < static_cast<int>(knots_.size()))
                                 ? knots_[i + p - r + 1] : 1.0;
            double denom = knot_ipr - knot_i;
            double alpha = (denom != 0.0) ? (u - knot_i) / denom : 0.0;
            d[j] = (1.0 - alpha) * d[j - 1] + alpha * d[j];
        }
    }

    return d[p];
}

double BSpline::evaluate(double t) const
{
    if (keyframes_.empty()) return 0.0;
    if (keyframes_.size() == 1) return keyframes_[0].value;

    // Clamp t to keyframe range
    if (t <= keyframes_.front().time) return keyframes_.front().value;
    if (t >= keyframes_.back().time) return keyframes_.back().value;

    // If only 2 keyframes, linear interpolation
    if (keyframes_.size() == 2) {
        double t0 = keyframes_[0].time;
        double t1 = keyframes_[1].time;
        double range = t1 - t0;
        if (range <= 0.0) return keyframes_[0].value;
        double alpha = (t - t0) / range;
        return keyframes_[0].value * (1.0 - alpha) + keyframes_[1].value * alpha;
    }

    // For 3 keyframes with degree 3, we need at least 4 control points.
    // Pad by repeating the endpoints (common B-spline technique).
    if (keyframes_.size() < 4) {
        // Use linear interpolation between nearest keyframes as fallback
        // (B-spline with <4 points and degree 3 is underdetermined without padding)
        auto it = std::lower_bound(keyframes_.begin(), keyframes_.end(), t,
            [](const BSplineKeyframe &kf, double val) { return kf.time < val; });
        size_t idx = std::distance(keyframes_.begin(), it);
        if (idx == 0) return keyframes_[0].value;
        if (idx >= keyframes_.size()) return keyframes_.back().value;
        double t0 = keyframes_[idx - 1].time;
        double t1 = keyframes_[idx].time;
        double range = t1 - t0;
        if (range <= 0.0) return keyframes_[idx - 1].value;
        double alpha = (t - t0) / range;
        return keyframes_[idx - 1].value * (1.0 - alpha) + keyframes_[idx].value * alpha;
    }

    // Normalize t to [0, 1] parameter domain
    double t0 = keyframes_.front().time;
    double t1 = keyframes_.back().time;
    double u = (t - t0) / (t1 - t0);

    // Clamp u to [0, 1]
    if (u <= 0.0) return keyframes_.front().value;
    if (u >= 1.0) return keyframes_.back().value;

    size_t span = find_span(u);
    return de_boor(span, u);
}

} // namespace dancehap