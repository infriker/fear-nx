#include "native_depth_color_clear.h"

#include <cassert>

using fear_nx::native::MapZeroDepthColorClearRectangle;
using fear_nx::native::MapConstantDepthColorClearRectangle;
using fear_nx::native::StencilClearLayout;
using fear_nx::native::StencilClearRect;

namespace {

constexpr StencilClearLayout kDepth320x2048Msaa4{
    0, 320, 2, fear_nx::native::kDepthClearFormatD24S8, 320, 2048};
constexpr StencilClearLayout kColor320x2048Msaa4{
    0, 320, 2, fear_nx::native::kColorClearFormatR8G8B8A8, 320, 2048};
constexpr StencilClearLayout kDepth160x2048Msaa4{
    0, 160, 2, fear_nx::native::kDepthClearFormatD24S8, 160, 2048};
constexpr StencilClearLayout kColor320x256Msaa1{
    0, 320, 0, fear_nx::native::kColorClearFormatR8G8B8A8, 320, 256};

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

void ExpectConstantRejected(const StencilClearLayout& source,
                            const StencilClearLayout& destination,
                            const StencilClearRect& source_rect) {
  StencilClearRect destination_rect{1, 2, 3, 4};
  assert(!MapConstantDepthColorClearRectangle(source, destination, source_rect,
                                              destination_rect));
  ExpectRect(destination_rect, 0, 0, 0, 0);
}

}  // namespace

int main() {
  StencilClearRect mapped{};

  // Captured D24 0xFF0000 becomes the RGBA8 word 0xFF000000. A matching
  // un-divided Z with W!=1 must not be mistaken for the same clear depth.
  constexpr float kOpaqueBlackDepth = float(0x00FF0001) / float(1 << 24);
  using fear_nx::native::IsConstantDepthClearVertex;
  assert(IsConstantDepthClearVertex(kOpaqueBlackDepth, 1.0f, false, kOpaqueBlackDepth));
  assert(!IsConstantDepthClearVertex(kOpaqueBlackDepth, 2.0f, false, kOpaqueBlackDepth));
  assert(IsConstantDepthClearVertex(kOpaqueBlackDepth, 2.0f, true, kOpaqueBlackDepth));
  assert(!IsConstantDepthClearVertex(1.0f, 1.0f, true, kOpaqueBlackDepth));

  // Captured weapon clear: 4x depth at 160x2048 and 1x RGBA8 at 320x256
  // share a four-tile EDRAM pitch. A 120x128 source clear maps to 240x256,
  // which is three whole tile columns by sixteen tile rows in the color view.
  assert(MapConstantDepthColorClearRectangle(kDepth160x2048Msaa4, kColor320x256Msaa1,
                                             {0, 0, 120, 128}, mapped));
  ExpectRect(mapped, 0, 0, 240, 256);

  auto gamma_1x_color = kColor320x256Msaa1;
  gamma_1x_color.format = fear_nx::native::kColorClearFormatR8G8B8A8Gamma;
  assert(MapConstantDepthColorClearRectangle(kDepth160x2048Msaa4, gamma_1x_color,
                                             {0, 0, 120, 128}, mapped));
  ExpectRect(mapped, 0, 0, 240, 256);

  ExpectConstantRejected(kDepth160x2048Msaa4, kColor320x256Msaa1, {0, 0, 119, 128});
  auto wrong_physical_pitch = kColor320x256Msaa1;
  wrong_physical_pitch.pitch_pixels = 400;
  ExpectConstantRejected(kDepth160x2048Msaa4, wrong_physical_pitch, {0, 0, 120, 128});
  auto clipped_to_partial_tile = kColor320x256Msaa1;
  clipped_to_partial_tile.width = 230;
  ExpectConstantRejected(kDepth160x2048Msaa4, clipped_to_partial_tile, {0, 0, 120, 128});
  auto wrong_base = kColor320x256Msaa1;
  wrong_base.base_tiles++;
  ExpectConstantRejected(kDepth160x2048Msaa4, wrong_base, {0, 0, 120, 128});

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
