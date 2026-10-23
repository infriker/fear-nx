// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace fear_nx::deko {

// Only NEW exclusive resident allocations, pinned by SharedGeometryCache.
// Snapshot each payload immediately; writes may be reordered until submission
// because no submitted work can read these ranges. Never read GPU memory or
// fill a gap: older live allocations may occupy even a single missing block.
class DeferredIndexUploadBatch {
 public:
  static constexpr uint32_t kCapacity=64u<<10;
  static constexpr uint32_t kEntries=kCapacity/64;
  struct Stats {
    uint64_t flushes=0,capacity_flushes=0,payloads=0,writes=0,merged=0;
    uint64_t sorted_flushes=0,assembled_bytes=0;
  };
  DeferredIndexUploadBatch()=default;
  DeferredIndexUploadBatch(const DeferredIndexUploadBatch&)=delete;
  DeferredIndexUploadBatch& operator=(const DeferredIndexUploadBatch&)=delete;
  template <typename Write>
  bool Append(uint8_t* destination,uint64_t gpu,std::span<const uint8_t> source,
              uint32_t owned_bytes,Write&& write) {
    const auto cpu=reinterpret_cast<uintptr_t>(destination);
    if(!destination || source.empty() || source.size()>owned_bytes ||
       !owned_bytes || owned_bytes>kCapacity || (owned_bytes&63) ||
       (cpu&63) || (gpu&63) ||
       cpu>std::numeric_limits<uintptr_t>::max()-owned_bytes ||
       gpu>UINT64_MAX-owned_bytes)return false;
    if(owned_bytes>kCapacity-used_ || count_==kEntries) {
      ++stats_.capacity_flushes;Flush(write);
    }
    if(count_ && reinterpret_cast<uintptr_t>(entries_[count_-1].cpu)>cpu) sorted_=false;
    entries_[count_++]={destination,gpu,used_,owned_bytes};
    std::memcpy(captured_.data()+used_,source.data(),source.size());
    std::memset(captured_.data()+used_+source.size(),0,owned_bytes-source.size());
    used_+=owned_bytes;++stats_.payloads;
    return true;
  }
  template <typename Write>
  void Flush(Write&& write) {
    if(!count_)return;
    if(!sorted_) {
      std::sort(entries_.begin(),entries_.begin()+count_,[](const Entry& a,const Entry& b) {
        return reinterpret_cast<uintptr_t>(a.cpu)<reinterpret_cast<uintptr_t>(b.cpu);
      });
      ++stats_.sorted_flushes;
    }
    uint32_t first=0;
    while(first<count_) {
      const auto& start=entries_[first];
      uint32_t end=first+1,bytes=start.bytes;
      bool contiguous_capture=true;
      while(end<count_ &&
            reinterpret_cast<uintptr_t>(entries_[end].cpu)==reinterpret_cast<uintptr_t>(start.cpu)+bytes &&
            entries_[end].gpu==start.gpu+bytes) {
        const auto& entry=entries_[end++];
        contiguous_capture=contiguous_capture && entry.offset==start.offset+bytes;
        bytes+=entry.bytes;
      }
      // A singleton or an already contiguous capture is the final upload
      // payload. Only genuinely reordered runs need the second RAM copy.
      const uint8_t* source=captured_.data()+start.offset;
      if(!contiguous_capture) {
        uint32_t offset=0;
        for(uint32_t i=first;i<end;++i) {
          const auto& entry=entries_[i];
          std::memcpy(joined_.data()+offset,captured_.data()+entry.offset,entry.bytes);
          offset+=entry.bytes;
        }
        source=joined_.data();stats_.assembled_bytes+=bytes;
      }
      write(start.cpu,source,bytes);
      ++stats_.writes;stats_.merged+=end-first-1;first=end;
    }
    ++stats_.flushes;count_=used_=0;sorted_=true;
  }
  bool pending() const {return count_!=0;}
  const Stats& stats() const {return stats_;}
 private:
  struct Entry {uint8_t* cpu;uint64_t gpu;uint32_t offset,bytes;};
  std::array<Entry,kEntries> entries_{};
  alignas(16) std::array<uint8_t,kCapacity> captured_{},joined_{};
  uint32_t count_=0,used_=0;
  bool sorted_=true;
  Stats stats_{};
};
} // namespace fear_nx::deko
