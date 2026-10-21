// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace fear_nx::input {

struct GyroOptions {
  double sensitivity = 1.0;
  double smoothing_ms = 16.0;
  double deadzone_dps = 0.15;
  bool invert_y = false;
  bool operator==(const GyroOptions&) const = default;
};

struct GyroDelta {
  float pitch = 0.0f;
  float yaw = 0.0f;
};

// HID supplies turns/second: X is pitch, Z is yaw (SDL's X and Y).
// F.E.A.R.'s local camera offset takes pitch/yaw in radians, as in EchoPatch.
class GyroFilter {
 public:
  void Reset() { pitch_ = yaw_ = 0.0; }

  GyroDelta Step(float x, float z, double dt, const GyroOptions& options) {
    if (!std::isfinite(x) || !std::isfinite(z) || !std::isfinite(dt) ||
        dt <= 0.0 || dt > 0.1 || !std::isfinite(options.sensitivity) ||
        !std::isfinite(options.smoothing_ms) || !std::isfinite(options.deadzone_dps)) {
      Reset();
      return {};
    }
    constexpr double tau = 2.0 * std::numbers::pi;
    const double deadzone = std::clamp(options.deadzone_dps, 0.0, 5.0) * tau / 360.0;
    const auto trim = [deadzone](double rate) {
      return std::copysign(std::max(0.0, std::abs(rate) - deadzone), rate);
    };
    const double sensitivity = std::clamp(options.sensitivity, 0.0, 10.0);
    const double smoothing = std::clamp(options.smoothing_ms, 0.0, 100.0) / 1000.0;
    const auto integrate = [dt, smoothing](double target, double& previous) {
      if (smoothing == 0.0) {
        previous = target;
        return target * dt;
      }
      const double decay = std::exp(-dt / smoothing);
      // Exact integral of the low-pass rate for this sample, so grouping the
      // same sensor samples into 25/30/60 FPS frames doesn't change the turn.
      const double delta = target * dt + (previous - target) * smoothing * (1.0 - decay);
      previous = target + (previous - target) * decay;
      return delta;
    };
    const double pitch = trim(-double(x) * tau) * (options.invert_y ? -1.0 : 1.0);
    const double yaw = trim(-double(z) * tau);
    return {float(integrate(pitch, pitch_) * sensitivity),
            float(integrate(yaw, yaw_) * sensitivity)};
  }

 private:
  double pitch_ = 0.0;
  double yaw_ = 0.0;
};

// A new aim session discards the buffered hip-fire/menu samples. Duplicate
// sample numbers are never integrated twice; a discontinuity starts fresh.
class GyroIntegrator {
 public:
  void Reset() {
    filter_.Reset();
    armed_ = false;
    last_sample_ = 0;
  }
  bool BeginFrame(bool active, uint64_t newest, double elapsed, const GyroOptions& options) {
    if (!active || !armed_ || options != options_ || !std::isfinite(elapsed) ||
        elapsed <= 0.0 || elapsed > 0.1 || newest < last_sample_ ||
        newest - last_sample_ > 16) {
      filter_.Reset();
      last_sample_ = newest;
      armed_ = active;
      options_ = options;
      return false;
    }
    return true;
  }
  GyroDelta Sample(uint64_t number, float x, float z, double seconds) {
    if (!armed_ || number <= last_sample_) return {};
    last_sample_ = number;
    return filter_.Step(x, z, seconds, options_);
  }

 private:
  GyroFilter filter_;
  GyroOptions options_;
  uint64_t last_sample_ = 0;
  bool armed_ = false;
};

}  // namespace fear_nx::input
