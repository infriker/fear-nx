// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace fear_nx::deko {
// Exact work-local texture sets. The immediately preceding set retains its
// cheap path; alternating materials use one four-way bucket. Hashes and mask
// hints route lookups only, never establish fetch or resource identity.
// Callers must still validate every returned value's image/resolve/guest leases.
template<class Value,size_t Sets=32,size_t Ways=4,size_t PlanSlots=64>
class PreviousDrawTextures {
 public:
  static_assert(Sets && !(Sets&(Sets-1)) && Ways && PlanSlots && !(PlanSlots&(PlanSlots-1)));
  static constexpr size_t kCapacity=Sets*Ways;
  using Shaders=std::array<uintptr_t,4>;
  using Stamp=std::array<uint64_t,8>;
  struct Stats {uint64_t lookups=0,last_hits=0,indexed_hits=0,probes=0,plan_misses=0,evictions=0;};
  const Stats& stats() const {return stats_;}
  const Value* Find(const Shaders& shaders,const Stamp& stamp,const uint32_t* fetch) const {
    ++stats_.lookups;
    if(last_<kCapacity && Matches(entries_[last_],shaders,stamp,fetch)) {
      ++stats_.last_hits;Touch(last_);return &values_[last_];
    }
    const uint64_t shader_hash=ShaderHash(shaders);
    const auto& plan=plans_[shader_hash&(PlanSlots-1)];
    if(!plan.valid || plan.shaders!=shaders){++stats_.plan_misses;return nullptr;}
    const uint64_t hash=FetchHash(shader_hash,fetch,plan.mask);
    const size_t first=(hash&(Sets-1))*Ways;
    for(size_t i=first;i<first+Ways;++i) {
      if(i==last_)continue;
      ++stats_.probes;
      const auto& entry=entries_[i];
      if(entry.hash==hash && entry.mask==plan.mask && Matches(entry,shaders,stamp,fetch)) {
        ++stats_.indexed_hits;Touch(i);return &values_[i];
      }
    }
    return nullptr;
  }
  Value* Find(const Shaders& shaders,const Stamp& stamp,const uint32_t* fetch) {
    return const_cast<Value*>(static_cast<const PreviousDrawTextures&>(*this).Find(shaders,stamp,fetch));
  }
  void Store(const Shaders& shaders,const Stamp& before,const Stamp& after,
             const uint32_t* fetch,uint32_t mask,const Value& value,bool eligible) {
    if(!eligible || before!=after)return;
    const uint64_t shader_hash=ShaderHash(shaders);
    auto& plan=plans_[shader_hash&(PlanSlots-1)];
    // A compiled shader pair has an immutable used-fetch mask. Be defensive
    // if its owner explicitly republishes a different plan under the same key.
    if(plan.valid && plan.shaders==shaders && plan.mask!=mask)
      for(auto& entry:entries_)if(entry.valid && entry.shaders==shaders)entry.valid=false;
    plan={shaders,mask,true};
    const uint64_t hash=FetchHash(shader_hash,fetch,mask);
    const size_t first=(hash&(Sets-1))*Ways;
    size_t target=kCapacity,stale=kCapacity,oldest=first;
    for(size_t i=first;i<first+Ways;++i) {
      const auto& entry=entries_[i];
      if(entry.hash==hash && entry.mask==mask && Matches(entry,shaders,after,fetch)) {target=i;break;}
      if(stale==kCapacity && (!entry.valid || entry.stamp!=after))stale=i;
      if(ages_[i]<ages_[oldest])oldest=i;
    }
    if(target==kCapacity) {
      target=stale!=kCapacity?stale:oldest;
      if(stale==kCapacity)++stats_.evictions;
    }
    auto& entry=entries_[target];
    entry.shaders=shaders;entry.stamp=after;entry.mask=mask;entry.hash=hash;
    values_[target]=value;
    for(uint32_t bits=mask;bits;bits&=bits-1) {
      const uint32_t at=std::countr_zero(bits)*6;
      std::memcpy(entry.fetch.data()+at,fetch+at,6*sizeof(uint32_t));
    }
    entry.valid=true;Touch(target);
  }
  // A failed lifetime/dependency check rejects just that set. Ordinary misses
  // must not call Invalidate(), or alternating materials could never survive.
  void Reject(const Value* value) {
    if(last_<kCapacity && value==&values_[last_]) {entries_[last_].valid=false;last_=kCapacity;return;}
    for(size_t i=0;i<kCapacity;++i)if(value==&values_[i]) {entries_[i].valid=false;return;}
  }
  void Invalidate() {
    for(auto& entry:entries_)entry.valid=false;
    last_=kCapacity;
  }
 private:
  struct Entry {
    Shaders shaders{};Stamp stamp{};
    std::array<uint32_t,192> fetch{};
    uint64_t hash=0;uint32_t mask=0;bool valid=false;
  };
  struct Plan {Shaders shaders{};uint32_t mask=0;bool valid=false;};
  static uint64_t Mix(uint64_t h,uint64_t word) {
    return (h^(h>>31)^word)*0xBF58476D1CE4E5B9ull;
  }
  static uint64_t ShaderHash(const Shaders& shaders) {
    uint64_t h=0x9E3779B97F4A7C15ull;
    for(const auto shader:shaders)h=Mix(h,uint64_t(shader));
    return h^(h>>32);
  }
  static uint64_t FetchHash(uint64_t h,const uint32_t* fetch,uint32_t mask) {
    h=Mix(h,mask);
    for(uint32_t bits=mask;bits;bits&=bits-1) {
      const uint32_t at=std::countr_zero(bits)*6;
      for(uint32_t i=0;i<6;i+=2)h=Mix(h,uint64_t(fetch[at+i])|(uint64_t(fetch[at+i+1])<<32));
    }
    return h^(h>>32);
  }
  static bool Matches(const Entry& entry,const Shaders& shaders,const Stamp& stamp,const uint32_t* fetch) {
    if(!entry.valid || entry.shaders!=shaders || entry.stamp!=stamp)return false;
    for(uint32_t bits=entry.mask;bits;bits&=bits-1) {
      const uint32_t at=std::countr_zero(bits)*6;
      if(std::memcmp(fetch+at,entry.fetch.data()+at,6*sizeof(uint32_t)))return false;
    }
    return true;
  }
  void Touch(size_t i) const {
    if(!++clock_){ages_.fill(0);clock_=1;}
    ages_[i]=clock_;last_=i;
  }
  std::array<Entry,kCapacity> entries_{};
  std::array<Value,kCapacity> values_{};
  std::array<Plan,PlanSlots> plans_{};
  mutable std::array<uint64_t,kCapacity> ages_{};
  mutable uint64_t clock_=0;
  mutable size_t last_=kCapacity;
  mutable Stats stats_{};
};
} // namespace fear_nx::deko
