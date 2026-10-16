// Copyright (c) 2026 Alasdair Monk
// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: MIT
//
// Adapted from Bonsplit; see third_party/bonsplit/LICENSE.
//
// Host-compilable unit test for pure cmux easing functions (no Chromium,
// no gtest). Build + run:
//
//   c++ -std=c++17 -I overlay \
//     overlay/chrome/browser/cmux_term/cmux_easing.cc \
//     overlay/chrome/browser/cmux_term/cmux_easing_test.cc \
//     -o /tmp/easing && /tmp/easing

#include "chrome/browser/cmux_term/cmux_easing.h"

#include <cmath>
#include <cstdio>

using namespace cmux;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("  FAIL: %s\n", what);
  }
}

bool Near(double a, double b, double tolerance = 1e-9) {
  return std::fabs(a - b) <= tolerance;
}

void TestEaseOutExpo() {
  Check(EaseOutExpo(0.0) == 0.0, "expo starts at 0");
  Check(Near(EaseOutExpo(0.5), 0.96875), "expo midpoint golden");
  Check(EaseOutExpo(1.0) == 1.0, "expo ends exactly at 1");
  Check(EaseOutExpo(2.0) == 1.0, "expo clamps past end");
}

void TestAppleSpringBounce() {
  constexpr double kBounce = 0.15;
  double previous = AppleSpring(0.0, kBounce);
  double max_value = previous;
  int upward_crossings = 0;
  bool was_above_one = previous > 1.0;
  bool reached_one = false;

  Check(previous == 0.0, "spring starts at 0");
  for (int i = 1; i <= 1000; ++i) {
    const double value = AppleSpring(i / 1000.0, kBounce);
    max_value = std::fmax(max_value, value);
    Check(value >= -1e-9, "spring stays non-negative");
    Check(value <= 1.01, "spring overshoot stays under 1%");
    if (!reached_one) {
      Check(value + 1e-9 >= previous, "spring rises to first crossing");
    }
    const bool above_one = value > 1.0;
    if (!was_above_one && above_one) {
      ++upward_crossings;
      reached_one = true;
    }
    was_above_one = above_one;
    previous = value;
  }

  Check(upward_crossings == 1, "spring has a single overshoot");
  Check(max_value > 1.0 && max_value <= 1.01, "spring overshoots slightly");
  Check(AppleSpring(1.0, kBounce) == 1.0, "spring ends clamped at 1");
}

void TestAppleSpringNoBounce() {
  double previous = AppleSpring(0.0, 0.0);
  for (int i = 1; i <= 1000; ++i) {
    const double value = AppleSpring(i / 1000.0, 0.0);
    Check(value + 1e-9 >= previous, "no-bounce spring is monotonic");
    Check(value <= 1.0, "no-bounce spring never overshoots");
    previous = value;
  }
  Check(AppleSpring(1.0, 0.0) == 1.0, "no-bounce spring ends at 1");
}

void TestSpringDurationForDistance() {
  Check(SpringDurationForDistance(0.0, 900.0, 150.0, 220.0) == 150.0,
        "distance duration starts at base");
  Check(SpringDurationForDistance(450.0, 900.0, 150.0, 220.0) == 150.0,
        "short distance duration stays at base");
  Check(SpringDurationForDistance(900.0, 900.0, 150.0, 220.0) == 150.0,
        "reference distance duration stays at base");
  Check(SpringDurationForDistance(1350.0, 900.0, 150.0, 220.0) == 185.0,
        "long distance duration interpolates");
  Check(SpringDurationForDistance(1800.0, 900.0, 150.0, 220.0) == 220.0,
        "distance duration caps at max");
  Check(SpringDurationForDistance(300.0, 0.0, 150.0, 220.0) == 150.0,
        "distance duration handles invalid reference");
  Check(SpringDurationForDistance(300.0, 900.0, 220.0, 150.0) == 220.0,
        "distance duration handles inverted range");
}

}  // namespace

int main() {
  std::printf("cmux_easing_test\n");
  TestEaseOutExpo();
  TestAppleSpringBounce();
  TestAppleSpringNoBounce();
  TestSpringDurationForDistance();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
