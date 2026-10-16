// Copyright (c) 2026 Alasdair Monk
// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: MIT
//
// Adapted from Bonsplit; see third_party/bonsplit/LICENSE.

#include "chrome/browser/cmux_term/cmux_easing.h"

#include <cmath>

namespace cmux {

namespace {

constexpr double kPi = 3.14159265358979323846264338327950288;
constexpr double kSpringRunDurationMultiplier = 1.35;

}  // namespace

double EaseOutExpo(double t) {
  if (t <= 0.0) {
    return 0.0;
  }
  if (t >= 1.0) {
    return 1.0;
  }
  return 1.0 - std::pow(2.0, -10.0 * t);
}

double AppleSpring(double t_normalized, double bounce) {
  if (t_normalized <= 0.0) {
    return 0.0;
  }
  if (t_normalized >= 1.0) {
    return 1.0;
  }

  double b = bounce;
  if (b < 0.0) {
    b = 0.0;
  } else if (b >= 1.0) {
    b = 0.999999;
  }

  const double zeta = 1.0 - b;
  const double omega_n_t =
      2.0 * kPi * kSpringRunDurationMultiplier * t_normalized;

  // bounce <= 0 collapses to the critically-damped (zeta == 1) limit of the
  // underdamped solution below: x(t) = 1 - e^(-w*t) * (1 + w*t).
  if (zeta >= 1.0) {
    return 1.0 - std::exp(-omega_n_t) * (1.0 + omega_n_t);
  }

  const double damped = std::sqrt(1.0 - zeta * zeta);
  const double omega_d_t = omega_n_t * damped;
  return 1.0 -
         std::exp(-zeta * omega_n_t) *
             (std::cos(omega_d_t) + (zeta / damped) * std::sin(omega_d_t));
}

double SpringDurationForDistance(double px,
                                 double reference_px,
                                 double base_ms,
                                 double max_ms) {
  if (max_ms <= base_ms) {
    return base_ms;
  }
  if (px <= 0.0 || reference_px <= 0.0) {
    return base_ms;
  }
  double ratio = (px - reference_px) / reference_px;
  if (ratio < 0.0) {
    ratio = 0.0;
  } else if (ratio > 1.0) {
    ratio = 1.0;
  }
  return base_ms + (max_ms - base_ms) * ratio;
}

}  // namespace cmux
