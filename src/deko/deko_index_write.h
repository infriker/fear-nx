// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace fear_nx::deko {

// Fresh, exclusively owned GPU upload memory only. Neither source overreads
// nor overlapping head/tail stores: uncached memory is written in order once.
// A partial vector is padded only if the caller owns the complete vector.
inline uint32_t WriteIndexBytes(uint8_t* dst, const uint8_t* src,
                               uint32_t bytes, uint32_t owned_bytes) {
  if (!bytes) return 0;
  uint32_t i=0;
#if defined(__aarch64__)
  for (; bytes-i>=64; i+=64) {
    const auto a=vld1q_u8(src+i),b=vld1q_u8(src+i+16);
    const auto c=vld1q_u8(src+i+32),d=vld1q_u8(src+i+48);
    vst1q_u8(dst+i,a);vst1q_u8(dst+i+16,b);
    vst1q_u8(dst+i+32,c);vst1q_u8(dst+i+48,d);
  }
  for (; bytes-i>=16; i+=16) vst1q_u8(dst+i,vld1q_u8(src+i));
#else
  i=bytes&~15u;
  if(i)std::memcpy(dst,src,i);
#endif
  if (i!=bytes) {
    if (owned_bytes>=i && owned_bytes-i>=16) {
      alignas(16) uint8_t tail[16]{};
      std::memcpy(tail,src+i,bytes-i);
#if defined(__aarch64__)
      vst1q_u8(dst+i,vld1q_u8(tail));
#else
      std::memcpy(dst+i,tail,16);
#endif
      return i+16;
    }
    std::memcpy(dst+i,src+i,bytes-i);
  }
  return bytes;
}

// Coalesce adjacent NEW resident-cache allocations, never hits or live older
// blocks. Caller proves ownership of each padded range through the allocator.
// No sorting or gap filling: a gap drains the previous span before starting
// another. Source is captured now, even if its conversion cache resets later.
template <uint32_t CapacityBytes>
class ResidentUploadBatch {
 public:
  static_assert(CapacityBytes && CapacityBytes%64==0);
  static constexpr uint32_t kCapacity=CapacityBytes;
  ResidentUploadBatch()=default;
  ResidentUploadBatch(const ResidentUploadBatch&)=delete;
  ResidentUploadBatch& operator=(const ResidentUploadBatch&)=delete;
  template <typename Write>
  bool Append(uint8_t* destination, uint64_t gpu,
              std::span<const uint8_t> source, uint32_t owned_bytes, Write&& write) {
    if(!destination || source.empty() || source.size()>kCapacity ||
       owned_bytes<source.size() || owned_bytes>kCapacity) return false;
    if(used_ && (reinterpret_cast<uintptr_t>(destination)!=reinterpret_cast<uintptr_t>(destination_)+used_ ||
                  gpu!=gpu_+used_ || owned_bytes>kCapacity-used_)) Flush(write);
    if(!used_){destination_=destination;gpu_=gpu;}
    std::memcpy(shadow_.data()+used_,source.data(),source.size());
    std::memset(shadow_.data()+used_+source.size(),0,owned_bytes-source.size());
    used_+=owned_bytes;
    return true;
  }
  template <typename Write> void Flush(Write&& write) {
    if(used_)write(destination_,shadow_.data(),used_);
    destination_=nullptr;gpu_=0;used_=0;
  }
  bool pending() const {return used_!=0;}
 private:
  alignas(16) std::array<uint8_t,kCapacity> shadow_{};
  uint8_t* destination_=nullptr;
  uint64_t gpu_=0;
  uint32_t used_=0;
};
using ResidentIndexUploadBatch = ResidentUploadBatch<64u<<10>;
} // namespace fear_nx::deko
