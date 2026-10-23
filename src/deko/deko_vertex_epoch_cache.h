// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include "deko_guest_writes.h"

namespace fear_nx::deko {

// NFSMW-style work-local deduplication, separate from content-keyed residency.
// The ring processor must publish every guest synchronization/write boundary.
// A hit owns an immutable upload in this work; it does NOT extend a resident
// allocation's GPU lifetime beyond that already pinned by the original lookup.
// No content hash is needed between guest synchronization boundaries. This is
// the guest GPU ownership contract, not protection against arbitrary CPU races.
template<class Value, size_t Buckets=16384> class VertexEpochCache {
 public:
  static_assert(Buckets && !(Buckets & (Buckets-1)));
  struct Stats { uint64_t lookups=0,hits=0,bytes=0,collisions=0,invalidations=0; };
  void BeginEpoch(uint64_t work,uint64_t frame,uint64_t ownership,
                  uint64_t validation,uint64_t guards) {
    // Ownership can end at a GPU coherency wait without expiring the content
    // validation cache. Full validation/guard changes still end local reuse.
    const std::array<uint64_t,5> identity{work,frame,ownership,validation,guards};
    if(initialized_ && identity==identity_)return;
    identity_=identity;initialized_=true;
    if(!++generation_){entries_={};generation_=1;}
    ++stats_.invalidations;
  }
  const Value* Find(uint32_t address,uint32_t bytes,uint32_t endian,uint64_t minimum_gpu,
                    const GuestMemoryWrites& writes) {
    ++stats_.lookups;
    auto& entry=entries_[Bucket(address,bytes,endian)];
    if(entry.generation!=generation_ || entry.address!=address ||
       entry.bytes!=bytes || entry.endian!=endian || entry.value.gpu<minimum_gpu)return nullptr;
    if(writes.ChangedSince(address,bytes,entry.write_version))return nullptr;
    entry.write_version=writes.version();
    ++stats_.hits;stats_.bytes+=bytes;return &entry.value;
  }
  void Store(uint32_t address,uint32_t bytes,uint32_t endian,const Value& value,
             const GuestMemoryWrites& writes) {
    auto& entry=entries_[Bucket(address,bytes,endian)];
    if(entry.generation==generation_ && (entry.address!=address ||
       entry.bytes!=bytes || entry.endian!=endian))++stats_.collisions;
    entry={value,generation_,writes.version(),address,bytes,endian};
  }
  const Stats& stats() const {return stats_;}
 private:
  static size_t Bucket(uint32_t address,uint32_t bytes,uint32_t endian) {
    uint64_t h=(uint64_t(address)<<2)^endian^(uint64_t(bytes)*0x9E3779B97F4A7C15ULL);
    h^=h>>30;h*=0xBF58476D1CE4E5B9ULL;h^=h>>27;
    return size_t(h)&(Buckets-1);
  }
  struct Entry { Value value{};uint64_t generation=0,write_version=0;uint32_t address=0,bytes=0,endian=0; };
  std::array<Entry,Buckets> entries_{};
  std::array<uint64_t,5> identity_{};
  uint64_t generation_=0;
  bool initialized_=false;
  Stats stats_{};
};
} // namespace fear_nx::deko
