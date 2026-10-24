// SPDX-License-Identifier: GPL-3.0-or-later
#include "quality.h"
#include "fear_nx_pch.h"

#include <algorithm>
#include <array>
#include <atomic>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

REXCVAR_DEFINE_UINT32(
    nx_volumetric_light_slices, 128, "F.E.A.R. NX",
    "Volumetric-light quality: 128 is balanced, 256 is the Xbox 360 default, 0 keeps the game default.");
REXCVAR_DEFINE_UINT32(
    nx_volumetric_light_shadow_size, 64, "F.E.A.R. NX",
    "Volumetric shadow-map size: 64 is balanced, 128 is the Xbox 360 default, 0 keeps the game default.");

namespace fear_nx::game {
namespace {
std::atomic<bool> verified{false};
std::atomic<bool> logged_applied{false};

constexpr uint32_t kVolumetricResourceInit = 0x8241E338;
constexpr uint32_t kSlicesVarTrack = 0x83374D2C;
constexpr uint32_t kShadowSizeVarTrack = 0x83374D5C;
constexpr uint32_t kSlicesNamePointer = 0x82020D34;
constexpr uint32_t kShadowSizeNamePointer = 0x82020CE4;
constexpr uint32_t kVarTrackInterface = 4;
constexpr uint32_t kVarTrackName = 8;
constexpr uint32_t kSetConsoleVariableFloatVtableOffset = 0x104;
constexpr uint32_t kMaxXenonSlices = 256;
constexpr uint32_t kRetailShadowSize = 128;

bool IsExpectedName(uint8_t* base, uint32_t address,
                    uint32_t expected_address, const char* expected) {
  if (address != expected_address) return false;
  for (size_t i = 0;; ++i) {
    const uint8_t expected_byte = static_cast<uint8_t>(expected[i]);
    if (REX_LOAD_U8(address + uint32_t(i)) != expected_byte) return false;
    if (!expected_byte) return true;
  }
}

bool SetVarTrackFloat(uint8_t* base, uint32_t track_address,
                      uint32_t expected_name_address, const char* expected_name,
                      float requested) {
  // The VarTrack is initialized by the verified guest function above. Its
  // fields are (handle, LTCS interface, variable name), as used by the retail
  // VarTrack::Init implementation.
  const uint32_t client = REX_LOAD_U32(track_address + kVarTrackInterface);
  const uint32_t name = REX_LOAD_U32(track_address + kVarTrackName);
  if (!client || !IsExpectedName(base, name, expected_name_address, expected_name)) {
    return false;
  }

  const uint32_t vtable = REX_LOAD_U32(client);
  if (!vtable) return false;
  const uint32_t setter = REX_LOAD_U32(vtable + kSetConsoleVariableFloatVtableOffset);
  if (!setter) return false;

  auto* runtime = rex::Runtime::instance();
  if (!runtime || !runtime->function_dispatcher() ||
      !rex::runtime::ThreadState::Get()) {
    return false;
  }
  auto* function = runtime->function_dispatcher()->GetFunction(setter);
  if (!function) return false;

  // The game's own VarTrack::Init calls this vtable entry with (interface,
  // name, defaultFloat), proving both the slot and mixed GPR/FPR signature.
  rex::ppc::GuestToHostFunction<void>(function, client, name, requested);
  return true;
}
}  // namespace

void VerifyQualityHook(uint8_t* base) {
  verified = false;

  // First instructions of CVLFXResourceMgr::Init in the supported retail XEX.
  constexpr std::array<uint32_t, 4> expected = {
      0x7D8802A6, 0x9181FFF8, 0xFBC1FFE8, 0xFBE1FFF0};
  for (size_t i = 0; i < expected.size(); ++i) {
    if (REX_LOAD_U32(kVolumetricResourceInit + uint32_t(i * 4)) != expected[i]) {
      REXLOG_ERROR("F.E.A.R. NX volumetric quality override disabled: signature mismatch at {:08X}",
                   kVolumetricResourceInit);
      return;
    }
  }

  verified = true;
  REXLOG_INFO("F.E.A.R. NX volumetric quality hook verified; requested slices={}, shadow size={}",
              REXCVAR_GET(nx_volumetric_light_slices),
              REXCVAR_GET(nx_volumetric_light_shadow_size));
}
}  // namespace fear_nx::game

REX_EXTERN(__imp__sub_8241E338);

// CVLFXResourceMgr::Init creates the game's volumetric-light VarTracks. Apply
// the override once initialization succeeds, before effects read the setting.
REX_HOOK_RAW(sub_8241E338) {
  using namespace fear_nx::game;
  __imp__sub_8241E338(ctx, base);

  if (!verified || !ctx.r3.u8) return;
  const uint64_t init_result = ctx.r3.u64;
  const uint32_t requested = REXCVAR_GET(nx_volumetric_light_slices);
  bool slices_applied = false;
  if (requested != 0) {  // Zero explicitly keeps the retail default.
    const uint32_t slices = std::clamp(requested, 1u, kMaxXenonSlices);
    slices_applied = SetVarTrackFloat(base, kSlicesVarTrack, kSlicesNamePointer,
                                     "VolumetricLightSlices", static_cast<float>(slices));
    if (!slices_applied) {
      REXLOG_WARN("F.E.A.R. NX could not set VolumetricLightSlices; keeping the game value");
    }
  }

  const uint32_t requested_shadow_size =
      REXCVAR_GET(nx_volumetric_light_shadow_size);
  bool shadow_size_applied = false;
  if (requested_shadow_size != 0) {  // Zero keeps the SDK's 128x128 default.
    if (requested_shadow_size != 64 && requested_shadow_size != kRetailShadowSize) {
      REXLOG_WARN("F.E.A.R. NX ignores unsupported VolumetricLightShadowSize={} (use 0, 64, or 128)",
                  requested_shadow_size);
    } else {
      shadow_size_applied = SetVarTrackFloat(
          base, kShadowSizeVarTrack, kShadowSizeNamePointer,
          "VolumetricLightShadowSize", static_cast<float>(requested_shadow_size));
      if (!shadow_size_applied) {
        REXLOG_WARN("F.E.A.R. NX could not set VolumetricLightShadowSize; keeping the game value");
      }
    }
  }
  // Both guest setters above can dispatch through translated functions. Keep
  // CVLFXResourceMgr::Init's original return value intact for its caller.
  ctx.r3.u64 = init_result;

  if ((slices_applied || shadow_size_applied) &&
      !logged_applied.exchange(true)) {
    REXLOG_INFO("F.E.A.R. NX volumetric profile applied: slices request={}, shadow size request={} (lighting and volumetric shadows remain enabled)",
                requested, requested_shadow_size);
  }
}
