// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// SDK-independent helpers for bounding the guest vertex words a draw can read.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>

namespace fear_nx::deko {

class IndexBounds {
 public:
  static constexpr uint32_t kIndexMask = 0x00FFFFFFu;

  // Restart is compared against the raw index. The base is only applied to
  // ordinary indices, matching the guest draw's index conversion order.
  void Include(uint32_t raw_index, uint32_t base, bool restart_enabled = false,
               uint32_t restart_index = 0) noexcept {
    if (restart_enabled && raw_index == restart_index) {
      return;
    }
    IncludeTransformed(static_cast<uint32_t>((uint64_t(raw_index) + uint64_t(base)) & kIndexMask));
  }

  // Clamp each index to [minimum, maximum]. Since clamping is monotone, it is
  // sufficient to clamp the accumulated endpoints.
  void Clamp(uint32_t minimum, uint32_t maximum) noexcept {
    if (!valid_) {
      return;
    }
    if (minimum > maximum || maximum > kIndexMask) {
      Reset();
      return;
    }
    minimum_ = std::clamp(minimum_, minimum, maximum);
    maximum_ = std::clamp(maximum_, minimum, maximum);
  }

  // Auto-index draws form a contiguous raw index interval. Endpoint bounds
  // are exact when the transformed interval stays within one 24-bit cycle.
  // On wrap, use the full caller-provided clamp interval instead of walking
  // every generated index.
  static IndexBounds AutoDraw(uint32_t first_raw_index, uint32_t count, uint32_t base,
                              uint32_t clamp_minimum, uint32_t clamp_maximum) noexcept {
    IndexBounds result;
    if (count == 0 || clamp_minimum > clamp_maximum || clamp_maximum > kIndexMask) {
      return result;
    }

    const uint64_t first_sum = uint64_t(first_raw_index) + uint64_t(base);
    const uint64_t last_raw_index = uint64_t(first_raw_index) + uint64_t(count) - 1;
    const uint64_t last_sum = last_raw_index + uint64_t(base);
    if ((first_sum >> 24) != (last_sum >> 24)) {
      result.minimum_ = clamp_minimum;
      result.maximum_ = clamp_maximum;
      result.valid_ = true;
      result.conservative_ = true;
      return result;
    }

    result.IncludeTransformed(static_cast<uint32_t>(first_sum & kIndexMask));
    result.IncludeTransformed(static_cast<uint32_t>(last_sum & kIndexMask));
    result.Clamp(clamp_minimum, clamp_maximum);
    return result;
  }

  void Reset() noexcept {
    minimum_ = 0;
    maximum_ = 0;
    valid_ = false;
    conservative_ = false;
  }

  bool valid() const noexcept { return valid_; }
  bool conservative() const noexcept { return conservative_; }
  uint32_t min() const noexcept { return minimum_; }
  uint32_t max() const noexcept { return maximum_; }

 private:
  void IncludeTransformed(uint32_t index) noexcept {
    if (!valid_) {
      minimum_ = maximum_ = index;
      valid_ = true;
      return;
    }
    minimum_ = std::min(minimum_, index);
    maximum_ = std::max(maximum_, index);
  }

  uint32_t minimum_ = 0;
  uint32_t maximum_ = 0;
  bool valid_ = false;
  bool conservative_ = false;
};

struct FetchAccess {
  uint32_t stride_words = 0;
  uint32_t needed_words_mask = 0;
  int32_t offset_words = 0;
  bool index_rounded = false;
};

struct VertexByteRange {
  // Invalid results deliberately describe the original range, so callers can
  // fall back without risking a truncated guest read.
  uint64_t first_byte = 0;
  uint64_t end_byte = 0;
  uint64_t prefix_bytes = 0;
  uint64_t used_bytes = 0;
  bool valid = false;
  bool clipped = false;
  bool conservative = false;
};

inline uint32_t LowestSetBit(uint32_t mask) noexcept {
  uint32_t bit = 0;
  while ((mask & 1u) == 0) {
    mask >>= 1;
    ++bit;
  }
  return bit;
}

inline uint32_t HighestSetBit(uint32_t mask) noexcept {
  uint32_t bit = 0;
  while (mask >>= 1) {
    ++bit;
  }
  return bit;
}

inline uint32_t RoundedFetchIndex(uint32_t index) noexcept {
  // Keep the addition in float: at and above 2^23, +0.5f can tie-round an
  // odd integer to the next even integer before floor is applied.
  return static_cast<uint32_t>(std::floor(static_cast<float>(index) + 0.5f));
}

inline VertexByteRange UsedVertexPrefix(uint64_t original_bytes, const IndexBounds& indices,
                                        std::span<const FetchAccess> accesses) noexcept {
  VertexByteRange result{0, original_bytes, original_bytes, original_bytes, false, false, true};
  if (!indices.valid() || accesses.empty()) {
    return result;
  }

  uint64_t first_word = std::numeric_limits<uint64_t>::max();
  uint64_t end_word = 0;
  bool any_access = false;
  for (const FetchAccess& access : accesses) {
    if (access.needed_words_mask == 0) {
      continue;
    }

    const uint32_t first_index = access.index_rounded ? RoundedFetchIndex(indices.min()) : indices.min();
    const uint32_t last_index = access.index_rounded ? RoundedFetchIndex(indices.max()) : indices.max();
    const int64_t first_base = int64_t(first_index) * int64_t(access.stride_words) + access.offset_words;
    const int64_t last_base = int64_t(last_index) * int64_t(access.stride_words) + access.offset_words;
    const uint32_t first_component = LowestSetBit(access.needed_words_mask);
    const uint32_t last_component_end = HighestSetBit(access.needed_words_mask) + 1;
    const int64_t access_first = first_base + int64_t(first_component);
    const int64_t access_end = last_base + int64_t(last_component_end);
    if (access_first < 0 || access_end <= access_first) {
      return result;
    }

    const uint64_t candidate_first = static_cast<uint64_t>(access_first);
    const uint64_t candidate_end = static_cast<uint64_t>(access_end);
    if (candidate_end > std::numeric_limits<uint64_t>::max() / sizeof(uint32_t)) {
      return result;
    }
    first_word = std::min(first_word, candidate_first);
    end_word = std::max(end_word, candidate_end);
    any_access = true;
  }

  if (!any_access || first_word >= end_word || end_word > original_bytes / sizeof(uint32_t)) {
    return result;
  }

  const uint64_t first_byte = first_word * sizeof(uint32_t);
  const uint64_t end_byte = end_word * sizeof(uint32_t);
  result.first_byte = first_byte;
  result.end_byte = end_byte;
  result.prefix_bytes = end_byte;
  result.used_bytes = end_byte - first_byte;
  result.valid = true;
  result.clipped = end_byte < original_bytes;
  result.conservative = indices.conservative();
  return result;
}

struct VertexWindow {
  uint32_t first_byte = 0;
  uint32_t bytes = 0;
};

// Cache identity must not depend on the precise maximum index of each draw.
// Select a stable, descriptor-relative, power-of-two window containing every
// proven read. Small buffers and unknown bounds retain the whole descriptor.
inline VertexWindow SelectVertexWindowLegacy(uint32_t original_bytes,
                                       const VertexByteRange& range) noexcept {
  constexpr uint64_t kMinimumWindowBytes = 64u << 10;
  if (!range.valid || range.first_byte >= range.end_byte ||
      range.end_byte > original_bytes || (range.first_byte & 3) ||
      (range.end_byte & 3)) {
    return {0, original_bytes};
  }
  for (uint64_t size = kMinimumWindowBytes; size < original_bytes; size *= 2) {
    const uint64_t first = range.first_byte / size * size;
    const uint64_t end = std::min(first + size, uint64_t(original_bytes));
    if (range.end_byte <= end) {
      return {uint32_t(first), uint32_t(end - first)};
    }
  }
  return {0, original_bytes};
}

// Stable 64 KiB cells bound the proven range without inflating a small
// boundary-crossing draw to its power-of-two ancestor. Both ends remain on
// the same fixed grid for neighboring draws; unknown fetches keep all bytes.
inline VertexWindow SelectVertexWindow(uint32_t original_bytes,
                                       const VertexByteRange& range) noexcept {
  constexpr uint64_t kCellBytes = 64u << 10;
  if (!range.valid || range.first_byte >= range.end_byte ||
      range.end_byte > original_bytes || (range.first_byte & 3) ||
      (range.end_byte & 3)) return {0, original_bytes};
  const uint64_t first = range.first_byte / kCellBytes * kCellBytes;
  const uint64_t end = std::min((range.end_byte + kCellBytes - 1) / kCellBytes * kCellBytes,
                                uint64_t(original_bytes));
  return {uint32_t(first), uint32_t(end - first)};
}

// Changing data does not benefit from keeping unused 64 KiB cells alive.
// Keep only proven fetch words, with at most 15 bytes of edge alignment.
// Unknown/unsupported fetches still retain the full declared range.
inline VertexWindow SelectDynamicVertexWindow(uint32_t original_bytes,
                                              const VertexByteRange& range) noexcept {
  if(!range.valid || range.first_byte>=range.end_byte || range.end_byte>original_bytes ||
     (range.first_byte&3) || (range.end_byte&3))return {0,original_bytes};
  const uint64_t first=range.first_byte&~uint64_t(15);
  const uint64_t end=std::min((range.end_byte+15)&~uint64_t(15),uint64_t(original_bytes));
  return {uint32_t(first),uint32_t(end-first)};
}

// The translated shader uses a nonnegative byte base in the shared SSBO.
// Negative bases cannot be encoded by wrapping: its address shift is logical.
// Keep the original guest indices and subtract the window start instead.
inline bool RebaseVertexWindow(VertexWindow window, uint64_t upload_offset,
                               uint32_t capacity, uint32_t& fetch_base) noexcept {
  if (!window.bytes || (window.first_byte & 3) || (upload_offset & 3) ||
      upload_offset < window.first_byte || upload_offset > capacity ||
      window.bytes > capacity - upload_offset) {
    return false;
  }
  fetch_base = uint32_t(upload_offset - window.first_byte);
  return true;
}

}  // namespace fear_nx::deko
