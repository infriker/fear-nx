// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace fear_nx::deko {
// Exact byte equality of CPU shadows. Never read uncached GPU memory or past
// either span. In particular, NaNs and signed zero retain their bit patterns.
inline bool EqualCpuBytes(const uint8_t* a,const uint8_t* b,size_t bytes) {
#if defined(__aarch64__)
  while(bytes>=64) {
    const auto x=veorq_u8(vld1q_u8(a),vld1q_u8(b));
    const auto y=veorq_u8(vld1q_u8(a+16),vld1q_u8(b+16));
    const auto z=veorq_u8(vld1q_u8(a+32),vld1q_u8(b+32));
    const auto w=veorq_u8(vld1q_u8(a+48),vld1q_u8(b+48));
    if(vmaxvq_u8(vorrq_u8(vorrq_u8(x,y),vorrq_u8(z,w))))return false;
    a+=64;b+=64;bytes-=64;
  }
#endif
  // A single guest register is cheaper without a SIMD horizontal reduction.
  // memcpy expresses unaligned, alias-safe integer loads (LDP/LDR on AArch64).
  while(bytes>=16) {
    uint64_t a0,a1,b0,b1;
    std::memcpy(&a0,a,8);std::memcpy(&a1,a+8,8);
    std::memcpy(&b0,b,8);std::memcpy(&b1,b+8,8);
    if(a0!=b0 || a1!=b1)return false;
    a+=16;b+=16;bytes-=16;
  }
  return !bytes || !std::memcmp(a,b,bytes);
}
} // namespace fear_nx::deko
