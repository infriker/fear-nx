// SPDX-License-Identifier: GPL-3.0-or-later
// EchoPatch's approach: gate on actual aim state, add radians to the local
// player-camera rotation. Native libnx HID replaces the PC SDL/Win32 path.
#include "gyro_input.h"
#include "gyro_math.h"
#include "fear_nx_pch.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <switch.h>

REXCVAR_DEFINE_BOOL(input_gyro_enabled, true, "Input",
                   "Gyroscope aiming (only while the weapon is aimed). Takes effect immediately.");
REXCVAR_DEFINE_DOUBLE(input_gyro_sensitivity, 1.0, "Input",
                     "Gyro sensitivity: 1.0 = one degree of camera turn per degree of controller turn.")
    .range(0.0, 10.0);
REXCVAR_DEFINE_DOUBLE(input_gyro_smoothing_ms, 16.0, "Input",
                     "Gyro smoothing in milliseconds; 0 disables smoothing.").range(0.0, 100.0);
REXCVAR_DEFINE_DOUBLE(input_gyro_deadzone_dps, 0.15, "Input",
                     "Gyro drift deadzone in degrees per second.").range(0.0, 5.0);
REXCVAR_DEFINE_BOOL(input_gyro_invert_y, false, "Input", "Invert vertical gyro aiming.");

namespace fear_nx::input {
namespace {
using Clock = std::chrono::steady_clock;
using rex::X_RESULT;  // SDK status macros use the unqualified typedefs.
using rex::X_STATUS;
std::atomic<bool> input_active{false};
std::atomic<bool> focused{true};
std::atomic<bool> verified{false};
std::atomic<bool> shutting_down{false};
std::atomic<uint64_t> reset_generation{0};

// Fixed addresses for the SHA-256-validated Xbox executable. The aim singleton
// is read afresh, so load/death/weapon cancellation cannot leave a latched flag.
constexpr uint32_t kPlayerMgr = 0x833708DC;
constexpr uint32_t kAimState = 0x83371764;
thread_local uint32_t gameplay_camera = 0;
thread_local bool rotation_consumed = false;

class MotionSource {
 public:
  GyroDelta Read(bool aiming) {
    std::lock_guard lock(mutex_);
    if (shutting_down || !verified || !REXCVAR_GET(input_gyro_enabled)) {
      Stop();
      return {};
    }
    const auto now = Clock::now();
    const double elapsed = std::chrono::duration<double>(now - last_read_).count();
    last_read_ = now;
    const uint64_t generation = reset_generation.load();
    if (generation != generation_) {
      integrator_.Reset();
      generation_ = generation;
    }
    if (!hid_ready_) {
      if (now < retry_at_) return {};
      const Result rc = hidInitialize();
      if (R_FAILED(rc)) {
        retry_at_ = now + std::chrono::seconds(1);
        return {};
      }
      hid_ready_ = true;
      padInitializeDefault(&pad_);
    }
    padUpdate(&pad_);
    const auto [id, style, side] = SelectSensor();
    if (!style) {
      StopSensor();
      return {};
    }
    if (style != style_ || id != id_ || side != side_) {
      StopSensor();
      retry_at_ = {};
    }
    if (!started_) {
      if (now < retry_at_) return {};
      std::array<HidSixAxisSensorHandle, 2> handles{};
      const int count = style == HidNpadStyleTag_NpadJoyDual ? 2 : 1;
      Result rc = hidGetSixAxisSensorHandles(handles.data(), count, id,
                                             static_cast<HidNpadStyleTag>(style));
      if (R_SUCCEEDED(rc)) {
        handle_ = handles[side];
        rc = hidStartSixAxisSensor(handle_);
      }
      id_ = id;
      style_ = style;
      side_ = side;
      if (R_FAILED(rc)) {
        REXLOG_WARN("F.E.A.R. NX gyro: sensor start failed: {:08X}", rc);
        retry_at_ = now + std::chrono::seconds(1);
        return {};
      }
      started_ = true;
      // Horizon performs stationary drift calibration; no PC calibration files
      // or controller serial numbers are needed.
      hidSetGyroscopeZeroDriftMode(handle_, HidGyroscopeZeroDriftMode_Standard);
      REXLOG_INFO("F.E.A.R. NX gyro: sensor started (style={}, side={})", style, side);
    }
    std::array<HidSixAxisSensorState, 16> samples{};
    const size_t count = hidGetSixAxisSensorStates(handle_, samples.data(), samples.size());
    if (!count || !(samples[0].attributes & HidSixAxisSensorAttribute_IsConnected)) {
      integrator_.Reset();
      return {};
    }
    const GyroOptions options{REXCVAR_GET(input_gyro_sensitivity),
                              REXCVAR_GET(input_gyro_smoothing_ms),
                              REXCVAR_GET(input_gyro_deadzone_dps),
                              REXCVAR_GET(input_gyro_invert_y)};
    const bool active = aiming && input_active && focused;
    if (!integrator_.BeginFrame(active, samples[0].sampling_number, elapsed, options)) return {};
    GyroDelta delta;
    // libnx returns newest first. Integrate each fresh sample oldest first.
    for (size_t i = count; i-- > 0;) {
      const auto& sample = samples[i];
      if (!(sample.attributes & HidSixAxisSensorAttribute_IsConnected)) continue;
      const auto part = integrator_.Sample(sample.sampling_number, sample.angular_velocity.x,
                                          sample.angular_velocity.z, sample.delta_time * 1e-9);
      delta.pitch += part.pitch;
      delta.yaw += part.yaw;
    }
    return delta;
  }

  void Shutdown() {
    std::lock_guard lock(mutex_);
    Stop();
  }

 private:
  struct Sensor { HidNpadIdType id; uint32_t style; int side; };
  Sensor SelectSensor() const {
    if (!padIsConnected(&pad_)) return {};
    if (pad_.active_handheld)
      return {HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld, 0};
    const uint32_t style = padGetStyleSet(&pad_);
    if (style & HidNpadStyleTag_NpadFullKey)
      return {HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey, 0};
    if (style & HidNpadStyleTag_NpadJoyDual) {
      const uint32_t attributes = padGetAttributes(&pad_);
      // Detached pair: aim with the right Joy-Con, falling back to the left
      // only if the right one is absent. Never combine both motion streams.
      if (attributes & HidNpadAttribute_IsRightConnected)
        return {HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual, 1};
      if (attributes & HidNpadAttribute_IsLeftConnected)
        return {HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual, 0};
    }
    return {};  // Single sideways Joy-Cons are not a full F.E.A.R. controller.
  }
  void StopSensor() {
    if (started_) hidStopSixAxisSensor(handle_);
    started_ = false;
    style_ = 0;
    integrator_.Reset();
  }
  void Stop() {
    StopSensor();
    if (hid_ready_) hidExit();
    hid_ready_ = false;
    last_read_ = {};
  }
  std::mutex mutex_;
  PadState pad_{};
  HidSixAxisSensorHandle handle_{};
  HidNpadIdType id_ = HidNpadIdType_No1;
  uint32_t style_ = 0;
  int side_ = 0;
  bool hid_ready_ = false;
  bool started_ = false;
  Clock::time_point last_read_{};
  Clock::time_point retry_at_{};
  uint64_t generation_ = 0;
  GyroIntegrator integrator_;
};
MotionSource motion;

// Observe the SDK's existing menu-capture callback without altering its driver,
// device assignment, face buttons, stick input, shortcuts or vibration.
class InputActivityObserver final : public rex::input::InputDriver {
 public:
  InputActivityObserver() : InputDriver(nullptr, 0) {}
  rex::X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>&) override {
    const bool active = is_active();
    if (input_active.exchange(active) != active) ++reset_generation;
    if (!REXCVAR_GET(input_gyro_enabled)) motion.Shutdown();
  }
  rex::X_RESULT GetDeviceState(rex::input::DeviceId, rex::input::X_INPUT_STATE*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  rex::X_RESULT GetDeviceCapabilities(rex::input::DeviceId, uint32_t,
                                      rex::input::X_INPUT_CAPABILITIES*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  rex::X_RESULT SetDeviceVibration(rex::input::DeviceId, rex::input::X_INPUT_VIBRATION*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  rex::X_RESULT GetDeviceKeystroke(rex::input::DeviceId, uint32_t,
                                   rex::input::X_INPUT_KEYSTROKE*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
};

}  // namespace

std::unique_ptr<rex::input::InputSystem> CreateInputSystem(bool tool_mode) {
  auto system = rex::input::CreateDefaultInputSystem(tool_mode);
  if (!tool_mode) system->AddDriver(std::make_unique<InputActivityObserver>());
  return system;
}

void SetGyroFocus(bool value) {
  if (focused.exchange(value) != value) ++reset_generation;
}

void ShutdownGyro() {
  shutting_down = true;
  input_active = false;
  motion.Shutdown();
}

void VerifyGyroHooks(uint8_t* base) {
  // XEX hash is also checked by FearNxApp before the game is loaded.
  struct Signature { uint32_t address; std::array<uint32_t, 4> words; };
  constexpr Signature signatures[] = {
      {0x8237F648, {0x7D8802A6, 0x9181FFF8, 0xFBE1FFF0, 0x9421FF90}},
      {0x823B70A8, {0x7D8802A6, 0x9181FFF8, 0xFBE1FFF0, 0x9421FF80}},
      {0x82387DB8, {0x7D8802A6, 0x9181FFF8, 0xFBE1FFF0, 0x9421FFA0}},
  };
  for (const auto& signature : signatures) {
    for (size_t i = 0; i < signature.words.size(); ++i) {
      if (REX_LOAD_U32(signature.address + uint32_t(i * 4)) != signature.words[i]) {
        REXLOG_ERROR("F.E.A.R. NX gyro disabled: game signature mismatch at {:08X}",
                     signature.address);
        verified = false;
        return;
      }
    }
  }
  verified = true;
  REXLOG_INFO("F.E.A.R. NX gyro: verified aim state and player-camera hooks");
}

}  // namespace fear_nx::input

REX_EXTERN(__imp__sub_8237F648);
REX_EXTERN(__imp__sub_823B70A8);

// CPlayerMgr::UpdatePlayerMovement: only its local input rotation may get gyro.
REX_HOOK_RAW(sub_8237F648) {
  using namespace fear_nx::input;
  const uint32_t previous_camera = gameplay_camera;
  const bool previous_consumed = rotation_consumed;
  const uint32_t player = ctx.r3.u32;
  gameplay_camera = verified && player && REX_LOAD_U32(kPlayerMgr) == player
                        ? REX_LOAD_U32(player + 40) : 0;
  rotation_consumed = false;
  __imp__sub_8237F648(ctx, base);
  // If the game's own menu/cutscene gate suppressed rotation, don't carry
  // pre-menu sensor motion into the next usable gameplay update.
  if (!rotation_consumed) ++reset_generation;
  gameplay_camera = previous_camera;
  rotation_consumed = previous_consumed;
}

// CPlayerCamera::ApplyLocalRotationOffset, also used by recoil and scripts.
REX_HOOK_RAW(sub_823B70A8) {
  using namespace fear_nx::input;
  if (gameplay_camera && ctx.r3.u32 == gameplay_camera && !rotation_consumed) {
    rotation_consumed = true;
    const auto delta = motion.Read(REX_LOAD_U8(kAimState) != 0);
    const uint32_t offset = ctx.r4.u32;
    // The verified caller passes a stack-local float[3]. Leave roll unchanged.
    const float pitch = std::bit_cast<float>(REX_LOAD_U32(offset));
    const float yaw = std::bit_cast<float>(REX_LOAD_U32(offset + 4));
    if (delta.pitch != 0.0f)
      REX_STORE_U32(offset, std::bit_cast<uint32_t>(pitch + delta.pitch));
    if (delta.yaw != 0.0f)
      REX_STORE_U32(offset + 4, std::bit_cast<uint32_t>(yaw + delta.yaw));
  }
  __imp__sub_823B70A8(ctx, base);
}
