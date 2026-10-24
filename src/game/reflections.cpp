// SPDX-License-Identifier: GPL-3.0-or-later
#include "reflections.h"
#include "fear_nx_pch.h"

#include <array>
#include <atomic>
#include <rex/cvar.h>
#include <rex/hook.h>

REXCVAR_DEFINE_BOOL(nx_planar_reflections, false, "F.E.A.R. NX",
                   "Dynamic reflections on water and mirrors. Restart the game after changing.");

namespace fear_nx::game {
namespace {
std::atomic<bool> verified{false};

// Layout of CRenderTargetFX in the SHA-256-validated Xbox executable.
constexpr uint32_t kRequiredLod = 52;
constexpr uint32_t kBoundToMaterial = 56;
constexpr uint32_t kMirror = 80;
constexpr uint32_t kRefraction = 81;
constexpr uint32_t kMaterialParameter = 116;
constexpr uint32_t kDisabledLod = 3;  // GetRenderTargetLOD clamps the active LOD to 0..2.

bool IsReflectionMap(uint8_t* base, uint32_t target) {
  constexpr char name[] = "tReflectionMap";
  constexpr uint32_t length = sizeof(name) - 1;
  const uint32_t parameter = target + kMaterialParameter;
  // Xbox MSVC std::string has a leading allocator word, then its 16-byte
  // inline buffer / heap pointer, length and capacity. See sub_82182060.
  if (REX_LOAD_U32(parameter + 20) != length) return false;
  const uint32_t capacity = REX_LOAD_U32(parameter + 24);
  if (capacity < length) return false;
  const uint32_t data = capacity < 16 ? parameter + 4 : REX_LOAD_U32(parameter + 4);
  if (!data || data > UINT32_MAX - length) return false;
  for (uint32_t i = 0; i < length; ++i) {
    if (REX_LOAD_U8(data + i) != uint8_t(name[i])) return false;
  }
  return true;
}
}  // namespace

void VerifyReflectionHook(uint8_t* base) {
  verified = false;
  struct Signature { uint32_t address; std::array<uint32_t, 4> words; };
  // Init, Update, current-LOD clamp, UnbindMaterialParam, string assignment,
  // and UpdateRenderTarget (which also runs while simulation is paused).
  // The application also validates the complete supported XEX before loading.
  constexpr Signature signatures[] = {
      {0x82310AC0, {0x7D8802A6, 0x4856AA09, 0x9421FF90, 0x3D608337}},
      {0x8230FFC8, {0x7D8802A6, 0x4856B501, 0x9421FF80, 0x7C7F1B78}},
      {0x823E93B0, {0x7D8802A6, 0x9181FFF8, 0xFBE1FFF0, 0x9421FF90}},
      {0x8230F988, {0x7D8802A6, 0x4856BB3D, 0x9421FF80, 0x7C7C1B78}},
      {0x82182060, {0x7D8802A6, 0x486F9461, 0x9421FF80, 0x7C9E2378}},
      {0x82310860, {0x7D8802A6, 0x4856AC61, 0xDBC1FFC0, 0xDBE1FFC8}},
  };
  for (const auto& signature : signatures) {
    for (size_t i = 0; i < signature.words.size(); ++i) {
      if (REX_LOAD_U32(signature.address + uint32_t(i * 4)) != signature.words[i]) {
        REXLOG_ERROR("F.E.A.R. NX reflection override disabled: signature mismatch at {:08X}",
                     signature.address);
        return;
      }
    }
  }
  verified = true;
  REXLOG_INFO("F.E.A.R. NX: verified planar reflection hook; reflections {} on level load",
              REXCVAR_GET(nx_planar_reflections) ? "enabled" : "disabled");
}
}  // namespace fear_nx::game

REX_EXTERN(__imp__sub_82310AC0);
REX_EXTERN(__imp__sub_82310860);
REX_EXTERN(__imp__sub_8230F988);

// CRenderTargetFX::Init. Keep construction, networking and ownership unchanged;
// select the game's existing LOD-disabled path once when the FX is created.
REX_HOOK_RAW(sub_82310AC0) {
  using namespace fear_nx::game;
  const uint32_t target = ctx.r3.u32;
  __imp__sub_82310AC0(ctx, base);
  if (!ctx.r3.u8 || !target || !verified || REXCVAR_GET(nx_planar_reflections)) return;
  if (!REX_LOAD_U8(target + kMirror)) return;
  // PolyGrid sets BOTH flags on its reflection and refraction channels. The
  // material parameter distinguishes them; leave tRefractionMap enabled.
  if (!REX_LOAD_U8(target + kRefraction) || IsReflectionMap(base, target)) {
    REX_STORE_U32(target + kRequiredLod, kDisabledLod);
  }
  // The normal Update unbinds the material and restores its default texture.
}

// CRenderTargetFX::UpdateRenderTarget. During pause the SFX Update/LOD check
// may be skipped. Check the Init marker before SetupRenderTargetGroup can bind
// anything, so paused creation cannot render a disabled reflection channel.
REX_HOOK_RAW(sub_82310860) {
  using namespace fear_nx::game;
  const uint32_t target = ctx.r3.u32;
  if (verified && target && REX_LOAD_U32(target + kRequiredLod) == kDisabledLod) {
    if (REX_LOAD_U8(target + kBoundToMaterial)) {
      __imp__sub_8230F988(ctx, base);  // RestoreInstanceParamDefault, preserving ownership.
    }
    ctx.r3.u64 = 1;  // Same success result as the original unbound-target path.
    return;
  }
  __imp__sub_82310860(ctx, base);
}
