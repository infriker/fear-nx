// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include "deko_profile.h"
#include "deko_vertex_write.h"
#if defined(__SWITCH__)
extern "C" {
#include <switch/kernel/svc.h>
#include <switch/runtime/hosversion.h>
}
#endif

namespace fear_nx::deko {

// Bounded diagnostic only, never writes into an allocation referenced by draws.
// All three paths use the same payload size and the production NEON writer.
// GPU means CPU stores into CpuUncached/GpuCached upload memory, NOT GPU time.
class VertexWriteProbe {
 public:
  static constexpr uint32_t kMaxBytes=128u<<10;
  struct Stats {
    uint64_t samples=0,bytes=0,overhead_ns=0,allocation_skips=0;
    std::array<uint64_t,3> wall_ns{},thread_ns{},thread_samples{};
  };
  bool Due(uint64_t frame,uint32_t bytes) {
    if(bytes<4096 || frame<next_frame_)return false;
    const uint64_t now=ProfileClockNs();
    if(now<next_ns_)return false;
    next_frame_=frame+60;next_ns_=now+1000000000ULL;
    return true;
  }
  void AllocationSkipped() {++stats_.allocation_skips;}
  void AddOverhead(uint64_t ns) {stats_.overhead_ns+=ns;}
  void Run(const uint8_t* guest,uint8_t* gpu,uint32_t bytes,bool fast) {
    // Alternate which GPU write goes first to expose warm-page bias over a run.
    const bool direct_first=(stats_.samples&1)==0;
    const auto copy=[&](uint8_t* dst,const uint8_t* src) {
      if(fast)WriteVertexBytes(dst,src,bytes,bytes);
      else std::memcpy(dst,src,bytes);
    };
    if(direct_first)Measure(1,[&]{copy(gpu,guest);});
    Measure(0,[&]{copy(ram_.data(),guest);});
    Measure(2,[&]{copy(gpu,ram_.data());});
    if(!direct_first)Measure(1,[&]{copy(gpu,guest);});
    ++stats_.samples;stats_.bytes+=bytes;
  }
  const Stats& stats() const {return stats_;}
 private:
  static bool ThreadTicks(uint64_t& ticks) {
#if defined(__SWITCH__)
    const auto type=hosversionAtLeast(13,0,0)?InfoType_ThreadTickCount:InfoType_ThreadTickCountDeprecated;
    return svcGetInfo(&ticks,type,CUR_THREAD_HANDLE,UINT64_MAX)==0;
#else
    (void)ticks;return false;
#endif
  }
  template<class Copy> void Measure(size_t path,Copy&& copy) {
    uint64_t before=0,after=0;
    const bool have_before=ThreadTicks(before);
    const uint64_t start=ProfileClockNs();
    copy();
    const uint64_t elapsed=ProfileClockNs()-start;
    const bool have_after=ThreadTicks(after);
    stats_.wall_ns[path]+=elapsed;
    // Some emulators return success with zero ticks. Do not call that CPU time.
    if(have_before && have_after && after>before) {
      stats_.thread_ns[path]+=(after-before)*625/12;
      ++stats_.thread_samples[path];
    }
  }
  alignas(64) std::array<uint8_t,kMaxBytes> ram_{};
  uint64_t next_frame_=0,next_ns_=0;
  Stats stats_{};
};
} // namespace fear_nx::deko
