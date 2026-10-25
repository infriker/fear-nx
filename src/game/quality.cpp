// SPDX-License-Identifier: GPL-3.0-or-later
#include "quality.h"
#include "fear_nx_pch.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

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

// Release profile; both numeric settings use the game's own FX controls.
REXCVAR_DEFINE_UINT32(nx_client_fx_detail_level, 1, "F.E.A.R. NX",
                     "Client FX detail: 0 low, 1 medium, 2 original Xbox 360 detail. Restart required.")
    .range(0u, 2u).lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(nx_particle_bounce_scale, 0.5, "F.E.A.R. NX",
                     "Scale eligible particle bounce/splat probabilities: 0 off, 1 original. Restart required.")
    .range(0.0, 1.0).lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace fear_nx::game {
namespace {
std::atomic<bool> verified{false};
std::atomic<bool> logged_applied{false};
std::atomic<bool> effects_verified{false};

constexpr uint32_t kVarTrackInit = 0x822FC710;
constexpr uint32_t kClientFxDetailName = 0x82016EC0;
constexpr uint32_t kParticleBounceName = 0x820A6AB8;

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
bool MatchesSignature(uint8_t* base, uint32_t address,
                      const std::array<uint32_t, 4>& expected) {
  for (size_t i = 0; i < expected.size(); ++i) {
    if (REX_LOAD_U32(address + uint32_t(i * 4)) != expected[i]) return false;
  }
  return true;
}

void VerifyEffectHooks(uint8_t* base) {
  // Both variables use this exact VarTrack::Init in the supported XEX:
  // ClientFXDetailLevel: sub_8234C0A0, call at 0x8234C0F0 (default 2).
  // ParticleBounceScale: sub_82BB8FA0, call at 0x82BB9014 (default 1).
  effects_verified =
      MatchesSignature(base, kVarTrackInit,
                       {0x7D8802A6, 0x4857EDB5, 0xDBE1FFD0, 0x9421FF80}) &&
      IsExpectedName(base, kClientFxDetailName, kClientFxDetailName, "ClientFXDetailLevel") &&
      IsExpectedName(base, kParticleBounceName, kParticleBounceName, "ParticleBounceScale");
  if (!effects_verified) {
    REXLOG_ERROR("F.E.A.R. NX FX overrides disabled: guest signature/name mismatch");
  }
}
}  // namespace

void VerifyQualityHook(uint8_t* base) {
  VerifyEffectHooks(base);
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

REX_EXTERN(__imp__sub_822FC710);

// VarTrack::Init(this=r3, client=r4, name=r5, textDefault=r6, floatDefault=f1).
// Match only the two verified names. Other variables retain all original inputs.
REX_HOOK_RAW(sub_822FC710) {
  using namespace fear_nx::game;
  const uint32_t name = ctx.r5.u32;
  const char* expected = nullptr;
  float requested = 0.0f;
  if (effects_verified) {
    if (name == kClientFxDetailName) {
      expected = "ClientFXDetailLevel";
      requested = static_cast<float>(std::min(REXCVAR_GET(nx_client_fx_detail_level), 2u));
    } else if (name == kParticleBounceName) {
      expected = "ParticleBounceScale";
      const double scale = REXCVAR_GET(nx_particle_bounce_scale);
      requested = static_cast<float>(std::isfinite(scale) ? std::clamp(scale, 0.0, 1.0) : 0.5);
    }
  }
  if (!expected || !IsExpectedName(base, name, name, expected)) {
    __imp__sub_822FC710(ctx, base);
    return;
  }

  const uint32_t track = ctx.r3.u32;
  ctx.r6.u64 = 0;  // A text default must not override our numeric value.
  ctx.f1.f64 = requested;
  __imp__sub_822FC710(ctx, base);
  if (!ctx.r3.u32 || !track) return;

  // Init leaves an already existing cvar unchanged. Apply through the game's
  // setter after successful Init as well, before the effect consumes the value.
  const uint64_t init_result = ctx.r3.u64;
  if (!SetVarTrackFloat(base, track, name, expected, requested)) {
    REXLOG_WARN("F.E.A.R. NX could not apply {}={}; keeping the game value", expected, requested);
  }
  ctx.r3.u64 = init_result;
}
