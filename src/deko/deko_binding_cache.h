// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace fear_nx::deko {
// Bucket selection only, never a content fingerprint. Exact words and resource
// generations below still decide reuse, including deliberate hash collisions.
template<size_t N> inline uint64_t BindingBucketHash(const std::array<uint32_t,N>& words) {
  uint64_t h=0x9E3779B97F4A7C15ull;
  for(size_t i=0;i<N;i+=2) {
    const uint64_t pair=uint64_t(words[i])|(i+1<N?uint64_t(words[i+1])<<32:0);
    h=(h^(h>>31)^pair)*0xBF58476D1CE4E5B9ull;
  }
  return h^(h>>32);
}
// Bounded direct-mapped front cache. Hashes choose a bucket, never establish
// identity. A stamp invalidates references before their resources can retire.
template <typename Value, size_t KeyWords, size_t Slots = 1024, size_t Ways = 1>
class BindingMemo {
 public:
  static_assert(Slots && Ways);
  using Key = std::array<uint32_t, KeyWords>;
  using Stamp = std::array<uint64_t, 3>; // validation epoch, guard misses, images
  const Value* Find(uint64_t hash, const Key& key, const Stamp& stamp) const {
    const size_t first=(hash%Slots)*Ways;
    for(size_t i=first;i<first+Ways;++i) {
      const auto& entry=entries_[i];
      if(entry.valid && entry.stamp==stamp && entry.key==key) {
        recent_[hash%Slots]=i-first;return &entry.value;
      }
    }
    return nullptr;
  }
  void Store(uint64_t hash, const Key& key, const Stamp& stamp, const Value& value) {
    const size_t bucket=hash%Slots,first=bucket*Ways;
    size_t target=Ways;
    for(size_t i=0;i<Ways;++i) {
      const auto& entry=entries_[first+i];
      if(entry.valid && entry.stamp==stamp && entry.key==key){target=i;break;}
      if(target==Ways && (!entry.valid || entry.stamp!=stamp))target=i;
    }
    if(target==Ways) {
      target=next_[bucket];
      if(Ways>1 && target==recent_[bucket])target=(target+1)%Ways;
    }
    next_[bucket]=(target+1)%Ways;recent_[bucket]=target;
    entries_[first+target] = {key, stamp, value, true};
  }
 private:
  struct Entry { Key key{}; Stamp stamp{}; Value value{}; bool valid = false; };
  std::array<Entry, Slots*Ways> entries_{};
  std::array<size_t,Slots> next_{};
  mutable std::array<size_t,Slots> recent_{};
};
} // namespace fear_nx::deko
