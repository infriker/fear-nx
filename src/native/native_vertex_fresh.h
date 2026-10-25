// SPDX-License-Identifier: GPL-3.0-or-later
// Vertex-span arithmetic adapted from Juansolo457/fear360-recomp, commit
// fc5722793f71217ca888c329d654bba6f9eeeedc, port-nx/src/native/native_renderer.cpp.
#pragma once

#include <algorithm>
#include <cstdint>

namespace fear_nx::native {

struct VertexByteSpan {
  uint32_t offset = 0;
  uint32_t size = 0;
};

inline bool ComputeVertexByteSpan(uint32_t index_lo, uint32_t index_hi,
                                  int32_t base, uint32_t clamp_lo, uint32_t clamp_hi,
                                  uint32_t stride, uint32_t extent, uint32_t bytes,
                                  VertexByteSpan& span) {
  if (index_lo > index_hi || clamp_lo > clamp_hi || !stride || !extent || !bytes) return false;
  const int64_t lo = int64_t(index_lo) + base;
  const int64_t hi = int64_t(index_hi) + base;
  // The translated vertex index is masked to 24 bits before clamping.
  // A wrapped interval cannot be represented by one conservative span.
  if (lo < 0 || hi > 0xFFFFFF) return false;
  const uint32_t first = std::clamp(uint32_t(lo), clamp_lo, clamp_hi);
  const uint32_t last = std::clamp(uint32_t(hi), clamp_lo, clamp_hi);
  const uint64_t begin = uint64_t(first) * stride;
  const uint64_t end = uint64_t(last) * stride + extent;
  // Fall back to the original full-range path for out-of-bounds fetches.
  if (begin >= end || end > bytes || end - begin >= bytes) return false;
  span = {uint32_t(begin), uint32_t(end - begin)};
  return true;
}

// Plan placement without advancing the live ring until Allocate validates
// its in-flight submissions and unfinished-draw pins. A span's rebased fetch
// address must not underflow when subtracting its offset in the guest range.
inline uint64_t VertexStreamStart(uint64_t head, uint64_t size, uint64_t alignment,
                                  uint64_t capacity, uint64_t guard, uint64_t min_offset) {
  auto align = [alignment](uint64_t v) { return (v + alignment - 1) & ~(alignment - 1); };
  uint64_t start = align(head);
  if (start % capacity < min_offset) start = align(start - start % capacity + min_offset);
  if (start % capacity + size > capacity - guard) {
    start = align((start / capacity + 1) * capacity + min_offset);
  }
  return start;
}

}  // namespace fear_nx::native
