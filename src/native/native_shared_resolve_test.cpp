// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <optional>
#include <vector>

#include "native_resolve_layout.h"

using fear_nx::native::resolve_layout::CopyRegion;
using fear_nx::native::resolve_layout::PlanSharedWrite;
using fear_nx::native::resolve_layout::Point;
using fear_nx::native::resolve_layout::SharedWrite;
using fear_nx::native::resolve_layout::TileAddressOffset;

namespace {

constexpr uint32_t kPitch = 1280;
constexpr uint32_t kHeight = 720;
constexpr uint32_t kRootAddress = 0x1EB30000;
constexpr uint32_t kRightAddress = 0x1EB44000;

std::optional<SharedWrite> Plan(uint32_t address, uint32_t pitch,
                                uint32_t height, Point origin,
                                bool window_mapping_valid, uint32_t source_x,
                                uint32_t source_y, uint32_t width,
                                uint32_t copy_height, uint32_t source_width,
                                uint32_t source_height) {
  return PlanSharedWrite(address, pitch, height, origin, window_mapping_valid,
                         source_x, source_y, width, copy_height, source_width,
                         source_height);
}

void ApplyCopy(std::vector<uint8_t> &destination,
               const std::vector<uint8_t> &source, uint32_t destination_pitch,
               uint32_t source_pitch, const CopyRegion &region) {
  for (uint32_t y = 0; y < region.height; ++y) {
    for (uint32_t x = 0; x < region.width; ++x) {
      destination[(region.destination_y + y) * destination_pitch +
                  region.destination_x + x] =
          source[(region.source_y + y) * source_pitch + region.source_x + x];
    }
  }
}

} // namespace

int main() {
  // Hardware fixture: a 1280x720 shared target split into a 768-pixel root
  // write and a 640-pixel segment at x=640. The final 128 pixels overlap.
  const auto right_offset = TileAddressOffset({640, 0}, kPitch, kHeight, 2);
  assert(right_offset && *right_offset == 0x14000);
  const auto root = Plan(kRootAddress, kPitch, kHeight, {0, 0}, true, 0, 0, 768,
                         720, 768, 720);
  const auto right = Plan(kRightAddress, kPitch, kHeight, {640, 0}, true, 0, 0,
                          640, 720, 640, 720);
  assert(root && root->root_address == kRootAddress);
  assert(root->width == kPitch && root->height == kHeight);
  assert(root->region.destination_x == 0 && root->region.destination_y == 0);
  assert(right && right->root_address == kRootAddress);
  assert(right->origin.x == 640 && right->origin.y == 0);
  assert(right->region.destination_x == 640 && right->region.width == 640);

  // Root first, right segment second: the segment correctly wins the overlap.
  std::vector<uint8_t> frame(kPitch * kHeight, 0);
  std::vector<uint8_t> root_pixels(768 * kHeight, 0x11);
  std::vector<uint8_t> right_pixels(640 * kHeight, 0x22);
  ApplyCopy(frame, root_pixels, kPitch, 768, root->region);
  ApplyCopy(frame, right_pixels, kPitch, 640, right->region);
  assert(frame[0] == 0x11);
  assert(frame[639] == 0x11);
  assert(frame[640] == 0x22);
  assert(frame[767] == 0x22);
  assert(frame[768] == 0x22);
  assert(frame[kPitch - 1] == 0x22);
  assert(frame[(kHeight - 1) * kPitch + kPitch - 1] == 0x22);

  // Reversing the writes demonstrates why preserving submission order matters:
  // the later root update overwrites the first 128 pixels of the right segment.
  std::fill(frame.begin(), frame.end(), 0);
  ApplyCopy(frame, right_pixels, kPitch, 640, right->region);
  ApplyCopy(frame, root_pixels, kPitch, 768, root->region);
  assert(frame[639] == 0x11);
  assert(frame[640] == 0x11);
  assert(frame[767] == 0x11);
  assert(frame[768] == 0x22);
  assert(frame[kPitch - 1] == 0x22);

  // Partial source rectangles retain their offset inside the source and add
  // the tile origin when mapping to the destination.
  const auto partial = Plan(kRightAddress, kPitch, kHeight, {640, 0}, true, 32,
                            12, 300, 20, 640, 720);
  assert(partial && partial->root_address == kRootAddress);
  assert(partial->region.source_x == 32 && partial->region.source_y == 12);
  assert(partial->region.destination_x == 672 &&
         partial->region.destination_y == 12);
  assert(partial->region.width == 300 && partial->region.height == 20);

  // Vertical tile origins are recovered from the adjusted physical address.
  const auto row_one_offset = TileAddressOffset({0, 32}, kPitch, kHeight, 2);
  const auto row_two_offset = TileAddressOffset({0, 64}, kPitch, kHeight, 2);
  assert(row_one_offset && *row_one_offset == 0x28000);
  assert(row_two_offset && *row_two_offset == 0x50000);
  const auto row_one = Plan(kRootAddress + *row_one_offset, kPitch, kHeight,
                            {0, 32}, true, 0, 5, 64, 10, 64, 720);
  const auto row_two = Plan(kRootAddress + *row_two_offset, kPitch, kHeight,
                            {0, 64}, true, 16, 32, 64, 10, 128, 720);
  assert(row_one && row_one->root_address == kRootAddress);
  assert(row_one->region.destination_y == 37);
  assert(row_two && row_two->root_address == kRootAddress);
  assert(row_two->region.destination_x == 16 &&
         row_two->region.destination_y == 96);

  // A rejected mapping/scissor must never be guessed into a shared write.
  assert(!Plan(kRootAddress, kPitch, kHeight, {0, 0}, false, 0, 0, 768, 720,
               768, 720));

  // Invalid physical bases, including address subtraction underflow and
  // non-page-aligned guest addresses, are rejected.
  assert(!Plan(0, kPitch, kHeight, {0, 0}, true, 0, 0, 1, 1, 1, 1));
  assert(
      !Plan(kRootAddress + 4, kPitch, kHeight, {0, 0}, true, 0, 0, 1, 1, 1, 1));
  assert(!Plan(0x1000, kPitch, kHeight, {640, 0}, true, 0, 0, 1, 1, 1, 1));

  // The aperture is end-exclusive: an image ending exactly at 512 MiB fits,
  // while moving its root forward by one page does not.
  constexpr uint32_t kMaxPitch = 8192;
  constexpr uint32_t kMaxHeight = 8192;
  const auto aperture_edge =
      Plan(0x10000000, kMaxPitch, kMaxHeight, {0, 0}, true, 0, 0, 1, 1, 1, 1);
  assert(aperture_edge && aperture_edge->root_address == 0x10000000);
  assert(
      !Plan(0x10001000, kMaxPitch, kMaxHeight, {0, 0}, true, 0, 0, 1, 1, 1, 1));

  // Reject invalid extents, pitch alignment, tile origins and copy bounds.
  assert(!Plan(kRootAddress, 0, kHeight, {0, 0}, true, 0, 0, 1, 1, 1, 1));
  assert(!Plan(kRootAddress, 1279, kHeight, {0, 0}, true, 0, 0, 1, 1, 1, 1));
  assert(!Plan(kRootAddress, 8224, kHeight, {0, 0}, true, 0, 0, 1, 1, 1, 1));
  assert(!Plan(kRootAddress, kPitch, 0, {0, 0}, true, 0, 0, 1, 1, 1, 1));
  assert(!Plan(kRootAddress, kPitch, 8193, {0, 0}, true, 0, 0, 1, 1, 1, 1));
  assert(!Plan(kRootAddress, kPitch, kHeight, {1, 0}, true, 0, 0, 1, 1, 1, 1));
  assert(!Plan(kRootAddress, kPitch, kHeight, {0, 1}, true, 0, 0, 1, 1, 1, 1));
  assert(!Plan(kRootAddress, kPitch, kHeight, {kPitch, 0}, true, 0, 0, 1, 1, 1,
               1));
  assert(!Plan(kRootAddress, kPitch, kHeight, {0, kHeight}, true, 0, 0, 1, 1, 1,
               1));
  assert(!Plan(kRootAddress, kPitch, kHeight, {0, 0}, true, 0, 0, 0, 1, 1, 1));
  assert(!Plan(kRootAddress, kPitch, kHeight, {0, 0}, true, 0, 0, 1, 0, 1, 1));
  assert(!Plan(kRootAddress, kPitch, kHeight, {0, 0}, true, 1, 0, 1, 1, 1, 1));
  assert(!Plan(kRootAddress, kPitch, kHeight, {0, 0}, true, 0, 1, 1, 1, 1, 1));
  assert(!Plan(kRootAddress, kPitch, kHeight, {0, 0}, true, 0, 0, 1, 1, 0, 1));
  assert(!Plan(kRootAddress, kPitch, kHeight, {0, 0}, true, 0, 0, 1, 1, 1, 0));
  assert(!Plan(kRootAddress, kPitch, kHeight, {640, 0}, true, 640, 0, 1, 1, 700,
               720)); // destination x would be 1280
  const auto row_twenty_two_offset =
      TileAddressOffset({0, 704}, kPitch, kHeight, 2);
  assert(row_twenty_two_offset);
  assert(!Plan(kRootAddress + *row_twenty_two_offset, kPitch, kHeight, {0, 704},
               true, 0, 16, 1, 1, 1, 720)); // destination y would be 720
}