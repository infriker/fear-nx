// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_zcull.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

namespace {

struct DepthStencil {
  uint32_t depth;
  uint8_t stencil;

  bool operator==(const DepthStencil& other) const {
    return depth == other.depth && stencil == other.stencil;
  }
};

struct TestCase {
  const char* name;
  int32_t x;
  int32_t y;
  uint32_t width;
  uint32_t height;
  uint32_t pass_width;
  uint32_t pass_height;
  bool can_discard;
  bool fully_covers;
};

std::vector<DepthStencil> MakeOldImage(uint32_t width, uint32_t height,
                                       uint32_t sample_count) {
  std::vector<DepthStencil> image(std::size_t(width) * height * sample_count);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      for (uint32_t sample = 0; sample < sample_count; ++sample) {
        const std::size_t index = (std::size_t(y) * width + x) * sample_count + sample;
        image[index] = {0x10000000u + y * 0x10000u + x * 0x100u + sample,
                        uint8_t((x * 13u + y * 29u + sample * 47u) & 0xFFu)};
      }
    }
  }
  return image;
}

// Model vkCmdClearAttachments clipped by both the rendering area and the
// image. The clear updates every sample of each covered pixel.
void ApplyClear(std::vector<DepthStencil>& image, uint32_t image_width,
               uint32_t image_height, uint32_t sample_count, const TestCase& test,
               DepthStencil clear_value) {
  for (uint32_t y = 0; y < image_height; ++y) {
    if (y >= test.pass_height) continue;
    const int64_t py = int64_t(y);
    if (py < int64_t(test.y) || py >= int64_t(test.y) + int64_t(test.height)) continue;
    for (uint32_t x = 0; x < image_width; ++x) {
      if (x >= test.pass_width) continue;
      const int64_t px = int64_t(x);
      if (px < int64_t(test.x) || px >= int64_t(test.x) + int64_t(test.width)) continue;
      for (uint32_t sample = 0; sample < sample_count; ++sample) {
        image[(std::size_t(y) * image_width + x) * sample_count + sample] = clear_value;
      }
    }
  }
}

bool ClearActuallyCoversImage(uint32_t image_width, uint32_t image_height,
                              const TestCase& test) {
  for (uint32_t y = 0; y < image_height; ++y) {
    if (y >= test.pass_height || int64_t(y) < int64_t(test.y) ||
        int64_t(y) >= int64_t(test.y) + int64_t(test.height)) {
      return false;
    }
    for (uint32_t x = 0; x < image_width; ++x) {
      if (x >= test.pass_width || int64_t(x) < int64_t(test.x) ||
          int64_t(x) >= int64_t(test.x) + int64_t(test.width)) {
        return false;
      }
    }
  }
  return image_width != 0 && image_height != 0;
}

void CheckClearReplacement(uint32_t width, uint32_t height,
                           uint32_t sample_count, const TestCase& test) {
  const bool actual = fear_nx::native::CanDiscardDepthForClear(
      width, height, test.x, test.y, test.width, test.height,
      test.pass_width, test.pass_height);
  assert(actual == test.can_discard);

  const bool coverage = ClearActuallyCoversImage(width, height, test);
  if (test.fully_covers) {
    assert(coverage);
  } else {
    assert(!coverage);
  }

  constexpr DepthStencil kClearValue{0x5A123456u, 0xA7u};
  const std::vector<DepthStencil> old_image = MakeOldImage(width, height, sample_count);

  // Baseline: allocate a replacement, copy the old depth+stencil samples, then
  // execute the guest clear.
  std::vector<DepthStencil> full_refresh = old_image;
  ApplyClear(full_refresh, width, height, sample_count, test, kClearValue);

  // Optimized route: skip the preservation copy only when the helper proves
  // the clear replaces the whole allocated image. Otherwise preserve old data.
  std::vector<DepthStencil> optimized(std::size_t(width) * height * sample_count,
                                      {0xDEADBEEFu, 0x5Cu});
  if (!actual) optimized = old_image;
  ApplyClear(optimized, width, height, sample_count, test, kClearValue);
  assert(optimized == full_refresh);

  if (!coverage && width && height) {
    // Prove that skipping preservation on a rejected partial clear would lose
    // depth or stencil values that the guest did not overwrite.
    std::vector<DepthStencil> no_copy(optimized.size(), {0xDEADBEEFu, 0x5Cu});
    ApplyClear(no_copy, width, height, sample_count, test, kClearValue);
    assert(no_copy != full_refresh);
  }
}

void TestSmallImagesAndSamples() {
  constexpr uint32_t kWidth = 5;
  constexpr uint32_t kHeight = 3;
  const TestCase cases[] = {
      {"exact full clear", 0, 0, kWidth, kHeight, kWidth, kHeight, true, true},
      {"oversized clear clipped by complete pass", 0, 0, 99, 99, 7, 6, true, true},
      {"positive x offset", 1, 0, kWidth, kHeight, 7, 6, false, false},
      {"positive y offset", 0, 1, kWidth, kHeight, 7, 6, false, false},
      // Full after clipping, but conservatively rejected because the origin
      // is offset and the current production gate only accepts (0, 0).
      {"negative x offset fully covers", -1, 0, kWidth + 1, kHeight, 7, 6, false, true},
      {"negative y offset fully covers", 0, -1, kWidth, kHeight + 1, 7, 6, false, true},
      {"partial right edge", 0, 0, kWidth - 1, kHeight, 7, 6, false, false},
      {"partial bottom edge", 0, 0, kWidth, kHeight - 1, 7, 6, false, false},
      {"pass clips right edge", 0, 0, kWidth, kHeight, kWidth - 1, kHeight, false, false},
      {"pass clips bottom edge", 0, 0, kWidth, kHeight, kWidth, kHeight - 1, false, false},
      {"empty width", 0, 0, 0, kHeight, 7, 6, false, false},
      {"empty height", 0, 0, kWidth, 0, 7, 6, false, false},
  };

  constexpr uint32_t kSampleCounts[] = {1, 2, 4};
  for (uint32_t samples : kSampleCounts) {
    for (const TestCase& test : cases) {
      CheckClearReplacement(kWidth, kHeight, samples, test);
    }
  }
}

void TestPaddedImageRows() {
  // The render target is taller than the guest-visible clear. Padded rows are
  // still part of the allocation and must be cleared before old data is lost.
  CheckClearReplacement(4, 16, 4,
                       {"guest-height only", 0, 0, 4, 8, 4, 16, false, false});
  CheckClearReplacement(4, 16, 4,
                       {"full allocated extent", 0, 0, 4, 16, 4, 16, true, true});
  CheckClearReplacement(4, 16, 2,
                       {"pass clips padded rows", 0, 0, 4, 16, 4, 15, false, false});
}

void TestZeroAndMaximumBounds() {
  assert(!fear_nx::native::CanDiscardDepthForClear(0, 1, 0, 0, 1, 1, 1, 1));
  assert(!fear_nx::native::CanDiscardDepthForClear(1, 0, 0, 0, 1, 1, 1, 1));

  constexpr uint32_t kMax = std::numeric_limits<uint32_t>::max();
  assert(fear_nx::native::CanDiscardDepthForClear(kMax, kMax, 0, 0, kMax, kMax,
                                                  kMax, kMax));
  assert(fear_nx::native::CanDiscardDepthForClear(5, 3, 0, 0, kMax, kMax, 5, 3));
  // A 32-bit x+width calculation would wrap here and could incorrectly accept
  // a rectangle whose nonzero origin leaves the first column untouched.
  assert(!fear_nx::native::CanDiscardDepthForClear(kMax, 3, 1, 0, kMax, 3,
                                                   kMax, 3));
  assert(!fear_nx::native::CanDiscardDepthForClear(5, 3, 0, 0, kMax, kMax,
                                                   4, 3));
}

}  // namespace

int main() {
  TestSmallImagesAndSamples();
  TestPaddedImageRows();
  TestZeroAndMaximumBounds();
  std::cout << "native_zcull_test: passed\n";
  return 0;
}
