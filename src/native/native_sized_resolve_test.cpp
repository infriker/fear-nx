// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "native_resolve_layout.h"

namespace resolve_layout = fear_nx::native::resolve_layout;

namespace {

using resolve_layout::CopyRegion;
using resolve_layout::Point;
using resolve_layout::Rect;

struct Image {
  uint32_t width;
  uint32_t height;
  std::vector<uint32_t> pixels;

  Image(uint32_t w, uint32_t h, uint32_t initial = 0)
      : width(w), height(h), pixels(std::size_t(w) * h, initial) {}

  uint32_t& At(uint32_t x, uint32_t y) {
    return pixels[std::size_t(y) * width + x];
  }
  uint32_t At(uint32_t x, uint32_t y) const {
    return pixels[std::size_t(y) * width + x];
  }
};

void ApplyWrite(Image& source, Rect write, uint32_t value) {
  const uint64_t left = std::min<uint64_t>(write.x, source.width);
  const uint64_t top = std::min<uint64_t>(write.y, source.height);
  const uint64_t right = std::min<uint64_t>(uint64_t(write.x) + write.width, source.width);
  const uint64_t bottom = std::min<uint64_t>(uint64_t(write.y) + write.height, source.height);
  for (uint64_t y = top; y < bottom; ++y) {
    for (uint64_t x = left; x < right; ++x) {
      source.At(uint32_t(x), uint32_t(y)) = value;
    }
  }
}

void ApplyCopy(const Image& source, Image& destination, const CopyRegion& copy) {
  assert(uint64_t(copy.source_x) + copy.width <= source.width);
  assert(uint64_t(copy.source_y) + copy.height <= source.height);
  assert(uint64_t(copy.destination_x) + copy.width <= destination.width);
  assert(uint64_t(copy.destination_y) + copy.height <= destination.height);
  for (uint32_t y = 0; y < copy.height; ++y) {
    for (uint32_t x = 0; x < copy.width; ++x) {
      destination.At(copy.destination_x + x, copy.destination_y + y) =
          source.At(copy.source_x + x, copy.source_y + y);
    }
  }
}

// Build the full-refresh baseline directly from source pixels. This does not
// use the planner or CopyRegion, so it remains independent of the optimization.
void RefreshFullCrop(const Image& source, Image& destination, Point crop_origin) {
  for (uint32_t y = 0; y < destination.height; ++y) {
    for (uint32_t x = 0; x < destination.width; ++x) {
      const uint64_t source_x = uint64_t(crop_origin.x) + x;
      const uint64_t source_y = uint64_t(crop_origin.y) + y;
      if (source_x < source.width && source_y < source.height) {
        destination.At(x, y) = source.At(uint32_t(source_x), uint32_t(source_y));
      }
    }
  }
}

void TestPlanningAndOverflow() {
  // The crop begins inside the source and ends at its right and bottom edges.
  // A first fill must initialize every available pixel, even if the write
  // that caused allocation is entirely outside this crop.
  const Point crop{3, 2};
  auto first_fill = resolve_layout::PlanSizedCopy(
      10, 8, crop, 6, 5, false, Rect{0, 0, 1, 1});
  assert(first_fill && first_fill->source_x == 3 && first_fill->source_y == 2 &&
         first_fill->destination_x == 0 && first_fill->destination_y == 0 &&
         first_fill->width == 6 && first_fill->height == 5);

  // Once initialized, a disjoint or zero-area dirty rectangle is a no-op.
  assert(!resolve_layout::PlanSizedCopy(10, 8, crop, 6, 5, true, Rect{0, 0, 1, 1}));
  assert(!resolve_layout::PlanSizedCopy(10, 8, crop, 6, 5, true, Rect{4, 3, 0, 2}));
  assert(!resolve_layout::PlanSizedCopy(10, 8, crop, 6, 5, true, Rect{4, 3, 2, 0}));

  // No dirty rectangle means a full available crop refresh, including after
  // initialization.
  auto full_refresh = resolve_layout::PlanSizedCopy(10, 8, crop, 6, 5, true);
  assert(full_refresh && full_refresh->width == 6 && full_refresh->height == 5);

  // Dirty rectangles are in absolute source coordinates. Test a crop-local
  // destination origin, an interior rectangle, and clipping at each crop edge.
  auto interior = resolve_layout::PlanSizedCopy(10, 8, crop, 6, 5, true,
                                                 Rect{4, 3, 2, 2});
  assert(interior && interior->source_x == 4 && interior->source_y == 3 &&
         interior->destination_x == 1 && interior->destination_y == 1 &&
         interior->width == 2 && interior->height == 2);

  auto clipped_left_top = resolve_layout::PlanSizedCopy(
      10, 8, crop, 6, 5, true, Rect{0, 1, 5, 4});
  assert(clipped_left_top && clipped_left_top->source_x == 3 &&
         clipped_left_top->source_y == 2 && clipped_left_top->destination_x == 0 &&
         clipped_left_top->destination_y == 0 && clipped_left_top->width == 2 &&
         clipped_left_top->height == 3);

  auto clipped_right_bottom = resolve_layout::PlanSizedCopy(
      10, 8, crop, 6, 5, true, Rect{7, 4, 5, 5});
  assert(clipped_right_bottom && clipped_right_bottom->source_x == 7 &&
         clipped_right_bottom->source_y == 4 &&
         clipped_right_bottom->destination_x == 4 &&
         clipped_right_bottom->destination_y == 2 && clipped_right_bottom->width == 2 &&
         clipped_right_bottom->height == 3);

  // A requested crop may extend past the source. Only its source-backed area
  // can be copied; the destination coordinates stay relative to the crop.
  auto partial_crop = resolve_layout::PlanSizedCopy(5, 4, Point{3, 1}, 5, 5, false);
  assert(partial_crop && partial_crop->source_x == 3 && partial_crop->source_y == 1 &&
         partial_crop->destination_x == 0 && partial_crop->destination_y == 0 &&
         partial_crop->width == 2 && partial_crop->height == 3);

  // uint32_t endpoint arithmetic would wrap for these rectangles. The valid
  // source is only UINT32_MAX pixels in each dimension, so the final row and
  // column remain one pixel wide/high.
  constexpr uint32_t kMax = std::numeric_limits<uint32_t>::max();
  const Point near_max{kMax - 2, kMax - 3};
  auto max_full = resolve_layout::PlanSizedCopy(kMax, kMax, near_max, kMax, kMax, false);
  assert(max_full && max_full->source_x == kMax - 2 && max_full->source_y == kMax - 3 &&
         max_full->width == 2 && max_full->height == 3);
  auto max_dirty = resolve_layout::PlanSizedCopy(
      kMax, kMax, near_max, kMax, kMax, true, Rect{kMax - 1, kMax - 1, kMax, kMax});
  assert(max_dirty && max_dirty->source_x == kMax - 1 &&
         max_dirty->source_y == kMax - 1 && max_dirty->destination_x == 1 &&
         max_dirty->destination_y == 2 && max_dirty->width == 1 &&
         max_dirty->height == 1);

  // Empty dimensions and origins outside the source have no available crop.
  assert(!resolve_layout::PlanSizedCopy(0, 8, crop, 6, 5, false));
  assert(!resolve_layout::PlanSizedCopy(10, 0, crop, 6, 5, false));
  assert(!resolve_layout::PlanSizedCopy(10, 8, crop, 0, 5, false));
  assert(!resolve_layout::PlanSizedCopy(10, 8, crop, 6, 0, false));
  assert(!resolve_layout::PlanSizedCopy(10, 8, Point{10, 2}, 1, 1, false));
  assert(!resolve_layout::PlanSizedCopy(10, 8, Point{3, 8}, 1, 1, false));
}

void TestIncrementalPixelsAndConsecutiveWrites() {
  constexpr uint32_t kSourceWidth = 12;
  constexpr uint32_t kSourceHeight = 9;
  constexpr uint32_t kCropWidth = 6;
  constexpr uint32_t kCropHeight = 5;
  constexpr uint32_t kPixelSentinel = 0xDEADBEEFu;

  struct Write {
    Rect rect;
    uint32_t value;
  };
  // Mimic several tiled writes to the same shared destination in one frame:
  // first a disjoint source write forces initial fill, then partial-edge,
  // overlapping, and interior updates arrive in submission order.
  std::vector<Write> writes = {
      {{0, 0, 1, 1}, 11},
      {{7, 4, 5, 5}, 22},
      {{8, 5, 2, 3}, 33},
      {{2, 6, 3, 5}, 44},
      {{4, 3, 2, 1}, 55},
  };

  // Add a reproducible sequence of small writes so the pixel comparison covers
  // varied overlap and clipping patterns without relying on the planner for an
  // expected result.
  uint32_t random_state = 0x6D2B79F5u;
  for (uint32_t i = 0; i < 16; ++i) {
    random_state = random_state * 1664525u + 1013904223u;
    const uint32_t x = random_state % (kSourceWidth + 3);
    random_state = random_state * 1664525u + 1013904223u;
    const uint32_t y = random_state % (kSourceHeight + 3);
    random_state = random_state * 1664525u + 1013904223u;
    const uint32_t width = 1 + ((random_state >> 16) % 3);
    random_state = random_state * 1664525u + 1013904223u;
    const uint32_t height = 1 + ((random_state >> 16) % 3);
    writes.push_back({Rect{x, y, width, height}, 100 + i});
  }

  struct Crop {
    Point origin;
    uint32_t width;
    uint32_t height;
  };
  const Crop crops[] = {
      {{3, 2}, kCropWidth, kCropHeight},
      {{1, 1}, 4, 3},
      {{9, 6}, 5, 5},  // The requested crop extends beyond the source.
      {{0, 4}, 4, 5},
  };

  for (const Crop& crop : crops) {
    Image source(kSourceWidth, kSourceHeight);
    for (uint32_t y = 0; y < source.height; ++y) {
      for (uint32_t x = 0; x < source.width; ++x) {
        source.At(x, y) = 1000 + y * 32 + x;
      }
    }
    Image full_refresh(crop.width, crop.height, kPixelSentinel);
    Image incremental(crop.width, crop.height, kPixelSentinel);

    const uint32_t available_width =
        std::min(crop.width, kSourceWidth - crop.origin.x);
    const uint32_t available_height =
        std::min(crop.height, kSourceHeight - crop.origin.y);
    const uint64_t full_pixels_per_refresh =
        uint64_t(available_width) * available_height;
    bool filled = false;
    uint64_t incremental_pixels_copied = 0;

    for (const Write& write : writes) {
      ApplyWrite(source, write.rect, write.value);
      RefreshFullCrop(source, full_refresh, crop.origin);

      const auto update = resolve_layout::PlanSizedCopy(
          kSourceWidth, kSourceHeight, crop.origin, crop.width, crop.height,
          filled, write.rect);

      if (!filled) {
        // A first copy covers all source-backed pixels, even for a disjoint
        // write. An oversized crop must remain unfilled, as in TextureCache.
        const bool covers_available_crop =
            update && update->source_x == crop.origin.x &&
            update->source_y == crop.origin.y && update->destination_x == 0 &&
            update->destination_y == 0 && update->width == available_width &&
            update->height == available_height;
        assert(covers_available_crop);
        filled = update->width == crop.width && update->height == crop.height;
      }
      if (update) {
        ApplyCopy(source, incremental, *update);
        incremental_pixels_copied += uint64_t(update->width) * update->height;
      }
      assert(full_refresh.pixels == incremental.pixels);
    }

    const bool fits = available_width == crop.width && available_height == crop.height;
    assert(filled == fits);
    const uint64_t baseline_pixels_copied =
        uint64_t(writes.size()) * full_pixels_per_refresh;
    if (fits) {
      assert(incremental_pixels_copied < baseline_pixels_copied);
    } else {
      assert(incremental_pixels_copied == baseline_pixels_copied);
    }
  }
}

}  // namespace

int main() {
  TestPlanningAndOverflow();
  TestIncrementalPixelsAndConsecutiveWrites();
  return 0;
}
