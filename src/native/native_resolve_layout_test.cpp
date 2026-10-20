// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <cstdint>
#include <vector>

#include <rex/graphics/pipeline/texture/util.h>

#include "native_resolve_layout.h"

using fear_nx::native::resolve_layout::CompositeState;
using fear_nx::native::resolve_layout::FindSegmentOrigin;
using fear_nx::native::resolve_layout::FindTileOrigin;
using fear_nx::native::resolve_layout::MakeCopyRegion;
using fear_nx::native::resolve_layout::TileAddressOffset;

int main() {
  constexpr uint32_t kPitch = 1280;
  constexpr uint32_t kHeight = 720;
  constexpr uint32_t kRootA = 0x1EB30000;
  constexpr uint32_t kRightA = 0x1EB44000;
  constexpr uint32_t kRootB = 0x1E798000;
  constexpr uint32_t kRightB = 0x1E7AC000;

  // The hardware trace has a 640-pixel right segment. Its address is exactly
  // the Xenos 32x32 tile offset for 640x0 at 32 bpp and a 1280-pixel pitch.
  const auto right_a = FindSegmentOrigin(kRootA, kRightA, kPitch, kHeight, 2);
  const auto right_b = FindSegmentOrigin(kRootB, kRightB, kPitch, kHeight, 2);
  assert(right_a && right_a->x == 640 && right_a->y == 0);
  assert(right_b && right_b->x == 640 && right_b->y == 0);
  assert(TileAddressOffset({640, 0}, kPitch, kHeight, 2) == 0x14000);
  assert(!FindSegmentOrigin(kRootA, kRightA, kPitch, kHeight, 1));
  assert(!FindSegmentOrigin(kRootA, kRightA, kPitch, kHeight, 0));
  assert(!FindSegmentOrigin(kRootA, kRightA, kPitch, kHeight, 4));
  assert(!FindSegmentOrigin(kRightA, kRootA, kPitch, kHeight, 2));
  assert(!FindSegmentOrigin(kRootA, kRightA + 0x100, kPitch, kHeight, 2));
  assert(!FindSegmentOrigin(kRootA, kRightB, kPitch, kHeight, 2));
  assert(!FindTileOrigin(0, 0, kHeight, 2));
  assert(!FindTileOrigin(0, kPitch, 0, 2));
  assert(!FindTileOrigin(0, kPitch, kHeight, 0));
  assert(!FindTileOrigin(0, kPitch, kHeight, 1));
  assert(!TileAddressOffset({0, 0}, 0, kHeight, 2));
  assert(!TileAddressOffset({0, 0}, kPitch, 0, 2));
  assert(!TileAddressOffset({0, 0}, kPitch, kHeight, 0));
  assert(!TileAddressOffset({0, 0}, kPitch, kHeight, 1));

  // Compare tile-origin addresses across non-square, non-aligned surfaces with
  // the SDK's XGAddress2D-compatible implementation for both 32bpp and 64bpp.
  // This checks the native row-major tile calculation independently, rather
  // than just round-tripping two helpers that share the same formula.
  constexpr uint32_t kGridPitch = 1301;
  constexpr uint32_t kGridHeight = 781;
  constexpr uint32_t kGridRoot = 0x10000000;
  for (uint32_t bytes_per_pixel_log2 : {2u, 3u}) {
    for (uint32_t y = 0; y < kGridHeight; y += 32) {
      for (uint32_t x = 0; x < kGridPitch; x += 32) {
        const auto offset = TileAddressOffset({x, y}, kGridPitch, kGridHeight,
                                              bytes_per_pixel_log2);
        assert(offset);
        const auto sdk_offset = rex::graphics::texture_util::GetTiledOffset2D(
            int32_t(x), int32_t(y), kGridPitch, bytes_per_pixel_log2);
        assert(sdk_offset >= 0 && *offset == uint32_t(sdk_offset));
        const auto origin = FindTileOrigin(*offset, kGridPitch, kGridHeight,
                                           bytes_per_pixel_log2);
        assert(origin && origin->x == x && origin->y == y);
        const auto segment = FindSegmentOrigin(kGridRoot, kGridRoot + *offset, kGridPitch,
                                              kGridHeight, bytes_per_pixel_log2);
        assert(segment && segment->x == x && segment->y == y);
      }
    }
  }
  assert(!FindTileOrigin(1, kPitch, kHeight, 2));
  assert(!FindTileOrigin(1u << 30, kPitch, kHeight, 2));
  assert(!TileAddressOffset({32, 0}, kPitch, kHeight, 1));
  assert(!TileAddressOffset({1, 0}, kPitch, kHeight, 2));
  assert(!TileAddressOffset({0, 1}, kPitch, kHeight, 2));
  assert(!TileAddressOffset({kPitch, 0}, kPitch, kHeight, 2));
  assert(!TileAddressOffset({0, kHeight}, kPitch, kHeight, 2));

  const auto left_region = MakeCopyRegion({0, 0}, 0, 0, 768, 720, 768, 720,
                                          kPitch, kHeight);
  const auto right_region = MakeCopyRegion(*right_a, 0, 0, 640, 720, 640, 720,
                                           kPitch, kHeight);
  assert(left_region && right_region);
  assert(right_region->destination_x == 640);
  const auto shifted_region = MakeCopyRegion(*right_a, 100, 0, 300, 720, 640, 720,
                                             kPitch, kHeight);
  assert(shifted_region && shifted_region->source_x == 100 &&
         shifted_region->destination_x == 740);
  const auto shifted_xy_region = MakeCopyRegion({32, 64}, 20, 30, 128, 200, 400, 400,
                                                kPitch, kHeight);
  assert(shifted_xy_region && shifted_xy_region->source_x == 20 &&
         shifted_xy_region->source_y == 30 && shifted_xy_region->destination_x == 52 &&
         shifted_xy_region->destination_y == 94);
  assert(!MakeCopyRegion({0, 0}, 390, 0, 20, 10, 400, 400, kPitch, kHeight));
  assert(!MakeCopyRegion({0, 0}, 0, 390, 10, 20, 400, 400, kPitch, kHeight));
  assert(!MakeCopyRegion({0, 0}, 0, 0, 0, 10, 400, 400, kPitch, kHeight));
  assert(!MakeCopyRegion({0, 0}, 0, 0, 10, 0, 400, 400, kPitch, kHeight));
  assert(!MakeCopyRegion({1200, 0}, 0, 0, 100, 1, 400, 400, kPitch, kHeight));
  assert(!MakeCopyRegion({0, 700}, 0, 0, 1, 30, 400, 400, kPitch, kHeight));

  // Apply the two resolves in submission order: the right segment overwrites
  // the 128-pixel overlap and fills the remaining right edge.
  std::vector<uint8_t> output(kPitch * kHeight, 0);
  std::vector<uint8_t> left(768 * kHeight, 1);
  std::vector<uint8_t> right(640 * kHeight, 2);
  auto copy = [&](const std::vector<uint8_t>& source, uint32_t source_width,
                  const fear_nx::native::resolve_layout::CopyRegion& region) {
    for (uint32_t y = 0; y < region.height; ++y) {
      for (uint32_t x = 0; x < region.width; ++x) {
        output[(region.destination_y + y) * kPitch + region.destination_x + x] =
            source[(region.source_y + y) * source_width + region.source_x + x];
      }
    }
  };
  copy(left, 768, *left_region);
  copy(right, 640, *right_region);
  assert(output[0] == 1);
  assert(output[639] == 1);
  assert(output[640] == 2);
  assert(output[767] == 2);
  assert(output[768] == 2);
  assert(output[1279] == 2);
  assert(output[(kHeight - 1) * kPitch + 1279] == 2);

  // Unsupported or unrelated ranges must not be composed.
  assert(!MakeCopyRegion(*right_a, 0, 0, 641, 720, 640, 720, kPitch, kHeight));
  assert(!MakeCopyRegion(*right_a, 100, 0, 600, 720, 700, 720, kPitch, kHeight));
  assert(!MakeCopyRegion({960, 0}, 0, 0, 640, 720, 640, 720, kPitch, kHeight));

  // Root updates invalidate composite selection only for the current frame;
  // a compatible tiled root write can reactivate it after the copy completes.
  CompositeState state;
  state.Activate();
  assert(state.Usable());
  state.BeginRootWrite(true);
  assert(state.exists && !state.active && !state.Usable());
  state.Activate();
  assert(state.Usable());

  // No root write means the previous composite stays selected across frames.
  CompositeState unchanged;
  unchanged.Activate();
  assert(unchanged.Usable());
  assert(unchanged.Usable());

  // An unsupported root write must discard stale composite pixels.
  CompositeState unsupported;
  unsupported.Activate();
  unsupported.BeginRootWrite(false);
  assert(!unsupported.exists && !unsupported.active && !unsupported.Usable());

  // A segment with an unrecognized window-offset mapping must retire the
  // root composite instead of leaving stale pixels selected for sampling.
  CompositeState incompatible_segment;
  incompatible_segment.Activate();
  incompatible_segment.BeginRootWrite(false);
  assert(!incompatible_segment.Usable());

  // A segment without a current-frame root record is rejected the same way;
  // the previously active complete image must not hide the raw write.
  CompositeState missing_current_root;
  missing_current_root.Activate();
  missing_current_root.BeginRootWrite(false);
  assert(!missing_current_root.Usable());
}
