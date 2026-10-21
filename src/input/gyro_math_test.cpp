// SPDX-License-Identifier: GPL-3.0-or-later
#include "gyro_math.h"
#include <cassert>
#include <cstdio>
#include <limits>

using namespace fear_nx::input;
void Near(double actual, double expected, double tolerance = 1e-6) {
  assert(std::abs(actual - expected) < tolerance);
}

int main() {
  const GyroOptions raw{1.0, 0.0, 0.0, false};
  GyroFilter filter;
  auto delta = filter.Step(0.25f, -0.5f, 0.005, raw);
  Near(delta.pitch, -std::numbers::pi / 400.0);
  Near(delta.yaw, std::numbers::pi / 200.0);
  auto inverted = raw;
  inverted.invert_y = true;
  Near(filter.Step(0.25f, 0.0f, 0.005, inverted).pitch, -delta.pitch);
  auto doubled = raw;
  doubled.sensitivity = 2.0;
  Near(filter.Step(0.0f, -0.5f, 0.005, doubled).yaw, 2.0 * delta.yaw);
  const auto bad = filter.Step(std::numeric_limits<float>::quiet_NaN(), 1, 0.005, raw);
  Near(bad.pitch, 0); Near(bad.yaw, 0);
  Near(filter.Step(1, 1, 1.0, raw).yaw, 0);
  auto quiet = raw;
  quiet.deadzone_dps = 0.15;
  Near(filter.Step(0.0001f, 0.0001f, 0.005, quiet).yaw, 0);

  // At 200 Hz, grouping into 25, 30 and 60 render frames must preserve angle.
  const auto run = [](int fps) {
    GyroIntegrator integrator;
    GyroOptions options;
    options.deadzone_dps = 0;
    integrator.BeginFrame(true, 1, 0.0, options);
    double sum = 0;
    for (int frame = 1; frame <= fps; ++frame) {
      const uint64_t newest = 1 + frame * 200 / fps;
      if (integrator.BeginFrame(true, newest, 1.0 / fps, options)) {
        for (uint64_t n = newest > 15 ? newest - 15 : 1; n <= newest; ++n)
          sum += integrator.Sample(n, 0, 0.25f, 0.005).yaw;
      }
    }
    return sum;
  };
  Near(run(25), run(30)); Near(run(30), run(60));
  const double expected = -std::numbers::pi / 2 * (1 - 0.016 * (1 - std::exp(-1 / 0.016)));
  Near(run(30), expected);

  GyroIntegrator integrator;
  assert(!integrator.BeginFrame(false, 10, 0.03, raw));
  Near(integrator.Sample(11, 1, 1, 0.005).yaw, 0);
  assert(!integrator.BeginFrame(true, 20, 0.03, raw));  // discard hip-fire buffer
  assert(integrator.BeginFrame(true, 21, 0.03, raw));
  assert(integrator.Sample(21, 0, 1, 0.005).yaw != 0);
  Near(integrator.Sample(21, 0, 1, 0.005).yaw, 0);  // don't replay duplicate sample
  assert(!integrator.BeginFrame(false, 25, 0.03, raw));
  assert(!integrator.BeginFrame(true, 28, 0.03, raw));
  Near(integrator.Sample(28, 0, 1, 0.005).yaw, 0);
  assert(!integrator.BeginFrame(true, 35, 1.0, raw));  // pause/resume
  assert(!integrator.BeginFrame(true, 1, 0.03, raw));  // sensor reconnected
  assert(!integrator.BeginFrame(true, 50, 0.03, raw)); // ring buffer overrun
  assert(!integrator.BeginFrame(true, 51, 0.03, inverted));
  integrator.Reset();
  assert(!integrator.BeginFrame(true, 52, 0.03, raw));
  std::puts("Gyro tests passed: units, axes, gain, drift, frame rate, aim gates, stale samples.");
}
