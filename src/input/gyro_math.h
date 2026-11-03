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
  bool invert_x = false;
  bool operator==(const GyroOptions&) const = default;
};

struct GyroDelta {
  float pitch = 0.0f;
  float yaw = 0.0f;
};

// HID supplies turns/second. Pitch is local X; yaw is projected onto gravity.
// F.E.A.R.'s local camera offset takes pitch/yaw in radians, as in EchoPatch.
class GyroProjection {
 public:
  void Reset() { initialized_ = false; }

  float Yaw(float x, float y, float z, float ax, float ay, float az, double dt) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
        !std::isfinite(dt) || dt <= 0 || dt > 0.1) {
      Reset();
      return 0;
    }
    const double length = std::sqrt(double(ax) * ax + double(ay) * ay + double(az) * az);
    // Ignore strong linear acceleration/free fall. Use the last valid gravity
    // direction until a stationary-sized reading returns.
    if (std::isfinite(length) && length > 0.5 && length < 1.5) {
      const double k = initialized_ ? -std::expm1(-dt / 0.1) : 1.0;
      const double a[3] = {ax / length, ay / length, az / length};
      for (int i = 0; i < 3; ++i) gravity_[i] += (a[i] - gravity_[i]) * k;
      const double norm = std::sqrt(gravity_[0] * gravity_[0] +
                                    gravity_[1] * gravity_[1] + gravity_[2] * gravity_[2]);
      if (norm > 1e-6) {
        for (auto& component : gravity_) component /= norm;
        initialized_ = true;
      } else {
        Reset();
      }
    }
    // libnx acceleration points down. Negate it to retain the original flat
    // controller yaw convention while also supporting an upright handheld.
    return initialized_ ? float(-(x * gravity_[0] + y * gravity_[1] + z * gravity_[2])) : 0;
  }

 private:
  double gravity_[3] = {};
  bool initialized_ = false;
};

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
    const double yaw = trim(-double(z) * tau) * (options.invert_x ? -1.0 : 1.0);
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
    projection_.Reset();
    armed_ = false;
    last_sample_ = 0;
  }
  bool BeginFrame(bool active, uint64_t newest, double elapsed, const GyroOptions& options) {
    if (!active || !armed_ || options != options_ || !std::isfinite(elapsed) ||
        elapsed <= 0.0 || elapsed > 0.1 || newest < last_sample_ ||
        newest - last_sample_ > 16) {
      filter_.Reset();
      projection_.Reset();
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
  GyroDelta SampleMotion(uint64_t number, float x, float y, float z,
                         float ax, float ay, float az, double seconds) {
    if (!armed_ || number <= last_sample_) return {};
    return Sample(number, x, projection_.Yaw(x, y, z, ax, ay, az, seconds), seconds);
  }

 private:
  GyroFilter filter_;
  GyroProjection projection_;
  GyroOptions options_;
  uint64_t last_sample_ = 0;
  bool armed_ = false;
};

}  // namespace fear_nx::input
