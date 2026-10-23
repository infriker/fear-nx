// SPDX-License-Identifier: GPL-3.0-or-later

#include "native_attachment_clear.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <vector>

namespace {

using fear_nx::native::CanUseAttachmentLoadClear;

static_assert(CanUseAttachmentLoadClear(0, 0, 8, 6, 8, 6));
static_assert(CanUseAttachmentLoadClear(0, 0, 9, 7, 8, 6));
static_assert(!CanUseAttachmentLoadClear(0, 0, 8, 5, 8, 6));
static_assert(!CanUseAttachmentLoadClear(1, 0, 9, 6, 8, 6));
static_assert(!CanUseAttachmentLoadClear(0, -1, 8, 7, 8, 6));
static_assert(!CanUseAttachmentLoadClear(0, 0, 8, 6, 0, 6));
static_assert(!CanUseAttachmentLoadClear(0, 0, 8, 6, 8, 0));
static_assert(CanUseAttachmentLoadClear(0, 0, UINT32_MAX, UINT32_MAX,
                                        UINT32_MAX, UINT32_MAX));

void Expect(bool condition) {
  if (!condition) {
    std::abort();
  }
}

struct Rect {
  int32_t x;
  int32_t y;
  uint32_t width;
  uint32_t height;
};

struct ClearValue {
  uint32_t color;
  uint32_t depth_bits;
  uint8_t stencil;
};

struct Sample {
  uint32_t color;
  uint32_t depth_bits;
  uint8_t stencil;
};

bool operator==(const Sample& left, const Sample& right) {
  return left.color == right.color && left.depth_bits == right.depth_bits &&
         left.stencil == right.stencil;
}

struct Image {
  uint32_t width;
  uint32_t height;
  uint32_t samples_per_pixel;
  std::vector<Sample> samples;

  Image(uint32_t image_width, uint32_t image_height, uint32_t sample_count)
      : width(image_width),
        height(image_height),
        samples_per_pixel(sample_count),
        samples(static_cast<std::size_t>(image_width) * image_height *
                sample_count) {
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        for (uint32_t sample = 0; sample < samples_per_pixel; ++sample) {
          const std::size_t index = Index(x, y, sample);
          const uint32_t unique = static_cast<uint32_t>(index);
          samples[index] = Sample{0xA5000000u ^ unique,
                                  0x3F000000u ^ (unique * 2654435761u),
                                  static_cast<uint8_t>(unique ^ 0x5Au)};
        }
      }
    }
  }

  std::size_t Index(uint32_t x, uint32_t y, uint32_t sample) const {
    return (static_cast<std::size_t>(y) * width + x) * samples_per_pixel +
           sample;
  }

  Sample& At(uint32_t x, uint32_t y, uint32_t sample) {
    return samples[Index(x, y, sample)];
  }

  const Sample& At(uint32_t x, uint32_t y, uint32_t sample) const {
    return samples[Index(x, y, sample)];
  }
};

void ApplyClear(Sample& sample, const ClearValue& clear) {
  sample.color = clear.color;
  sample.depth_bits = clear.depth_bits;
  sample.stencil = clear.stencil;
}

// Model vkCmdClearAttachments with the clear rect clipped to the active render
// area and the image allocation. int64_t safely holds int32 offsets plus any
// uint32 extent, including the deliberately oversized test rectangles.
void ApplyRectClear(Image& image, uint32_t render_width,
                    uint32_t render_height, const Rect& rect,
                    const ClearValue& clear) {
  const int64_t left = std::max<int64_t>(0, rect.x);
  const int64_t top = std::max<int64_t>(0, rect.y);
  const int64_t right = std::min<int64_t>(
      std::min<uint32_t>(image.width, render_width),
      static_cast<int64_t>(rect.x) + rect.width);
  const int64_t bottom = std::min<int64_t>(
      std::min<uint32_t>(image.height, render_height),
      static_cast<int64_t>(rect.y) + rect.height);

  for (int64_t y = top; y < bottom; ++y) {
    for (int64_t x = left; x < right; ++x) {
      for (uint32_t sample = 0; sample < image.samples_per_pixel; ++sample) {
        ApplyClear(image.At(static_cast<uint32_t>(x),
                            static_cast<uint32_t>(y), sample),
                   clear);
      }
    }
  }
}

// A loadOp=CLEAR affects samples in the active render area, not the rest of
// the image allocation. The image bounds model attachment clipping.
void ApplyLoadClear(Image& image, uint32_t render_width,
                    uint32_t render_height, const ClearValue& clear) {
  const uint32_t active_width = std::min(image.width, render_width);
  const uint32_t active_height = std::min(image.height, render_height);
  for (uint32_t y = 0; y < active_height; ++y) {
    for (uint32_t x = 0; x < active_width; ++x) {
      for (uint32_t sample = 0; sample < image.samples_per_pixel; ++sample) {
        ApplyClear(image.At(x, y, sample), clear);
      }
    }
  }
}

void ExpectImagesEqual(const Image& left, const Image& right) {
  Expect(left.width == right.width);
  Expect(left.height == right.height);
  Expect(left.samples_per_pixel == right.samples_per_pixel);
  Expect(left.samples.size() == right.samples.size());
  for (std::size_t i = 0; i < left.samples.size(); ++i) {
    Expect(left.samples[i] == right.samples[i]);
  }
}

void ExpectOutsideRenderAreaUnchanged(const Image& before, const Image& after,
                                      uint32_t render_width,
                                      uint32_t render_height) {
  const uint32_t active_width = std::min(before.width, render_width);
  const uint32_t active_height = std::min(before.height, render_height);
  for (uint32_t y = 0; y < before.height; ++y) {
    for (uint32_t x = 0; x < before.width; ++x) {
      if (x < active_width && y < active_height) {
        continue;
      }
      for (uint32_t sample = 0; sample < before.samples_per_pixel; ++sample) {
        Expect(before.At(x, y, sample) == after.At(x, y, sample));
      }
    }
  }
}

void CheckStrategyMatchesRectClear(uint32_t image_width, uint32_t image_height,
                                   uint32_t render_width,
                                   uint32_t render_height, const Rect& rect,
                                   uint32_t samples, bool expected_load_clear) {
  const bool use_load_clear = CanUseAttachmentLoadClear(
      rect.x, rect.y, rect.width, rect.height, render_width, render_height);
  Expect(use_load_clear == expected_load_clear);

  const ClearValue clear{0x11223344u, 0x3E800000u, 0xC7u};
  const Image initial(image_width, image_height, samples);

  // Reference behavior: LOAD followed by a clipped VkClearAttachments rect.
  Image reference = initial;
  ApplyRectClear(reference, render_width, render_height, rect, clear);

  // Candidate behavior: CLEAR at attachment load only for a covering rect;
  // otherwise retain LOAD plus the original clipped rectangle clear.
  Image candidate = initial;
  if (use_load_clear) {
    ApplyLoadClear(candidate, render_width, render_height, clear);
  } else {
    ApplyRectClear(candidate, render_width, render_height, rect, clear);
  }

  ExpectImagesEqual(reference, candidate);
  ExpectOutsideRenderAreaUnchanged(initial, candidate, render_width,
                                   render_height);
}

void TestFullAllocationAndSmallerRenderArea() {
  for (uint32_t samples : {1u, 2u, 4u}) {
    CheckStrategyMatchesRectClear(8, 6, 8, 6, Rect{0, 0, 8, 6}, samples,
                                  true);

    // The clear covers the active area but the allocation has extra columns
    // and rows. loadOp=CLEAR must leave those samples bit-for-bit unchanged.
    CheckStrategyMatchesRectClear(11, 9, 7, 5, Rect{0, 0, 7, 5}, samples,
                                  true);

    // A larger clear rectangle still has the same effect after clipping to
    // the smaller active render area.
    CheckStrategyMatchesRectClear(10, 8, 6, 4,
                                  Rect{0, 0, UINT32_MAX, UINT32_MAX}, samples,
                                  true);

    // Padded allocation rows model render targets with unused bottom padding.
    CheckStrategyMatchesRectClear(8, 12, 8, 7, Rect{0, 0, 8, 7}, samples,
                                  true);
  }
}

void TestPartialAndOffsetRectsKeepFallback() {
  const Rect fallback_rects[] = {
      {0, 0, 7, 5},  // misses the final row
      {0, 0, 6, 6},  // misses the final column
      {0, 1, 7, 5},  // shifted down
      {1, 0, 6, 6},  // shifted right
      {-1, 0, 8, 6}, // nonzero origin, despite a wide extent
      {0, -1, 7, 7}, // nonzero origin, despite a tall extent
      {9, 8, 4, 4},  // disjoint from the active area
      {0, 0, 0, 6},  // empty rectangle
      {0, 0, 7, 0},  // empty rectangle
  };

  for (uint32_t samples : {1u, 2u, 4u}) {
    for (const Rect& rect : fallback_rects) {
      CheckStrategyMatchesRectClear(10, 9, 7, 6, rect, samples, false);
    }

    // Empty/default render areas never select a load clear.
    CheckStrategyMatchesRectClear(10, 9, 0, 6, Rect{0, 0, 7, 6}, samples,
                                  false);
    CheckStrategyMatchesRectClear(10, 9, 7, 0, Rect{0, 0, 7, 6}, samples,
                                  false);
  }
}

}  // namespace

int main() {
  TestFullAllocationAndSmallerRenderArea();
  TestPartialAndOffsetRectsKeepFallback();
  return 0;
}
