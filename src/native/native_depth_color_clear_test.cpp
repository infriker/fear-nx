#include "native_depth_color_clear.h"

#include <cassert>

using fear_nx::native::MapZeroDepthColorClearRectangle;
using fear_nx::native::StencilClearLayout;
using fear_nx::native::StencilClearRect;

namespace {

constexpr StencilClearLayout kDepth320x2048Msaa4{
    0, 320, 2, fear_nx::native::kDepthClearFormatD24S8, 320, 2048};
constexpr StencilClearLayout kColor320x2048Msaa4{
    0, 320, 2, fear_nx::native::kColorClearFormatR8G8B8A8, 320, 2048};

void ExpectRect(const StencilClearRect& actual, int32_t x0, int32_t y0, int32_t x1,
                int32_t y1) {
  assert(actual.x0 == x0 && actual.y0 == y0 && actual.x1 == x1 && actual.y1 == y1);
}

void ExpectRejected(const StencilClearLayout& source, const StencilClearLayout& destination,
                    const StencilClearRect& source_rect) {
  StencilClearRect destination_rect{1, 2, 3, 4};
  assert(!MapZeroDepthColorClearRectangle(source, destination, source_rect, destination_rect));
  ExpectRect(destination_rect, 0, 0, 0, 0);
}

}  // namespace

int main() {
  StencilClearRect mapped{};

  // Captured dimensions: 320x240 at guest 4x MSAA. A 4x Xenos EDRAM tile is
  // 40x8 logical pixels, so all four edges land on whole-tile boundaries.
  assert(MapZeroDepthColorClearRectangle(kDepth320x2048Msaa4, kColor320x2048Msaa4,
                                         {0, 0, 320, 240}, mapped));
  ExpectRect(mapped, 0, 0, 320, 240);

  // The gamma RGBA8 alias uses the same 32-bit storage layout.
  auto gamma_color = kColor320x2048Msaa4;
  gamma_color.format = fear_nx::native::kColorClearFormatR8G8B8A8Gamma;
  assert(MapZeroDepthColorClearRectangle(kDepth320x2048Msaa4, gamma_color,
                                         {40, 8, 280, 240}, mapped));
  ExpectRect(mapped, 40, 8, 280, 240);

  // Clipping can still be safe when the resulting source and color bounds end
  // on whole tiles.
  assert(MapZeroDepthColorClearRectangle(kDepth320x2048Msaa4, kColor320x2048Msaa4,
                                         {-20, -4, 500, 240}, mapped));
  ExpectRect(mapped, 0, 0, 320, 240);

  auto shorter_color = kColor320x2048Msaa4;
  shorter_color.height = 240;
  assert(MapZeroDepthColorClearRectangle(kDepth320x2048Msaa4, shorter_color,
                                         {0, 0, 320, 400}, mapped));
  ExpectRect(mapped, 0, 0, 320, 240);

  // A destination extent that clips at a non-tile edge must be rejected.
  auto partial_color_extent = kColor320x2048Msaa4;
  partial_color_extent.width = 300;
  ExpectRejected(kDepth320x2048Msaa4, partial_color_extent, {0, 0, 320, 240});
  partial_color_extent = kColor320x2048Msaa4;
  partial_color_extent.height = 236;
  ExpectRejected(kDepth320x2048Msaa4, partial_color_extent, {0, 0, 320, 240});

  // Partial tiles, empty/outside rectangles, layout mismatches, and unverified
  // formats/MSAA are all conservative failures.
  ExpectRejected(kDepth320x2048Msaa4, kColor320x2048Msaa4, {0, 0, 318, 240});
  ExpectRejected(kDepth320x2048Msaa4, kColor320x2048Msaa4, {0, 0, 320, 239});
  ExpectRejected(kDepth320x2048Msaa4, kColor320x2048Msaa4, {0, 0, 0, 240});
  ExpectRejected(kDepth320x2048Msaa4, kColor320x2048Msaa4, {-20, 0, -1, 240});

  auto different_base = kColor320x2048Msaa4;
  different_base.base_tiles++;
  ExpectRejected(kDepth320x2048Msaa4, different_base, {0, 0, 320, 240});

  auto different_pitch = kColor320x2048Msaa4;
  different_pitch.pitch_pixels = 400;
  ExpectRejected(kDepth320x2048Msaa4, different_pitch, {0, 0, 320, 240});

  auto different_msaa = kColor320x2048Msaa4;
  different_msaa.guest_msaa = 1;
  ExpectRejected(kDepth320x2048Msaa4, different_msaa, {0, 0, 320, 240});
  different_msaa = kColor320x2048Msaa4;
  different_msaa.guest_msaa = 3;
  ExpectRejected(kDepth320x2048Msaa4, different_msaa, {0, 0, 320, 240});

  auto unsupported_depth = kDepth320x2048Msaa4;
  unsupported_depth.format = 1;
  ExpectRejected(unsupported_depth, kColor320x2048Msaa4, {0, 0, 320, 240});

  auto unsupported_color = kColor320x2048Msaa4;
  unsupported_color.format = 2;
  ExpectRejected(kDepth320x2048Msaa4, unsupported_color, {0, 0, 320, 240});

  auto partial_source_extent = kDepth320x2048Msaa4;
  partial_source_extent.width = 300;
  ExpectRejected(partial_source_extent, kColor320x2048Msaa4, {0, 0, 320, 240});
  partial_source_extent = kDepth320x2048Msaa4;
  partial_source_extent.height = 236;
  ExpectRejected(partial_source_extent, kColor320x2048Msaa4, {0, 0, 320, 240});

  return 0;
}
