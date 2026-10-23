// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "deko_cpu_compare.h"

#include "deko_vertex_range.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace fear_nx::deko {

// Address/length only suggest a previously computed content hash. A hint is
// NEVER proof: the caller must find an exact matching CPU shadow before reuse.
// This removes hashing plus duplicate cache probes on hits while still seeing
// writes at any byte within a work, including immediate command buffer reuse.
class IndexValidationMemo {
 public:
  struct Stats { uint64_t candidates=0, hits=0, rejected=0; };
  template <typename Verify>
  bool TryReuse(std::span<const uint8_t> source, Verify&& verify) {
    if (source.empty()) return false;
    const auto& e=entries_[Slot(source)];
    if (e.address!=reinterpret_cast<uintptr_t>(source.data()) || e.bytes!=source.size()) return false;
    ++stats_.candidates;
    if (verify(e.hash)) { ++stats_.hits;return true; }
    ++stats_.rejected;return false;
  }
  void Remember(std::span<const uint8_t> source,uint64_t hash) {
    if (!source.empty()) entries_[Slot(source)]={reinterpret_cast<uintptr_t>(source.data()),source.size(),hash};
  }
  const Stats& stats() const { return stats_; }
 private:
  static size_t Slot(std::span<const uint8_t> source) {
    uint64_t h=(uint64_t(reinterpret_cast<uintptr_t>(source.data()))>>2) ^ (uint64_t(source.size())*0x9E3779B97F4A7C15ull);
    h^=h>>30;h*=0xBF58476D1CE4E5B9ull;
    h^=h>>27;h*=0x94D049BB133111EBull;
    return size_t(h^(h>>31))&4095;
  }
  struct Entry {uintptr_t address=0;size_t bytes=0;uint64_t hash=0;};
  std::array<Entry,4096> entries_{};
  Stats stats_{};
};

// Immutable uploads are reusable only in the current GPU work. CPU shadows
// verify exact bytes even on a hash collision and are never read from the
// CPU-uncached GPU upload heap. Dropping metadata cannot reclaim GPU memory.
template <typename Value, size_t KeyWords, size_t PayloadAlignment = 1>
class WorkUploadCache {
 public:
  static_assert(PayloadAlignment && !(PayloadAlignment & (PayloadAlignment-1)) &&
                PayloadAlignment <= alignof(std::max_align_t));
  using Key = std::array<uint64_t, KeyWords>;
  WorkUploadCache(size_t max_entries, size_t max_shadow_bytes)
      : max_entries_(max_entries), max_shadow_bytes_(max_shadow_bytes),
        entries_(TableSize(max_entries)) {}

  // Retain the table and byte arena: no node/vector frees on the ring thread.
  // Stale entries are invisible, including after the epoch counter wraps.
  void Clear() {
    size_ = shadow_bytes_ = used_bytes_ = 0;
    if (++epoch_ == 0) {
      for (auto& entry : entries_) entry.epoch = 0;
      epoch_ = 1;
    }
  }

  const Value* Find(const Key& key, std::span<const uint8_t> source,
                    std::span<const uint8_t>* payload = nullptr,
                    std::span<const uint8_t>* verified_source = nullptr) const {
    const auto& entry = entries_[FindSlot(key)];
    if (entry.epoch != epoch_ || entry.source_bytes != source.size() ||
        (!source.empty() && std::memcmp(arena_.get() + entry.offset, source.data(), source.size()))) {
      return nullptr;
    }
    if (payload) *payload = entry.payload_bytes
        ? std::span<const uint8_t>(arena_.get() + entry.offset + entry.payload_offset, entry.payload_bytes)
        : std::span<const uint8_t>{};
    if (verified_source) *verified_source = entry.source_bytes
        ? std::span<const uint8_t>(arena_.get() + entry.offset, entry.source_bytes)
        : std::span<const uint8_t>{};
    return &entry.value;
  }

  bool Insert(const Key& key, std::span<const uint8_t> source, const Value& value,
              std::span<const uint8_t> payload = {}) {
    auto& entry = entries_[FindSlot(key)];
    const bool exists = entry.epoch == epoch_;
    const size_t old_bytes = exists ? entry.payload_offset + entry.payload_bytes : 0;
    if ((!exists && size_ >= max_entries_) || source.size() > max_shadow_bytes_ ||
        payload.size() > max_shadow_bytes_ - source.size()) return false;
    // Only the converted-index instantiation opts into alignment. Padding is
    // charged against the same fixed arena limit, never hidden over-allocation.
    const size_t payload_offset = payload.empty() ? source.size() : Align(source.size());
    if (payload_offset > max_shadow_bytes_ || payload.size() > max_shadow_bytes_-payload_offset) return false;
    const size_t bytes = payload_offset + payload.size();
    if (bytes > max_shadow_bytes_ - (shadow_bytes_ - old_bytes)) return false;
    // A collision replacement may reuse its region. Larger replacements need
    // fresh arena space; exhaustion merely declines retention, never a draw.
    const bool reuse = exists && bytes <= entry.capacity;
    const size_t offset = reuse ? entry.offset : Align(used_bytes_);
    if (!reuse && (offset > max_shadow_bytes_ || bytes > max_shadow_bytes_ - offset)) return false;
    if (bytes && !arena_) arena_.reset(new uint8_t[max_shadow_bytes_]);
    if (!source.empty()) std::memmove(arena_.get() + offset, source.data(), source.size());
    if (!payload.empty()) std::memmove(arena_.get() + offset + payload_offset, payload.data(), payload.size());
    if (!reuse) { used_bytes_ = offset + bytes; entry.capacity = bytes; }
    if (!exists) ++size_;
    entry.key = key; entry.value = value; entry.offset = offset;
    entry.source_bytes = source.size(); entry.payload_offset = payload_offset;
    entry.payload_bytes = payload.size(); entry.epoch = epoch_;
    shadow_bytes_ = shadow_bytes_ - old_bytes + bytes;
    return true;
  }

  size_t size() const { return size_; }
  size_t shadow_bytes() const { return shadow_bytes_; }
  size_t capacity_bytes() const { return max_shadow_bytes_; }
  const void* storage_identity() const { return arena_.get(); }

 private:
  static size_t Align(size_t value) { return (value+PayloadAlignment-1)&~(PayloadAlignment-1); }
  static size_t TableSize(size_t count) {
    size_t size = 2;
    while (size / 2 < count) size *= 2;
    return size;
  }
  struct Hash {
    size_t operator()(const Key& key) const {
      uint64_t result = 0x9E3779B97F4A7C15ull;
      for (uint64_t word : key) {
        word ^= word >> 30; word *= 0xBF58476D1CE4E5B9ull;
        word ^= word >> 27; word *= 0x94D049BB133111EBull;
        result ^= (word ^ (word >> 31)) + (result << 6) + (result >> 2);
      }
      return size_t(result);
    }
  };
  size_t FindSlot(const Key& key) const {
    size_t slot = Hash{}(key) & (entries_.size() - 1);
    while (entries_[slot].epoch == epoch_ && entries_[slot].key != key)
      slot = (slot + 1) & (entries_.size() - 1);
    return slot;
  }
  struct Entry {
    Key key{}; Value value{};
    size_t offset = 0, source_bytes = 0, payload_offset = 0, payload_bytes = 0, capacity = 0;
    uint64_t epoch = 0;
  };
  size_t max_entries_, max_shadow_bytes_, shadow_bytes_ = 0;
  size_t size_ = 0, used_bytes_ = 0;
  uint64_t epoch_ = 1;
  std::vector<Entry> entries_;
  std::unique_ptr<uint8_t[]> arena_;
};

// Register generations may skip preparation, but GPU addresses are valid only
// in their original work. A new work must upload the retained CPU bytes again.
template <typename Value>
struct GenerationUpload {
  const Value* Find(uint64_t generation, uint64_t work) const {
    return valid_ && generation_ == generation && work_ == work ? &value_ : nullptr;
  }
  void Store(uint64_t generation, uint64_t work, const Value& value) {
    generation_ = generation; work_ = work; value_ = value; valid_ = true;
  }
 private:
  Value value_{};
  uint64_t generation_ = 0, work_ = 0;
  bool valid_ = false;
};

struct ConstantCopyRange {
  size_t source_offset = 0, destination_offset = 0, size = 0;
};

// Conservative destination envelope for ONE packing revision. Consumers that
// skipped a revision (including older work slots) must scan their whole shadow.
struct ConstantDirtyRange { size_t begin=0,end=0; };

// Shadow of an ordered GPU constant buffer, never a readback from GPU memory.
// Initialize is called only for a fresh allocation in this work. Later writes
// must be recorded as PushConstants commands, not CPU writes to that buffer:
// earlier draws in the command list still need its earlier values.
class ConstantDeltaShadow {
 public:
  struct Changes { uint32_t bytes = 0, commands = 0; };
  static constexpr size_t kMaxPushBytes = 0x7FF0; // deko3d limit, rounded to 16.
  static constexpr size_t kMergeGapBytes = 64;
  bool Ready(uint64_t work, size_t bytes) const {
    return valid_ && work_ == work && bytes_ == bytes;
  }
  bool Initialize(uint64_t work, std::span<const uint8_t> source) {
    if (source.empty() || source.size() > 65536) return false;
    shadow_.assign((source.size() + 255) & ~size_t(255), 0);
    std::memcpy(shadow_.data(), source.data(), source.size());
    bytes_ = source.size(); work_ = work; valid_ = true; return true;
  }
  std::span<const uint8_t> bytes() const { return shadow_; }
  template <typename Emit>
  Changes Update(std::span<const uint8_t> source, Emit emit) {
    return UpdateRange(source,{0,source.size()},emit);
  }
  template <typename Emit>
  Changes UpdateRange(std::span<const uint8_t> source,ConstantDirtyRange dirty,Emit emit) {
    Changes result;
    if (!valid_ || source.size() != bytes_) return result;
    if(dirty.begin>=dirty.end || dirty.end>bytes_)return result;
    const size_t end = (dirty.end + 15) & ~size_t(15);
    auto changed = [&](size_t offset) {
      if (bytes_ - offset >= 16)
        return !EqualCpuBytes(shadow_.data() + offset, source.data() + offset, 16);
      return !EqualCpuBytes(shadow_.data() + offset, source.data() + offset, bytes_ - offset);
    };
    for (size_t offset = dirty.begin & ~size_t(15); offset < end;) {
      // Constant-size comparisons let AArch64 skip four unchanged registers
      // together; never read beyond the source's final partial register.
      while (offset + 64 <= bytes_ && offset + 64 <= end &&
             EqualCpuBytes(shadow_.data() + offset, source.data() + offset, 64)) offset += 64;
      if (offset == end) break;
      if (!changed(offset)) { offset += 16; continue; }
      const size_t start = offset;
      size_t last_changed_end=offset+16;
      offset+=16;
      // A small unchanged gap costs less than another selector/offset/push
      // command sequence. Preserve the exact gap bytes in the combined push.
      while(offset<end && offset-start<kMaxPushBytes) {
        if(changed(offset))last_changed_end=offset+16;
        else if(offset+16-last_changed_end>kMergeGapBytes)break;
        offset+=16;
      }
      offset=last_changed_end; // Do not emit trailing unchanged registers.
      std::memcpy(shadow_.data() + start, source.data() + start,
                  std::min(offset, bytes_) - start);
      // emit must copy its input into the command stream synchronously.
      emit(uint32_t(start), std::span<const uint8_t>(shadow_).subspan(start, offset - start));
      result.bytes += uint32_t(offset - start); ++result.commands;
    }
    return result;
  }
 private:
  std::vector<uint8_t> shadow_;
  size_t bytes_ = 0;
  uint64_t work_ = 0;
  bool valid_ = false;
};

// Reflection is immutable. Validate/merge its copy ranges once and keep zero
// padding initialized, rather than clearing and validating the layout per draw.
class ConstantPackingPlan {
 public:
  // Writers stamp this group whenever one of its per-register versions changes.
  // A layout owns its observations; preparing one layout cannot clear another.
  static constexpr size_t kVersionGroupRegisters = 8;
  bool Configure(size_t source_bytes, size_t destination_bytes,
                 std::span<const ConstantCopyRange> ranges) {
    if (!destination_bytes || ranges.empty()) return false;
    for (const auto& r : ranges) {
      if (r.source_offset > source_bytes || r.size > source_bytes - r.source_offset ||
          r.destination_offset > destination_bytes || r.size > destination_bytes - r.destination_offset)
        return false;
    }
    ranges_.clear();
    for (const auto& r : ranges) {
      if (!ranges_.empty() && ranges_.back().source_offset + ranges_.back().size == r.source_offset &&
          ranges_.back().destination_offset + ranges_.back().size == r.destination_offset) ranges_.back().size += r.size;
      else ranges_.push_back(r);
    }
    // A clipped copy is valid only if no later destination write can overwrite
    // it. Keep the established ordered full replay for overlapping layouts.
    auto destination_order=ranges_;
    std::sort(destination_order.begin(),destination_order.end(),
              [](const auto& a,const auto& b){return a.destination_offset<b.destination_offset;});
    partial_packable_=true;
    size_t destination_end=0;
    for(const auto& r:destination_order)if(r.size) {
      if(r.destination_offset<destination_end)partial_packable_=false;
      destination_end=std::max(destination_end,r.destination_offset+r.size);
    }
    // Only source bytes actually read by reflection can affect the packed
    // result. Merge their union once; retain the original destination order
    // below, including overlapping destination writes and zero padding.
    reads_=ranges_;
    std::sort(reads_.begin(),reads_.end(),[](const auto& a,const auto& b){return a.source_offset<b.source_offset;});
    size_t count=0;
    for(const auto r:reads_) {
      if(!r.size)continue;
      if(count && r.source_offset<=reads_[count-1].source_offset+reads_[count-1].size) {
        auto& last=reads_[count-1];
        last.size=std::max(last.source_offset+last.size,r.source_offset+r.size)-last.source_offset;
      } else reads_[count++]=r;
    }
    reads_.resize(count);
    used_bytes_=0;for(const auto& r:reads_)used_bytes_+=r.size;
    source_.assign(source_bytes,0); packed_.assign(destination_bytes, 0);
    version_reads_.clear();version_groups_.clear();
    for(const auto& r:reads_)for(size_t at=r.source_offset;at<r.source_offset+r.size;) {
      const size_t bytes=std::min(r.source_offset+r.size-at,16-at%16);
      const size_t group=at/(16*kVersionGroupRegisters);
      if(version_groups_.empty() || version_groups_.back().index!=group)
        version_groups_.push_back({group,version_reads_.size(),version_reads_.size(),0});
      version_reads_.push_back({at,bytes,0});
      version_groups_.back().end=version_reads_.size();at+=bytes;
    }
    versions_valid_=false;version_source_=nullptr;group_source_=nullptr;
    valid_ = false; revision_ = 0;dirty_={0,destination_bytes};
    return true;
  }
  bool Update(std::span<const uint8_t> source) {
    versions_valid_=false;group_source_=nullptr;
    if (source.size() != source_.size() || packed_.empty()) return false;
    bool changed=!valid_;
    if(valid_)for(const auto& r:reads_) {
      if(!EqualCpuBytes(source_.data()+r.source_offset,source.data()+r.source_offset,r.size)){changed=true;break;}
    }
    if(!changed)return false;
    for(const auto& r:reads_)std::memcpy(source_.data()+r.source_offset,source.data()+r.source_offset,r.size);
    CopyAllRanges();
    dirty_={0,packed_.size()};
    valid_ = true; ++revision_; return true;
  }
  // Versions belong to individual source registers, not a single shader or
  // work slot. Every layout retains its own observations. Optional group
  // versions avoid walking hundreds of registers after a small matrix write;
  // changed groups still use exact register versions and exact reflected bytes.
  bool UpdateVersioned(std::span<const uint8_t> source,std::span<const uint64_t> versions,
                       std::span<const uint64_t> groups={}) {
    if(source.size()!=source_.size() || versions.size()<(source.size()+15)/16)return Update(source);
    const size_t required_groups=((source.size()+15)/16+kVersionGroupRegisters-1)/kVersionGroupRegisters;
    const uint64_t* group_source=!groups.empty() && groups.size()>=required_groups?groups.data():nullptr;
    if(!versions_valid_ || version_source_!=versions.data() || group_source_!=group_source || !valid_) {
      const bool changed=Update(source);
      SnapshotVersions(versions,group_source);return changed;
    }
    size_t changed_begin=source_.size(),changed_end=0;
    auto scan=[&](size_t begin,size_t end) {
      for(size_t i=begin;i<end;++i) {
        auto& r=version_reads_[i];
        ++version_checks_;
        const auto version=versions[r.offset/16];
        if(r.version==version) {++version_skips_;continue;}
        r.version=version;
        if(EqualCpuBytes(source_.data()+r.offset,source.data()+r.offset,r.bytes))continue;
        std::memcpy(source_.data()+r.offset,source.data()+r.offset,r.bytes);
        // Reads are in ascending source order, so no min/max reduction is
        // needed for every changed register.
        if(!changed_end)changed_begin=r.offset;
        changed_end=r.offset+r.bytes;
      }
    };
    if(group_source) {
      size_t dirty_groups=0;
      for(const auto& g:version_groups_) {
        ++version_group_checks_;
        if(g.version==groups[g.index])++version_group_skips_;
        else ++dirty_groups;
      }
      if(!dirty_groups)return false;
      if(version_groups_.size()>1 && dirty_groups*4>=version_groups_.size()*3) {
        // A full bank write makes register-by-register comparisons/copies
        // slower than contiguous cached reads. Preserve the exact source-union
        // contract and ordered packing, then refresh every consumer's versions.
        ++version_bulk_updates_;
        const bool changed=Update(source);SnapshotVersions(versions,group_source);return changed;
      }
      for(auto& g:version_groups_)if(g.version!=groups[g.index]) {
        g.version=groups[g.index];scan(g.begin,g.end);
      }
    } else scan(0,version_reads_.size());
    if(changed_begin>=changed_end)return false;
    if(partial_packable_ && !reads_.empty() &&
       (changed_begin>reads_.front().source_offset || changed_end<reads_.back().source_offset+reads_.back().size)) {
      // All source changes have been captured before any destination writes,
      // so source aliasing remains safe. The dirty envelope may include gaps;
      // their retained bytes are exact and a single memcpy is cheaper than
      // allocating or walking a second per-register destination map.
      dirty_={packed_.size(),0};
      for(const auto& r:ranges_) {
        const size_t begin=std::max(changed_begin,r.source_offset);
        const size_t end=std::min(changed_end,r.source_offset+r.size);
        if(begin<end) {
          const size_t destination=r.destination_offset+begin-r.source_offset;
          std::memcpy(packed_.data()+destination,source_.data()+begin,end-begin);
          dirty_.begin=std::min(dirty_.begin,destination);
          dirty_.end=std::max(dirty_.end,destination+end-begin);
          packed_copy_bytes_+=end-begin;
        }
      }
      ++partial_packs_;
    } else {CopyAllRanges();dirty_={0,packed_.size()};}
    ++revision_;return true;
  }
  uint64_t version_checks() const {return version_checks_;}
  uint64_t version_skips() const {return version_skips_;}
  uint64_t version_group_checks() const {return version_group_checks_;}
  uint64_t version_group_skips() const {return version_group_skips_;}
  uint64_t version_bulk_updates() const {return version_bulk_updates_;}
  uint64_t packed_copy_bytes() const {return packed_copy_bytes_;}
  uint64_t partial_packs() const {return partial_packs_;}
  bool configured() const { return !packed_.empty(); }
  std::span<const uint8_t> bytes() const { return packed_; }
  uint64_t revision() const { return revision_; }
  ConstantDirtyRange dirty_since(uint64_t previous) const {
    if(previous==revision_)return {};
    return previous && previous==revision_-1?dirty_:ConstantDirtyRange{0,packed_.size()};
  }
  size_t range_count() const { return ranges_.size(); }
  size_t source_bytes_used() const {return used_bytes_;}
 private:
  void SnapshotVersions(std::span<const uint64_t> versions,const uint64_t* groups) {
    for(auto& r:version_reads_)r.version=versions[r.offset/16];
    if(groups)for(auto& g:version_groups_)g.version=groups[g.index];
    versions_valid_=true;version_source_=versions.data();group_source_=groups;
  }
  void CopyAllRanges() {
    // Original destination order also covers layouts whose writes overlap.
    for(const auto& r:ranges_)if(r.size) {
      std::memcpy(packed_.data()+r.destination_offset,source_.data()+r.source_offset,r.size);
      packed_copy_bytes_+=r.size;
    }
  }
  struct RegisterRead {size_t offset,bytes;uint64_t version;};
  struct RegisterGroup {size_t index,begin,end;uint64_t version;};
  std::vector<RegisterRead> version_reads_;
  std::vector<RegisterGroup> version_groups_;
  bool versions_valid_=false,partial_packable_=false;
  const uint64_t* version_source_=nullptr;
  const uint64_t* group_source_=nullptr;
  uint64_t version_checks_=0,version_skips_=0,version_group_checks_=0,version_group_skips_=0;
  uint64_t packed_copy_bytes_=0,partial_packs_=0,version_bulk_updates_=0;
  std::vector<ConstantCopyRange> ranges_,reads_;
  std::vector<uint8_t> source_, packed_;
  size_t used_bytes_=0;
  bool valid_ = false;
  uint64_t revision_ = 0;
  ConstantDirtyRange dirty_{};
};

struct IndexUploadDescription {
  uint32_t count = 0, base = 0, minimum = 0, maximum = IndexBounds::kIndexMask;
  uint32_t restart = 0, endian = 0;
  bool generated = false, index32 = false, fan = false, loop = false, reset = false;
  bool output16 = false;
  bool compact16 = false; // Try value-checked packing; actual width lives in the cached value.
};

// Keep restart sentinels, generated topology and unusual byte orders on the
// established 32-bit path. Min/max remain shader-side operations in both paths.
inline bool CanUseIndex16(const IndexUploadDescription& d) {
  return !d.generated && !d.index32 && !d.fan && !d.loop && !d.reset &&
         d.base == 0 && d.endian <= 1;
}

inline std::array<uint64_t, 4> IndexUploadKey(const IndexUploadDescription& d, uint64_t hash) {
  const uint64_t flags = uint64_t(d.generated) | (uint64_t(d.index32) << 1) |
      (uint64_t(d.fan) << 2) | (uint64_t(d.loop) << 3) | (uint64_t(d.reset) << 4) |
      (uint64_t(d.output16) << 5) | (uint64_t(d.compact16) << 6) | (uint64_t(d.endian) << 8);
  return {hash, uint64_t(d.count) | (flags << 32), uint64_t(d.base) | (uint64_t(d.restart) << 32),
          uint64_t(d.minimum) | (uint64_t(d.maximum) << 32)};
}

inline uint64_t IndexUploadCount(const IndexUploadDescription& d) {
  if (d.fan && d.loop) return 0;
  if (d.fan) return d.count < 3 ? 0 : (uint64_t(d.count) - 2) * 3;
  if (d.loop) return d.count < 2 ? 0 : uint64_t(d.count) + 1;
  return d.count;
}

// Conversion writes to cached CPU staging memory. A miss uploads the complete
// result in one memcpy, rather than one uncached GPU write per index.
template <typename ReadIndex>
bool BuildIndexUpload(const IndexUploadDescription& d, ReadIndex read,
                      std::span<uint32_t> output, IndexBounds& bounds) {
  const uint64_t count = IndexUploadCount(d);
  if (!count || output.size() != count) return false;
  bounds.Reset();
  auto put = [&](size_t at, uint32_t raw) {
    bounds.Include(raw, d.base, d.reset, d.restart);
    output[at] = d.reset && raw == d.restart ? UINT32_MAX : raw;
  };
  if (d.fan) {
    const uint32_t first = read(0);
    for (uint32_t i = 0; i + 2 < d.count; ++i) {
      put(size_t(i) * 3, first);
      put(size_t(i) * 3 + 1, read(i + 1));
      put(size_t(i) * 3 + 2, read(i + 2));
    }
  } else {
    for (uint32_t i = 0; i < d.count; ++i) put(i, read(i));
    if (d.loop) put(d.count, read(0));
  }
  bounds.Clamp(d.minimum, d.maximum);
  return true;
}

template <typename ReadIndex>
bool BuildIndexUpload16(const IndexUploadDescription& d, ReadIndex read,
                        std::span<uint16_t> output, IndexBounds& bounds) {
  if (!CanUseIndex16(d) || !d.count || output.size() != d.count) return false;
  bounds.Reset();
  for (uint32_t i = 0; i < d.count; ++i) {
    const uint32_t raw = read(i);
    if (raw > UINT16_MAX) return false; // Never silently truncate a changed reader.
    output[i] = uint16_t(raw);
    bounds.Include(raw, d.base);
  }
  bounds.Clamp(d.minimum, d.maximum);
  return true;
}

// Pack only raw, already endian-decoded and topology-expanded values. Guest
// base/clamp stay shader-side; do not decide eligibility from clamped bounds.
// Restart keeps the established Uint32 sentinel path (ordinary 65535 is legal
// when restart is disabled). A rejected conversion leaves output untouched.
inline bool PackIndexUpload16(const IndexUploadDescription& d,
                              std::span<const uint32_t> converted,
                              std::span<uint16_t> output) {
  if (d.reset || converted.empty() || converted.size()!=output.size()) return false;
  for (uint32_t value:converted) if(value>UINT16_MAX)return false;
  for(size_t i=0;i<converted.size();++i)output[i]=uint16_t(converted[i]);
  return true;
}

}  // namespace fear_nx::deko
