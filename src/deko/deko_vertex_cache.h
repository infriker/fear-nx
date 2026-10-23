// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <list>
#include <limits>
#include <unordered_map>
#include <vector>

namespace fear_nx::deko {

// CPU-only metadata for immutable vertex copies stored in the prefix of each
// Nucleo upload-ring slot. BeginWork must be called only after Nucleo has
// waited for the selected slot's previous fence. Each slot has its own cache;
// offsets are relative to that slot's upload-buffer base.
class PersistentVertexCache {
 public:
  static constexpr uint32_t kPageBytes = 4096;
  static constexpr uint32_t kReservedPrefixBytes = kPageBytes;
  static constexpr uint32_t kDefaultCapacityBytes = 16u << 20;

  struct Result {
    uint32_t offset = 0;
    uint32_t bytes_to_copy = 0;
    bool hit = false;
    bool valid = false;
  };

  struct Stats {
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t copied_bytes = 0;
    uint64_t evictions = 0;
    uint64_t capacity_misses = 0;
    uint64_t candidate_evictions = 0;
  };

  explicit PersistentVertexCache(uint32_t capacity_bytes = kDefaultCapacityBytes)
      : capacity_bytes_(capacity_bytes / kPageBytes * kPageBytes),
        page_count_(capacity_bytes_ / kPageBytes) {}

  // The selected slot's previous GPU work must have passed its fence before
  // this call. Entries in other slots may still be in flight and are never
  // modified while this slot is active.
  void BeginWork(uint64_t work, uint32_t slot_count) {
    if (!slot_count) {
      slots_.clear();
      active_ = false;
      return;
    }
    if (slots_.size() != slot_count) {
      slots_.clear();
      slots_.resize(slot_count);
      for (Slot& slot : slots_) slot.used_pages.resize(page_count_, 0);
    }
    work_ = work;
    slot_index_ = static_cast<uint32_t>(work % slot_count);
    active_ = true;
    active_slot_discarded_ = false;
  }

  // Call after BeginWork has selected a fence-safe slot if the renderer must
  // use the full ordinary upload ring for this work. Its prefix may then be
  // overwritten, so forget only this slot and disable cache allocations until
  // the next BeginWork. Cumulative statistics are preserved.
  void DiscardActiveSlot() {
    if (!active_) return;
    Slot& slot = slots_[slot_index_];
    slot.entries.clear();
    slot.candidates.clear();
    slot.candidate_order.clear();
    std::fill(slot.used_pages.begin(), slot.used_pages.end(), uint8_t{0});
    active_slot_discarded_ = true;
  }

  // The first identical observation remains in the ordinary Nucleo upload
  // ring. The second observation admits an immutable copy in this slot's
  // prefix. On a valid miss, the caller must memcpy bytes_to_copy bytes into
  // the returned offset before recording the draw. Large or temporarily
  // unallocatable ranges return invalid and use the normal upload ring.
  Result FindOrAllocate(uint32_t address, uint32_t bytes, uint64_t content_hash,
                        uint32_t minimum_offset = 0) {
    if (!active_) {
      return {};
    }
    if (active_slot_discarded_) {
      ++stats_.misses;
      return {};
    }
    if (bytes == 0 || page_count_ <= 1) {
      if (active_) ++stats_.misses;
      return {};
    }

    Slot& slot = slots_[slot_index_];
    const Key key{address, bytes, content_hash};
    auto found = slot.entries.find(key);
    if (found != slot.entries.end()) {
      // A physical slice may be referenced through another descriptor base.
      // Its old payload stays immutable; a caller needing a higher placement
      // must use an ordinary upload instead of encoding a negative fetch base.
      if (found->second.offset < minimum_offset) {
        ++stats_.misses;
        ++stats_.capacity_misses;
        return {};
      }
      found->second.last_used_work = work_;
      ++stats_.hits;
      return {found->second.offset, 0, true, true};
    }
    ++stats_.misses;

    const uint64_t page_bytes = (uint64_t(bytes) + kPageBytes - 1) / kPageBytes;
    const uint64_t minimum_page = std::max<uint64_t>(1,
        (uint64_t(minimum_offset) + kPageBytes - 1) / kPageBytes);
    if (minimum_page + page_bytes > page_count_) {
      ++stats_.capacity_misses;
      return {};
    }

    auto candidate = slot.candidates.find(key);
    if (candidate == slot.candidates.end()) {
      AddCandidate(slot, key);
      return {};
    }
    slot.candidate_order.splice(slot.candidate_order.end(), slot.candidate_order,
                                candidate->second);

    uint32_t first_page = 0;
    if (!FindContiguousPages(slot, static_cast<uint32_t>(page_bytes), uint32_t(minimum_page), first_page)) {
      while (EvictLeastRecentlyUsed(slot)) {
        if (FindContiguousPages(slot, static_cast<uint32_t>(page_bytes), uint32_t(minimum_page), first_page)) break;
      }
      if (!FindContiguousPages(slot, static_cast<uint32_t>(page_bytes), uint32_t(minimum_page), first_page)) {
        ++stats_.capacity_misses;
        return {};
      }
    }

    for (uint32_t page = first_page; page < first_page + page_bytes; ++page) {
      slot.used_pages[page] = 1;
    }
    const uint32_t offset = first_page * kPageBytes;
    slot.entries.emplace(key, Entry{offset, static_cast<uint32_t>(page_bytes), work_});
    slot.candidate_order.erase(candidate->second);
    slot.candidates.erase(candidate);
    stats_.copied_bytes += bytes;
    return {offset, bytes, false, true};
  }

  const Stats& stats() const { return stats_; }
  uint32_t capacity_bytes() const { return capacity_bytes_; }
  uint32_t active_slot() const { return active_ ? slot_index_ : UINT32_MAX; }

 private:
  struct Key {
    uint32_t address;
    uint32_t bytes;
    uint64_t content_hash;

    bool operator==(const Key& other) const {
      return address == other.address && bytes == other.bytes &&
             content_hash == other.content_hash;
    }
  };

  struct KeyHasher {
    size_t operator()(const Key& key) const {
      uint64_t value = key.content_hash ^ (uint64_t(key.address) << 32) ^ key.bytes;
      value ^= value >> 30;
      value *= UINT64_C(0xBF58476D1CE4E5B9);
      value ^= value >> 27;
      value *= UINT64_C(0x94D049BB133111EB);
      value ^= value >> 31;
      return static_cast<size_t>(value);
    }
  };

  struct Entry {
    uint32_t offset;
    uint32_t page_count;
    uint64_t last_used_work;
  };

  struct Slot {
    Slot() = default;
    Slot(Slot&&) = default;
    Slot& operator=(Slot&&) = default;
    Slot(const Slot&) = delete;
    Slot& operator=(const Slot&) = delete;
    std::unordered_map<Key, Entry, KeyHasher> entries;
    std::list<Key> candidate_order;
    std::unordered_map<Key, std::list<Key>::iterator, KeyHasher> candidates;
    std::vector<uint8_t> used_pages;
  };

  void AddCandidate(Slot& slot, const Key& key) {
    const size_t max_candidates = std::max<size_t>(64, size_t(page_count_) * 2);
    if (slot.candidates.size() >= max_candidates) {
      // Constant-time LRU maintenance, independent of the 8192-entry limit.
      // This retires CPU admission metadata only, never a live GPU payload.
      slot.candidates.erase(slot.candidate_order.front());
      slot.candidate_order.pop_front();
      ++stats_.candidate_evictions;
    }
    slot.candidate_order.push_back(key);
    slot.candidates.emplace(key, std::prev(slot.candidate_order.end()));
  }

  bool FindContiguousPages(const Slot& slot, uint32_t pages, uint32_t minimum_page,
                           uint32_t& first_page) const {
    if (pages == 0 || page_count_ <= 1) return false;
    uint32_t run_start = minimum_page;  // Page zero is reserved by the renderer.
    uint32_t run_length = 0;
    for (uint32_t page = minimum_page; page < page_count_; ++page) {
      if (!slot.used_pages[page]) {
        if (!run_length) run_start = page;
        if (++run_length == pages) {
          first_page = run_start;
          return true;
        }
      } else {
        run_length = 0;
      }
    }
    return false;
  }

  bool EvictLeastRecentlyUsed(Slot& slot) {
    auto victim = slot.entries.end();
    for (auto it = slot.entries.begin(); it != slot.entries.end(); ++it) {
      // A range used earlier in this open work may already be referenced by
      // commands in the current command buffer. Never recycle it mid-work.
      if (it->second.last_used_work == work_) continue;
      if (victim == slot.entries.end() ||
          it->second.last_used_work < victim->second.last_used_work) {
        victim = it;
      }
    }
    if (victim == slot.entries.end()) return false;
    const uint32_t first_page = victim->second.offset / kPageBytes;
    for (uint32_t page = first_page; page < first_page + victim->second.page_count; ++page) {
      slot.used_pages[page] = 0;
    }
    slot.entries.erase(victim);
    ++stats_.evictions;
    return true;
  }

  uint32_t capacity_bytes_ = 0;
  uint32_t page_count_ = 0;
  uint64_t work_ = 0;
  uint32_t slot_index_ = 0;
  bool active_ = false;
  bool active_slot_discarded_ = false;
  Stats stats_{};
  std::vector<Slot> slots_;
};

}  // namespace fear_nx::deko
