// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Texture scheduling follows NFSMW 266f943: nfsmw_dk_texturas.cpp and
// nfsmw_nativo_xenos.h. Geometry epochs and their quarantines are independent.
#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif
#include <xxhash.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>

namespace fear_nx::deko {
class TextureChecks {
 public:
  static constexpr uint32_t kGuardChecks=3000, kMaxDeferrals=8, kExcludedBases=4;
  static constexpr uint64_t kBudget=6144*1024, kBlock=4096, kStride=8;
  struct State {
    uint64_t hash=0, sample=0, next=0, last=std::numeric_limits<uint64_t>::max();
    uint32_t interval=1, deferrals=0, samples=0;
    bool valid=false, sample_valid=false;
  };
  struct Stats {
    uint64_t lookups=0, frame_hits=0, scheduled_hits=0, full_checks=0, full_bytes=0;
    uint64_t sample_checks=0, sample_bytes=0, sample_hits=0, guard_checks=0, guard_misses=0;
    uint64_t budget_deferrals=0, changes=0, excluded_bases=0;
    uint32_t training=0;
    bool sampling_disabled=false;
  };
  struct Result {uint64_t hash=0, bytes=0;bool guard_miss=false;};
  const Stats& stats() const {return stats_;}
  static uint64_t SampleBytes(uint64_t size) {
    if(!size)return 0;
    const auto last=(size-1)/kBlock;
    return (last+kStride-1)/kStride*kBlock+size-last*kBlock;
  }
  static uint64_t Sample(std::span<const uint8_t> data,uint64_t seed=0) {
    if(data.empty())return seed;
    const auto last=(data.size()-1)/kBlock;
    for(uint64_t block=0;block<last;block+=kStride)
      seed=XXH3_64bits_withSeed(data.data()+block*kBlock,kBlock,seed);
    return XXH3_64bits_withSeed(data.data()+last*kBlock,data.size()-last*kBlock,seed);
  }
  Result Check(State& s,uint64_t frame,uint32_t base_address,uint64_t layout_key,
               std::span<const uint8_t> base,std::span<const uint8_t> mips,
               bool enabled=true,bool allow_sample=true) {
    ++stats_.lookups;
    if(frame_!=frame){frame_=frame;frame_bytes_=0;}
    if(enabled && s.valid) {
      if(s.last==frame){++stats_.frame_hits;return {s.hash};}
      if(frame<s.next){++stats_.scheduled_hits;return {s.hash};}
    }
    const uint64_t full_bytes=base.size()+mips.size();
    const uint64_t sample_bytes=SampleBytes(base.size())+SampleBytes(mips.size());
    const bool excluded=std::find(excluded_.begin(),excluded_.begin()+excluded_count_,base_address)
                        !=excluded_.begin()+excluded_count_;
    const bool sample=enabled && allow_sample && !stats_.sampling_disabled && !excluded &&
        s.valid && s.interval>=4 && full_bytes && sample_bytes*2<=full_bytes;
    const bool useful=sample && s.interval>=8 && s.sample_valid;
    const bool sample_only=useful && stats_.training>=kGuardChecks && s.samples+1<kStride;
    uint64_t bytes=sample_only?sample_bytes:full_bytes+(sample?sample_bytes:0);
    // Only established stable contents may be delayed. A first/changed texture
    // always hashes immediately, and no stable texture can starve indefinitely.
    if(enabled && s.valid && s.interval>=8 && s.deferrals<kMaxDeferrals &&
        frame_bytes_ && frame_bytes_+bytes>kBudget) {
      ++s.deferrals;++stats_.budget_deferrals;
      s.next=frame+1+(layout_key&1);s.last=frame;
      return {s.hash};
    }
    s.deferrals=0;s.last=frame;frame_bytes_+=bytes;
    auto unchanged=[&]() {
      s.interval=std::min(s.interval*2,32u);
      s.next=frame+s.interval+(s.interval==32?((layout_key>>7)&7):0);
    };
    uint64_t sampled=0;
    if(sample) {
      sampled=Sample(mips,Sample(base));
      ++stats_.sample_checks;stats_.sample_bytes+=sample_bytes;
    }
    const bool sample_equal=useful && sampled==s.sample;
    if(sample_only && sample_equal) {
      ++s.samples;++stats_.sample_hits;unchanged();return {s.hash,bytes};
    }
    if(sample_only){bytes+=full_bytes;frame_bytes_+=full_bytes;}
    uint64_t full=XXH3_64bits(base.data(),base.size());
    if(!mips.empty())full=XXH3_64bits_withSeed(mips.data(),mips.size(),full);
    ++stats_.full_checks;stats_.full_bytes+=full_bytes;
    const bool equal=s.valid && full==s.hash;
    bool miss=false;
    if(useful && !sample_only) {
      ++stats_.guard_checks;++stats_.training;
      if(sample_equal && !equal) {
        miss=true;++stats_.guard_misses;s.sample_valid=false;
        if(excluded_count_==kExcludedBases)stats_.sampling_disabled=true;
        else {excluded_[excluded_count_++]=base_address;stats_.excluded_bases=excluded_count_;stats_.training=0;}
      }
    }
    if(equal && enabled) {
      if(sample && !miss && !stats_.sampling_disabled){s.sample=sampled;s.sample_valid=true;}
      s.samples=0;unchanged();
    } else {
      if(s.valid && !equal)++stats_.changes;
      s.hash=full;s.valid=true;s.sample_valid=false;s.samples=0;s.interval=1;s.next=frame+1;
    }
    return {full,bytes,miss};
  }
 private:
  Stats stats_{};
  std::array<uint32_t,kExcludedBases> excluded_{};
  uint32_t excluded_count_=0;
  uint64_t frame_=std::numeric_limits<uint64_t>::max(),frame_bytes_=0;
};
} // namespace fear_nx::deko
