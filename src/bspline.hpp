// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// bspline.hpp — B-spline cubique clamped pour keyframes d'opacité (ADR-012).
// Évaluation runtime légère, sans dépendance externe.

#pragma once

#include <vector>

namespace dancehap {

/// A single keyframe point on the opacity curve.
struct BSplineKeyframe {
    double time = 0.0;   ///< seconds
    double value = 0.0;  ///< 0.0–1.0
};

/// Clamped cubic B-spline evaluator (degree 3, de Boor algorithm).
/// Given N keyframes, builds a clamped knot vector so the curve passes
/// through the first and last control points. Evaluates at any time t.
class BSpline {
public:
    /// Construct from keyframes (must be sorted by time, at least 1).
    explicit BSpline(std::vector<BSplineKeyframe> keyframes);

    /// Evaluate the opacity at time t. Clamped to first/last keyframe
    /// when t is outside [first.time, last.time].
    /// If only 1 keyframe, returns its value for all t.
    /// If 2 keyframes, linear interpolation (B-spline degenerates).
    double evaluate(double t) const;

    /// Number of keyframes.
    size_t size() const { return keyframes_.size(); }

    /// True if no keyframes.
    bool empty() const { return keyframes_.empty(); }

private:
    std::vector<BSplineKeyframe> keyframes_;
    std::vector<double> knots_;  ///< clamped knot vector

    /// Build the clamped knot vector for degree 3.
    void build_knots();

    /// Find the knot span index containing parameter u.
    /// Returns i such that knots_[i] <= u < knots_[i+1].
    size_t find_span(double u) const;

    /// de Boor algorithm: evaluate B-spline at parameter u.
    double de_boor(size_t span, double u) const;
};

} // namespace dancehap