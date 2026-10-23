// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Keep XXH3's short-range implementation inline. Vertex buffers are commonly
// fingerprinted as many independent cache lines, where an out-of-line call
// per line costs more than the hash itself.
#ifndef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#endif
#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif
#include <xxhash.h>
#include "deko_guest_writes.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <unordered_map>

namespace fear_nx::deko {

enum class ValidationKind : uint8_t { kGeometry, kTexture };

class ValidationCache {
 public:
  static constexpr size_t kLineBytes = 64;
  static constexpr uint32_t kPhaseCount = 8;
  static constexpr size_t kSmallRangeBytes = 1024;
  static constexpr size_t kMaxEntries = 8192;
  static constexpr size_t kMaxQuarantinedRanges = 8192;
  static constexpr uint64_t kGuardChecks = 20000;
  static constexpr uint64_t kTextureMaxInterval = 32;
  static constexpr uint64_t kVolatileFrames = 64;

  struct Result {
    uint64_t hash = 0;
    uint64_t bytes_hashed = 0;
    bool bypass_cache = false;
    bool guard_miss = false;
    bool volatile_geometry = false;
    bool unhashed_geometry = false;
  };

  struct Stats {
    uint64_t lookups = 0;
    uint64_t hits = 0;
    uint64_t full_checks = 0;
    uint64_t full_bytes = 0;
    uint64_t sample_bytes = 0;
    uint64_t sample_changes = 0;
    uint64_t guard_checks = 0;
    uint64_t guard_misses = 0;
    uint64_t invalidations = 0;
    uint64_t sampled_checks = 0;
    uint64_t training_remaining = kGuardChecks;
    bool sampling_disabled = false;
    uint64_t quarantined_ranges = 0;
    uint64_t strict_lookups = 0;
    uint64_t strict_bytes = 0;
    uint64_t quarantine_overflows = 0;
    uint64_t epoch_hits = 0;
    uint64_t volatile_bypasses = 0;
    uint64_t volatile_full_bytes = 0;
    uint64_t volatile_exact_checks = 0;
    uint64_t texture_deferred = 0;
    uint64_t strict_epoch_hits=0,volatile_unhashed_calls=0,volatile_unhashed_bytes=0;
  };

  void SetEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    HardInvalidate();
  }

  bool enabled() const { return enabled_; }
  void SetGeometryEpochChecks(bool enabled) {
    if(geometry_epoch_checks_!=enabled){geometry_epoch_checks_=enabled;HardInvalidate();}
  }

  // A texture binding may skip repeated lookup only within this validation
  // epoch. A guard miss anywhere invalidates leases too: it can quarantine an
  // overlapping range without otherwise advancing the epoch.
  uint64_t memo_epoch() const { return epoch_; }
  GuestMemoryWrites& guest_writes() {return guest_writes_;}
  const GuestMemoryWrites& guest_writes() const {return guest_writes_;}
  bool TextureRangeCanReuseBinding(uint32_t address, size_t size) const {
    return !size || (enabled_ && !stats_.sampling_disabled &&
        size > kSmallRangeBytes && RangeCanBeMemoized(address, size) &&
        !IsQuarantined(address, size));
  }

  // A frame change is a content epoch boundary. Work/progress notifications
  // may add more boundaries through Revalidate() without resetting training.
  void BeginFrame(uint64_t frame) {
    if (frame_ != frame) {
      frame_ = frame;
      Revalidate();
    }
  }

  void Revalidate() {
    ++epoch_;
    ++stats_.invalidations;
  }

  // Kept for existing renderer call sites; this is a soft invalidation and
  // intentionally preserves both entries and global training progress.
  void Invalidate() { Revalidate(); }

  void HardInvalidate() {
    entries_.clear();
    ++epoch_;
    ++stats_.invalidations;
  }

  Result Hash(const uint8_t* data, uint32_t address, size_t size, uint64_t seed = 0,
              ValidationKind kind = ValidationKind::kGeometry) {
    ++stats_.lookups;
    if (!data && size != 0) return {};

    if (!RangeCanBeMemoized(address, size)) {
      return FullRaw(data, size, seed);
    }

    if(enabled_ && geometry_epoch_checks_ && kind==ValidationKind::kGeometry &&
        (size<=kSmallRangeBytes || stats_.sampling_disabled || IsQuarantined(address,size))) {
      const Key key{address,size,seed,kind};
      auto found=FindCurrent(key);
      if(found!=entries_.end() && found->second.exact_epoch &&
          found->second.last_validated_epoch==epoch_ && found->second.guard_generation==stats_.guard_misses) {
        ++stats_.hits;++stats_.epoch_hits;++stats_.strict_epoch_hits;
        return {found->second.hash,0,false};
      }
      const auto result=FullRaw(data,size,seed);
      if(IsQuarantined(address,size)){++stats_.strict_lookups;stats_.strict_bytes+=size;}
      if(entries_.size()>=kMaxEntries){entries_.clear();++epoch_;++stats_.invalidations;}
      auto& entry=entries_[key];entry.hash=result.hash;entry.exact_epoch=true;
      entry.write_version=guest_writes_.version();
      entry.last_validated_epoch=epoch_;entry.guard_generation=stats_.guard_misses;
      return result;
    }
    // Small ranges are cheap and remain exact per use. Sampling them would
    // add cache bookkeeping while making their freshness worse.
    if (size <= kSmallRangeBytes || !enabled_ || stats_.sampling_disabled) {
      return FullRaw(data, size, seed);
    }

    // A guard miss proves this physical range needs exact validation. Check
    // the merged interval set before consulting any epoch memo or volatility.
    if (IsQuarantined(address, size)) {
      ++stats_.strict_lookups;
      stats_.strict_bytes += size;
      return FullRaw(data, size, seed);
    }

    const Key key{address, size, seed, kind};
    auto it = FindCurrent(key);
    if (it == entries_.end()) {
      if (entries_.size() >= kMaxEntries) {
        entries_.clear();
        ++epoch_;
        ++stats_.invalidations;
      }
      Entry entry;
      entry.write_version=guest_writes_.version();
      const auto full = FullSampled(data, size, seed, address);
      entry.hash = full.hash;
      entry.phase_hashes = full.phase_hashes;
      entry.last_full_hash = full.hash;
      entry.last_validated_epoch = epoch_;
      entry.last_validated_frame = frame_;
      entry.texture_interval = 1;
      entry.texture_next_frame = frame_ + 1;
      auto [inserted, _] = entries_.emplace(key, std::move(entry));
      return {inserted->second.hash, size, false};
    }

    Entry& entry = it->second;
    if (kind == ValidationKind::kGeometry && frame_ < entry.volatile_until_frame) {
      ++stats_.volatile_bypasses;
      return geometry_epoch_checks_?UnhashedVolatile(size):FullVolatile(data, size, seed);
    }
    if (kind == ValidationKind::kGeometry && entry.volatile_until_frame != 0) {
      entry.volatile_until_frame = 0;
      entry.consecutive_changes = 0;
      if (entry.needs_volatile_reseed) {
        const auto full = FullSampled(data, size, seed, address);
        entry.hash = full.hash;
        entry.last_full_hash = full.hash;
        entry.phase_hashes = full.phase_hashes;
        entry.last_validated_epoch = epoch_;
        entry.last_validated_frame = frame_;
        entry.needs_volatile_reseed = false;
        return {entry.hash, size, false};
      }
    }

    if (entry.last_validated_epoch == epoch_) {
      ++stats_.hits;
      ++stats_.epoch_hits;
      return {entry.hash, 0, false};
    }

    if (kind == ValidationKind::kTexture && frame_ < entry.texture_next_frame) {
      entry.last_validated_epoch = epoch_;
      ++stats_.hits;
      ++stats_.texture_deferred;
      return {entry.hash, 0, false};
    }

    const uint32_t phase = kind == ValidationKind::kTexture
                               ? entry.next_phase
                               : uint32_t((frame_ + (uint64_t(address) >> 6)) & (kPhaseCount - 1));
    const auto sample_result = SamplePhase(data, size, address, phase, seed);
    stats_.sample_bytes += sample_result.bytes;
    uint64_t bytes_hashed = sample_result.bytes;
    ++stats_.sampled_checks;
    stats_.training_remaining = stats_.guard_checks >= kGuardChecks
                                    ? 0
                                    : kGuardChecks - stats_.guard_checks;

    const bool sample_matches = sample_result.hash == entry.phase_hashes[phase];
    bool changed = false;
    if (!sample_matches) {
      ++stats_.sample_changes;
      const auto full = FullSampled(data, size, seed, address);
      bytes_hashed += size;
      if (full.hash != entry.last_full_hash) {
        changed = true;
        if (kind == ValidationKind::kGeometry) {
          ++entry.consecutive_changes;
          if (entry.consecutive_changes >= 2) {
            entry.volatile_until_frame = frame_ + kVolatileFrames;
            entry.needs_volatile_reseed = true;
            ++stats_.volatile_bypasses;
          }
        }
      } else {
        entry.consecutive_changes = 0;
      }
      entry.hash = full.hash;
      entry.phase_hashes = full.phase_hashes;
      entry.last_full_hash = full.hash;
      if (kind == ValidationKind::kTexture) {
        entry.texture_interval = 1;
        entry.texture_next_frame = frame_ + 1;
      }
    } else {
      entry.consecutive_changes = 0;
      // The first 20,000 successful samples are checked against a complete
      // fingerprint. After that training window, matching samples are trusted.
      if (stats_.guard_checks < kGuardChecks) {
        const uint64_t full_hash = FullFingerprint(data, size, seed, address);
        bytes_hashed += size;
        if (full_hash != entry.last_full_hash) {
          ++stats_.guard_misses;
          AddQuarantine(address, size);
          const uint64_t raw_hash = FullRaw(data, size, seed).hash;
          bytes_hashed += size;
          entry.hash = raw_hash;
          entry.last_full_hash = raw_hash;
          entry.phase_hashes.fill(0);
          entry.last_validated_epoch = epoch_;
          entry.last_validated_frame = frame_;
          return {raw_hash, bytes_hashed, false, true};
        }
        ++stats_.guard_checks;
        stats_.training_remaining = kGuardChecks - stats_.guard_checks;
      }
      ++stats_.hits;
      if (kind == ValidationKind::kTexture) {
        entry.texture_interval = std::min<uint64_t>(entry.texture_interval * 2,
                                                    kTextureMaxInterval);
        entry.texture_next_frame = frame_ + entry.texture_interval;
      }
    }

    if (kind == ValidationKind::kTexture) entry.next_phase = (entry.next_phase + 1) & (kPhaseCount - 1);
    entry.last_validated_epoch = epoch_;
    entry.last_validated_frame = frame_;
    if (changed && kind == ValidationKind::kGeometry &&
        entry.volatile_until_frame > frame_) {
      if(geometry_epoch_checks_) {auto result=UnhashedVolatile(size);result.bytes_hashed=bytes_hashed;return result;}
      // The sample-derived composite is not suitable as a stable same-work
      // geometry key. Start the volatile window with an exact raw hash too.
      const auto exact = FullVolatile(data, size, seed);
      bytes_hashed += exact.bytes_hashed;
      entry.hash = exact.hash;
      entry.last_full_hash = exact.hash;
      return {exact.hash, bytes_hashed, true, false, true};
    }
    return {entry.hash, bytes_hashed, false};
  }

  const Stats& stats() const { return stats_; }
  size_t entry_count() const { return entries_.size(); }

  // Ask about the original stable window before choosing a per-draw slice.
  // Exact dynamic slices do not enter this table or fragment the persistent
  // cache. The stable entry is reseeded normally after its volatile period.
  bool GeometryNeedsExactWindow(uint32_t address,size_t size) const {
    if(!enabled_)return false;
    if(IsQuarantined(address,size))return true;
    const auto it=entries_.find(Key{address,size,0,ValidationKind::kGeometry});
    return it!=entries_.end() && frame_<it->second.volatile_until_frame;
  }
  Result HashDynamicGeometry(const uint8_t* data,size_t size) {
    ++stats_.lookups;
    return FullVolatile(data,size,0);
  }

 private:
  struct Key {
    uint32_t address;
    size_t size;
    uint64_t seed;
    ValidationKind kind;
    friend bool operator==(const Key&, const Key&) = default;
  };

  struct KeyHash {
    size_t operator()(const Key& key) const noexcept {
      uint64_t h = (uint64_t(key.address) << 32) ^ uint64_t(key.size);
      h ^= key.seed + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
      h ^= uint64_t(key.kind) * 0xD6E8FEB86659FD93ull;
      return static_cast<size_t>(h ^ (h >> 32));
    }
  };

  struct Entry {
    uint64_t write_version=0;
    uint64_t hash = 0;
    uint64_t last_full_hash = 0;
    std::array<uint64_t, kPhaseCount> phase_hashes{};
    uint64_t last_validated_epoch = std::numeric_limits<uint64_t>::max();
    uint64_t last_validated_frame = 0;
    uint64_t texture_interval = 1;
    uint64_t texture_next_frame = 0;
    uint64_t volatile_until_frame = 0;
    uint32_t consecutive_changes = 0;
    bool needs_volatile_reseed = false;
    bool exact_epoch=false;
    uint64_t guard_generation=0;
    uint8_t next_phase = 0;
  };

  auto FindCurrent(const Key& key) -> std::unordered_map<Key,Entry,KeyHash>::iterator {
    auto it=entries_.find(key);
    if(it!=entries_.end()) {
      if(guest_writes_.ChangedSince(key.address,uint32_t(key.size),it->second.write_version)) {
        entries_.erase(it);return entries_.end(); // Full reseed, never sample a known write.
      }
      it->second.write_version=guest_writes_.version();
    }
    return it;
  }

  struct Fingerprints {
    uint64_t hash = 0;
    std::array<uint64_t, kPhaseCount> phase_hashes{};
  };

  static constexpr uint64_t kFullSeed = 0x243F6A8885A308D3ull;
  static constexpr uint64_t kPhaseSeed = 0x13198A2E03707344ull;
  static constexpr uint64_t kFoldMultiplier = 0x9E3779B97F4A7C15ull;

  bool RangeCanBeMemoized(uint32_t address, size_t size) const {
    return size == 0 || uint64_t(address) + uint64_t(size) <= (uint64_t(1) << 32);
  }

  bool IsQuarantined(uint32_t address, size_t size) const {
    if (size == 0) return false;
    const uint64_t begin = address;
    const uint64_t end = begin + uint64_t(size);
    auto next = quarantined_ranges_.lower_bound(begin);
    if (next != quarantined_ranges_.end() && next->first < end) return true;
    if (next != quarantined_ranges_.begin()) {
      const auto previous = std::prev(next);
      if (previous->second > begin) return true;
    }
    return false;
  }

  void AddQuarantine(uint32_t address, size_t size) {
    const uint64_t begin = address;
    const uint64_t end = begin + uint64_t(size);
    uint64_t merged_begin = begin;
    uint64_t merged_end = end;
    auto it = quarantined_ranges_.lower_bound(begin);
    if (it != quarantined_ranges_.begin()) {
      auto previous = std::prev(it);
      if (previous->second > begin) it = previous;
    }

    bool merged = false;
    while (it != quarantined_ranges_.end() && it->first < merged_end) {
      if (it->second <= merged_begin) {
        ++it;
        continue;
      }
      merged = true;
      merged_begin = std::min(merged_begin, it->first);
      merged_end = std::max(merged_end, it->second);
      it = quarantined_ranges_.erase(it);
    }

    // Adjacent half-open intervals remain separate: [a,b) does not overlap
    // [b,c). Saturation is the only case that disables sampling globally.
    if (!merged && quarantined_ranges_.size() >= kMaxQuarantinedRanges) {
      stats_.sampling_disabled = true;
      ++stats_.quarantine_overflows;
      return;
    }
    quarantined_ranges_.emplace(merged_begin, merged_end);
    stats_.quarantined_ranges = quarantined_ranges_.size();
  }

  Result FullRaw(const uint8_t* data, size_t size, uint64_t seed) {
    ++stats_.full_checks;
    stats_.full_bytes += size;
    return {XXH3_64bits_withSeed(data, size, seed), size, false};
  }

  Result UnhashedVolatile(size_t size) {
    ++stats_.volatile_unhashed_calls;stats_.volatile_unhashed_bytes+=size;
    // The renderer must snapshot/upload every use; hash zero is NOT a cache key.
    return {0,0,true,false,true,true};
  }

  Result FullVolatile(const uint8_t* data, size_t size, uint64_t seed) {
    ++stats_.volatile_exact_checks;
    stats_.volatile_full_bytes += size;
    Result result = FullRaw(data, size, seed);
    result.bypass_cache = true;
    result.volatile_geometry = true;
    return result;
  }

  static uint64_t Fold(uint64_t current, uint64_t value) {
    uint64_t mixed = current ^ value;
    mixed *= kFoldMultiplier;
    return mixed ^ (mixed >> 29);
  }

  static size_t LineCount(size_t size) { return (size + kLineBytes - 1) / kLineBytes; }

  static uint64_t HashLine(const uint8_t* data, size_t size, size_t line, uint64_t seed) {
    const size_t offset = line * kLineBytes;
    const size_t count = std::min(kLineBytes, size - offset);
    return XXH3_64bits_withSeed(data + offset, count, seed);
  }

  uint64_t FullFingerprint(const uint8_t* data, size_t size, uint64_t seed,
                           uint32_t address) {
    const size_t line_count = LineCount(size);
    uint64_t result = kFullSeed ^ seed ^ (uint64_t(address) << 1) ^ uint64_t(size);
    const uint64_t line_base = uint64_t(address) >> 6;
    for (size_t line = 0; line < line_count; ++line) {
      result = Fold(result, HashLine(data, size, line, seed) ^ (line_base + line));
    }
    ++stats_.full_checks;
    stats_.full_bytes += size;
    return result;
  }

  struct SampleResult {
    uint64_t hash = 0;
    size_t bytes = 0;
  };

  static SampleResult SamplePhase(const uint8_t* data, size_t size, uint32_t address,
                                  uint32_t phase, uint64_t seed) {
    const size_t line_count = LineCount(size);
    const size_t last = line_count - 1;
    const uint64_t line_base = uint64_t(address) >> 6;
    SampleResult result;
    result.hash = kPhaseSeed + phase + seed + (uint64_t(address) << 1) + uint64_t(size);
    const auto fold_line = [&](size_t line) {
      const size_t offset = line * kLineBytes;
      const size_t bytes = std::min(kLineBytes, size - offset);
      result.hash = Fold(result.hash, HashLine(data, size, line, seed) ^ (line_base + line));
      result.bytes += bytes;
    };

    // Fold in address order exactly as the full per-phase fingerprint does,
    // but visit only the selected interior lines plus the two edge lines.
    fold_line(0);
    if (last != 0) {
      size_t first_selected = (phase - uint32_t(line_base & (kPhaseCount - 1))) &
                              (kPhaseCount - 1);
      if (first_selected == 0) first_selected = kPhaseCount;
      for (size_t line = first_selected; line < last; line += kPhaseCount) fold_line(line);
      fold_line(last);
    }
    return result;
  }

  Fingerprints FullSampled(const uint8_t* data, size_t size, uint64_t seed,
                           uint32_t address) {
    Fingerprints result;
    const size_t line_count = LineCount(size);
    const size_t last = line_count - 1;
    const uint64_t line_base = uint64_t(address) >> 6;
    result.hash = kFullSeed ^ seed ^ (uint64_t(address) << 1) ^ uint64_t(size);
    for (uint32_t phase = 0; phase < kPhaseCount; ++phase)
      result.phase_hashes[phase] = kPhaseSeed + phase + seed + (uint64_t(address) << 1) + uint64_t(size);

    for (size_t line = 0; line < line_count; ++line) {
      const uint64_t absolute_line = line_base + line;
      const uint64_t line_hash = HashLine(data, size, line, seed);
      result.hash = Fold(result.hash, line_hash ^ absolute_line);
      const uint32_t line_phase = uint32_t(absolute_line & (kPhaseCount - 1));
      result.phase_hashes[line_phase] = Fold(result.phase_hashes[line_phase], line_hash ^ absolute_line);
      if (line == 0 || line == last) {
        for (uint32_t phase = 0; phase < kPhaseCount; ++phase) {
          if (phase != line_phase)
            result.phase_hashes[phase] = Fold(result.phase_hashes[phase], line_hash ^ absolute_line);
        }
      }
    }
    ++stats_.full_checks;
    stats_.full_bytes += size;
    return result;
  }

  bool enabled_ = false;
  bool geometry_epoch_checks_ = false;
  uint64_t frame_ = 0;
  uint64_t epoch_ = 0;
  Stats stats_{};
  std::unordered_map<Key, Entry, KeyHash> entries_;
  GuestMemoryWrites guest_writes_;
  // Physical addresses, not shader metadata, define the safety boundary.
  // This survives memo-table resets so a hard entry clear cannot un-quarantine
  // a range whose sample disagreed with its full fingerprint.
  std::map<uint64_t, uint64_t> quarantined_ranges_;
};

}  // namespace fear_nx::deko
