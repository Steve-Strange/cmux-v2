// Copyright (c) 2026 Alasdair Monk
// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: MIT
//
// Adapted from Bonsplit; see third_party/bonsplit/LICENSE.

#ifndef CHROME_BROWSER_CMUX_TERM_CMUX_EASING_H_
#define CHROME_BROWSER_CMUX_TERM_CMUX_EASING_H_

namespace cmux {

// bonsplit SplitAnimator.swift pane/divider curve:
// progress == 1 ? 1 : 1 - pow(2, -10 * progress).
// `t` is normalized to [0, 1]; values at or past the end clamp to exactly 1.
double EaseOutExpo(double t);

// Closed-form Apple spring(duration:D, bounce:b) using the WWDC23 model:
// omega_n = 2*pi/D, zeta = 1 - b, and underdamped response
// x(t) = 1 - exp(-zeta*omega_n*t) *
//        (cos(omega_d*t) + (zeta*omega_n/omega_d) * sin(omega_d*t)).
//
// Callers pass `t_normalized` over the run duration, not the perceived SwiftUI
// duration D. The run duration is 1.35 * D so the spring visually settles
// around D and clamps to exactly 1 at the end of the run.
double AppleSpring(double t_normalized, double bounce);

// Scales a spring's perceived duration by travel distance. `reference_px` is
// the short-hop threshold: travel at or below it uses `base_ms`, then longer
// glides interpolate up to `max_ms` over the next `reference_px`.
double SpringDurationForDistance(double px,
                                 double reference_px,
                                 double base_ms,
                                 double max_ms);

}  // namespace cmux

#endif  // CHROME_BROWSER_CMUX_TERM_CMUX_EASING_H_
