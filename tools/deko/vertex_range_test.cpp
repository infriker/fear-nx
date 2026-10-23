// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../src/deko/deko_vertex_range.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <span>

namespace {

int failures = 0;

#define EXPECT(condition)                                                                                              \
  do {                                                                                                                 \
    if (!(condition)) {                                                                                                \
      std::cerr << __FILE__ << ':' << __LINE__ << ": expectation failed: " #condition "\n";                            \
      ++failures;                                                                                                      \
    }                                                                                                                  \
  } while (false)

using fear_nx::deko::FetchAccess;
using fear_nx::deko::IndexBounds;
using fear_nx::deko::UsedVertexPrefix;

void TestIndexTransformRestartAndClamp() {
  IndexBounds bounds;
  bounds.Include(0xFFFFFEu, 5u);  // 24-bit wrap after adding the base.
  bounds.Include(0x000009u, 5u);
  EXPECT(bounds.valid());
  EXPECT(bounds.min() == 3u);
  EXPECT(bounds.max() == 14u);

  IndexBounds restart;
  restart.Include(0xFFFFu, 200u, true, 0xFFFFu);  // Restart is skipped before adding 200.
  restart.Include(2u, 200u, true, 0xFFFFu);
  EXPECT(restart.valid());
  EXPECT(restart.min() == 202u);
  EXPECT(restart.max() == 202u);

  IndexBounds clamped;
  clamped.Include(4u, 0u);
  clamped.Include(60u, 0u);
  clamped.Clamp(10u, 50u);
  EXPECT(clamped.min() == 10u);
  EXPECT(clamped.max() == 50u);
  clamped.Clamp(51u, 50u);
  EXPECT(!clamped.valid());

  IndexBounds uint32_sum;
  uint32_sum.Include(std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max());
  EXPECT(uint32_sum.min() == 0xFFFFFEu);
}

void TestAutoDrawEndpointsAndWrapFallback() {
  const IndexBounds simple = IndexBounds::AutoDraw(10u, 4u, 5u, 0u, 100u);
  EXPECT(simple.valid());
  EXPECT(!simple.conservative());
  EXPECT(simple.min() == 15u);
  EXPECT(simple.max() == 18u);

  // The base moves this interval past the raw 24-bit edge without a wrap in
  // the transformed interval, so endpoint bounds remain exact.
  const IndexBounds shifted = IndexBounds::AutoDraw(0xFFFFFEu, 4u, 3u, 0u, 0xFFFFFFu);
  EXPECT(shifted.valid());
  EXPECT(!shifted.conservative());
  EXPECT(shifted.min() == 1u);
  EXPECT(shifted.max() == 4u);

  const IndexBounds wrapped = IndexBounds::AutoDraw(0xFFFFFEu, 4u, 0u, 7u, 99u);
  EXPECT(wrapped.valid());
  EXPECT(wrapped.conservative());
  EXPECT(wrapped.min() == 7u);
  EXPECT(wrapped.max() == 99u);

  const IndexBounds wide = IndexBounds::AutoDraw(0xFFFFFFF0u, 0x40u, 0u, 2u, 8u);
  EXPECT(wide.valid());
  EXPECT(wide.conservative());
  EXPECT(wide.min() == 2u);
  EXPECT(wide.max() == 8u);

  EXPECT(!IndexBounds::AutoDraw(0u, 0u, 0u, 0u, 10u).valid());
  EXPECT(!IndexBounds::AutoDraw(0u, 1u, 0u, 11u, 10u).valid());
}

void TestUsedPrefixAcrossAccesses() {
  IndexBounds bounds;
  bounds.Include(3u, 0u);
  bounds.Include(5u, 0u);
  const FetchAccess accesses[] = {
      {8u, 0b10100u, -1, false},  // words [25,44)
      {4u, 0b00011u, 0, false},   // words [12,22)
  };
  const auto range = UsedVertexPrefix(200u, bounds, accesses);
  EXPECT(range.valid);
  EXPECT(range.clipped);
  EXPECT(!range.conservative);
  EXPECT(range.first_byte == 48u);
  EXPECT(range.end_byte == 176u);
  EXPECT(range.prefix_bytes == 176u);
  EXPECT(range.used_bytes == 128u);
}

void TestZeroStrideAndEmptyMask() {
  IndexBounds bounds;
  bounds.Include(5u, 0u);
  bounds.Include(8u, 0u);
  const FetchAccess repeated{0u, 0b110u, 0, false};
  const auto repeated_range = UsedVertexPrefix(16u, bounds, std::span(&repeated, 1));
  EXPECT(repeated_range.valid);
  EXPECT(repeated_range.first_byte == 4u);
  EXPECT(repeated_range.end_byte == 12u);
  EXPECT(repeated_range.prefix_bytes == 12u);

  const FetchAccess empty{4u, 0u, 0, false};
  const auto empty_range = UsedVertexPrefix(16u, bounds, std::span(&empty, 1));
  EXPECT(!empty_range.valid);
  EXPECT(empty_range.prefix_bytes == 16u);
  EXPECT(empty_range.first_byte == 0u);
}

void TestUnsafeOrOutOfRangeAccessFallsBack() {
  IndexBounds bounds;
  bounds.Include(0u, 0u);
  bounds.Include(1u, 0u);

  const FetchAccess negative{1u, 0b1u, -2, false};
  const auto negative_range = UsedVertexPrefix(64u, bounds, std::span(&negative, 1));
  EXPECT(!negative_range.valid);
  EXPECT(negative_range.prefix_bytes == 64u);
  EXPECT(negative_range.first_byte == 0u);

  IndexBounds high;
  high.Include(2u, 0u);
  const FetchAccess too_small{4u, 0b1111u, 0, false};
  const auto small_range = UsedVertexPrefix(32u, high, std::span(&too_small, 1));
  EXPECT(!small_range.valid);
  EXPECT(small_range.prefix_bytes == 32u);

  IndexBounds huge;
  huge.Include(0xFFFFFFu, 0u);
  const FetchAccess huge_stride{std::numeric_limits<uint32_t>::max(), 0x80000000u, 0, false};
  const auto huge_range = UsedVertexPrefix(1u << 20, huge, std::span(&huge_stride, 1));
  EXPECT(!huge_range.valid);
  EXPECT(huge_range.prefix_bytes == (1u << 20));

  EXPECT(!UsedVertexPrefix(64u, IndexBounds{}, std::span(&negative, 1)).valid);
}

void TestFloatRoundedFetchIndices() {
  constexpr uint32_t even = 1u << 23;
  constexpr uint32_t odd = even + 1;
  const uint64_t descriptor_bytes = 64ull << 20;

  IndexBounds even_bounds;
  even_bounds.Include(even, 0u);
  const FetchAccess rounded{1u, 1u, 0, true};
  const auto even_range = UsedVertexPrefix(descriptor_bytes, even_bounds, std::span(&rounded, 1));
  EXPECT(even_range.valid);
  EXPECT(even_range.first_byte == uint64_t(even) * 4u);

  IndexBounds odd_bounds;
  odd_bounds.Include(odd, 0u);
  const auto odd_rounded_range = UsedVertexPrefix(descriptor_bytes, odd_bounds, std::span(&rounded, 1));
  EXPECT(odd_rounded_range.valid);
  EXPECT(odd_rounded_range.first_byte == uint64_t(even + 2u) * 4u);

  const FetchAccess exact{1u, 1u, 0, false};
  const auto odd_exact_range = UsedVertexPrefix(descriptor_bytes, odd_bounds, std::span(&exact, 1));
  EXPECT(odd_exact_range.valid);
  EXPECT(odd_exact_range.first_byte == uint64_t(odd) * 4u);
}

}  // namespace

int main() {
  TestIndexTransformRestartAndClamp();
  TestAutoDrawEndpointsAndWrapFallback();
  TestUsedPrefixAcrossAccesses();
  TestZeroStrideAndEmptyMask();
  TestUnsafeOrOutOfRangeAccessFallsBack();
  TestFloatRoundedFetchIndices();
  if (failures != 0) {
    std::cerr << failures << " vertex range test(s) failed\n";
    return 1;
  }
  std::cout << "vertex range tests passed\n";
  return 0;
}
