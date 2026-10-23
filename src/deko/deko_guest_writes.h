// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>

namespace fear_nx::deko {
// Ring-thread journal of EVENT_WRITE_EXT's exact 12-byte physical writes.
// Updating a repeated address is O(log N); no resource/cache scan on a write.
// Equal-length intervals let queries start at begin-11, including overlaps
// whose start precedes the resource. Keep older overlapping addresses: a newer
// partial overwrite must not hide the unchanged tail of an older write.
// Capacity eviction drops only the oldest last-write serial. Recent leases
// retain exact history instead of all expiring when the address table fills.
class GuestMemoryWrites {
 public:
  static constexpr uint32_t kExtentBytes=12;
  static constexpr size_t kMaxAddresses=65536;
  static constexpr uint64_t kRecentQueryWrites=8;
  struct Stats {uint64_t writes=0,queries=0,overlaps=0,history_resets=0,history_misses=0,history_evictions=0,recent_queries=0;};
  GuestMemoryWrites()=default;
  // Intrusive recency links point into this journal's stable map nodes.
  GuestMemoryWrites(const GuestMemoryWrites&)=delete;
  GuestMemoryWrites& operator=(const GuestMemoryWrites&)=delete;
  GuestMemoryWrites(GuestMemoryWrites&&)=delete;
  GuestMemoryWrites& operator=(GuestMemoryWrites&&)=delete;
  uint64_t version() const {return version_;}
  size_t size() const {return writes_.size();}
  const Stats& stats() const {return stats_;}
  void RecordExtent(uint32_t address) {
    address&=0x1FFFFFFFu;
    ++version_;++stats_.writes;
    auto found=writes_.find(address);
    if(found!=writes_.end()) {
      auto& entry=found->second;entry.version=version_;
      if(newest_!=&entry){Unlink(entry);Append(entry);}
      return;
    }
    if(writes_.size()>=kMaxAddresses) {
      // The list is ordered by LAST write, so a repeated address remains
      // available to leases older than that repeat. Only serials strictly
      // before the removed write lose provably complete history.
      const uint32_t oldest_address=oldest_->address;
      if(history_floor_<oldest_->version)history_floor_=oldest_->version;
      Unlink(*oldest_);
      auto node=writes_.extract(oldest_address);
      node.key()=address;node.mapped()=Entry{version_,nullptr,nullptr,address};
      auto inserted=writes_.insert(std::move(node));Append(inserted.position->second);
      ++stats_.history_evictions;return;
    }
    auto [inserted,_]=writes_.emplace(address,Entry{version_,nullptr,nullptr,address});
    Append(inserted->second);
  }
  bool ChangedSince(uint32_t address,uint32_t bytes,uint64_t since) const {
    if(!bytes || since==version_)return false;
    ++stats_.queries;
    // Bounded-history overflow must never silently validate an old lease.
    // It forces one fresh check for old leases, without flushing GPU storage.
    if(since<history_floor_){++stats_.history_misses;return true;}
    const uint64_t end=uint64_t(address)+bytes;
    if(writes_.empty() || end<=writes_.begin()->first ||
        uint64_t(address)>=uint64_t(writes_.rbegin()->first)+kExtentBytes)return false;
    if(since<version_ && version_-since<=kRecentQueryWrites) {
      // After the history guard every newer write is represented. Walking
      // last-write order visits at most eight entries, even if this resource
      // spans thousands of older addresses. Repeats reduce that count.
      ++stats_.recent_queries;
      for(const auto* entry=newest_;entry && entry->version>since;entry=entry->older) {
        if(uint64_t(entry->address)<end && uint64_t(entry->address)+kExtentBytes>address) {
          ++stats_.overlaps;return true;
        }
      }
      return false;
    }
    const uint32_t first=address>=kExtentBytes?address-(kExtentBytes-1):0;
    for(auto it=writes_.lower_bound(first);it!=writes_.end() && it->first<end;++it) {
      if(it->second.version>since){++stats_.overlaps;return true;}
    }
    return false;
  }
 private:
  struct Entry {
    uint64_t version;
    Entry* older;
    Entry* newer;
    uint32_t address;
  };
  void Unlink(Entry& entry) {
    if(entry.older)entry.older->newer=entry.newer;else oldest_=entry.newer;
    if(entry.newer)entry.newer->older=entry.older;else newest_=entry.older;
  }
  void Append(Entry& entry) {
    entry.older=newest_;entry.newer=nullptr;
    if(newest_)newest_->newer=&entry;else oldest_=&entry;
    newest_=&entry;
  }
  std::map<uint32_t,Entry> writes_;
  Entry *oldest_=nullptr,*newest_=nullptr;
  uint64_t version_=0,history_floor_=0;
  mutable Stats stats_{};
};

// Two RAM spans cover a texture's base and mip allocations. GPU-only images
// and null bindings have empty spans and retain their image/version lifetime.
struct GuestMemoryLease {
  // Trivial storage keeps unused draw-dependency tails uninitialized. Owners
  // that need an empty lease value-initialize it with {}.
  uint32_t base,base_bytes,mips,mip_bytes;
  mutable uint64_t version;
  bool Current(const GuestMemoryWrites& writes) const {
    if(version==writes.version())return true;
    if(writes.ChangedSince(base,base_bytes,version) ||
        writes.ChangedSince(mips,mip_bytes,version))return false;
    version=writes.version();return true;
  }
};
} // namespace fear_nx::deko
