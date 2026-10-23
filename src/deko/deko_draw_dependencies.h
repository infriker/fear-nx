// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <map>
#include <span>
#include <vector>
#include "deko_cpu_compare.h"

namespace fear_nx::deko {
// Bounded work-local metadata: collisions compare the entire exact key.
// Keep at most half the flat table occupied, and reset by generation rather
// than allocating/freeing tree nodes at each GPU work boundary. GPU payloads
// are owned elsewhere; declining a cache insertion never discards a draw.
template<class Value, size_t Buckets=8192> class LocalVertexLookup {
 public:
  using Key=std::array<uint64_t,3>;
  static_assert(Buckets && !(Buckets&(Buckets-1)));
  static constexpr size_t kMaxEntries=Buckets;
  static constexpr size_t kTableSize=2*Buckets;
  const Value* Find(const Key& key) {
    size_t slot=Bucket(key);
    auto& first=entries_[slot];
    if(first.epoch==epoch_ && first.key==key) {++fast_hits;return &first.value;}
    // Compatibility counter: formerly a tree lookup, now a secondary probe
    // or an empty first bucket. It is not the number of slots examined.
    ++map_lookups;
    if(first.epoch!=epoch_)return nullptr;
    slot=(slot+1)&(kTableSize-1);
    for(size_t probes=1;probes<kTableSize;++probes) {
      const auto& entry=entries_[slot];
      if(entry.epoch!=epoch_)return nullptr;
      if(entry.key==key)return &entry.value;
      slot=(slot+1)&(kTableSize-1);
    }
    return nullptr;
  }
  void Store(const Key& key,const Value& value) {
    size_t slot=Bucket(key);
    for(size_t probes=0;probes<kTableSize;++probes) {
      auto& entry=entries_[slot];
      if(entry.epoch!=epoch_) {
        if(size_==kMaxEntries)return;
        entry.key=key;entry.value=value;entry.epoch=epoch_;++size_;return;
      }
      if(entry.key==key){entry.value=value;return;}
      slot=(slot+1)&(kTableSize-1);
    }
  }
  size_t size() const {return size_;}
  void Clear() {
    size_=0;
    if(!++epoch_) {for(auto& entry:entries_)entry.epoch=0;epoch_=1;}
  }
  static size_t Bucket(const Key& key) {
    uint64_t h=key[2]^(key[0]*0x9E3779B185EBCA87ULL)^(key[1]*0xC2B2AE3D27D4EB4FULL);
    h^=h>>32;return size_t(h)&(kTableSize-1);
  }
  uint64_t fast_hits=0,map_lookups=0;
 private:
  struct Entry {Key key{};Value value{};uint64_t epoch=0;};
  std::array<Entry,kTableSize> entries_{};
  size_t size_=0;
  uint64_t epoch_=1;
};

struct DrawSetupIdentity {
  uint64_t state=0,targets=0;
  uintptr_t vs=0,ps=0;
  uint32_t initiator=0;
  bool depth24=false,half_pixel=false;
  bool operator==(const DrawSetupIdentity&) const=default;
};

// System constants have no guest register-write callback. Compare their CPU
// block once, then share the per-register versions with every packed layout.
class CpuRegisterVersions {
 public:
  void Update(std::span<const uint8_t> data) {
    if(bytes_.size()!=data.size()) {
      bytes_.assign(data.begin(),data.end());versions_.assign((data.size()+15)/16,++clock_);return;
    }
    for(size_t i=0;i<versions_.size();++i) {
      const size_t offset=i*16,bytes=std::min(size_t(16),data.size()-offset);
      if(!EqualCpuBytes(bytes_.data()+offset,data.data()+offset,bytes)) {
        std::memcpy(bytes_.data()+offset,data.data()+offset,bytes);versions_[i]=++clock_;
      }
    }
  }
  std::span<const uint64_t> versions() const {return versions_;}
  uint64_t generation() const {return clock_;}
 private:
  std::vector<uint8_t> bytes_;
  std::vector<uint64_t> versions_;
  uint64_t clock_=0;
};

// Descriptor slots may be recycled. Compare their allocation identity rather
// than pointers or a global image epoch, before reading cached texture nodes.
class ImageDescriptorVersions {
 public:
  uint64_t NewImage() {return ++clock_;}
  void Bind(uint32_t slot,uint64_t image,uint64_t version=1) {
    if(slot>=images_.size())images_.resize(size_t(slot)+1);
    images_[slot]={image,version};
  }
  void Retire(uint32_t slot) {if(slot<images_.size())images_[slot]={};}
  bool Alive(uint32_t slot,uint64_t image,uint64_t version=1) const {
    return image && slot<images_.size() && images_[slot]==std::array<uint64_t,2>{image,version};
  }
  uint64_t Image(uint32_t slot) const {return slot<images_.size()?images_[slot][0]:0;}
 private:
  uint64_t clock_=0;
  std::vector<std::array<uint64_t,2>> images_;
};
} // namespace fear_nx::deko
