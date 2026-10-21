#include "native_stencil_clear.h"

#include <cassert>

using fear_nx::native::MapStencilClearRectangle;
using fear_nx::native::StencilClearLayout;
using fear_nx::native::StencilClearRect;

namespace {

constexpr StencilClearLayout kMsaa4Left{1024, 400, 2, 0, 400, 816};
constexpr StencilClearLayout kMsaa2Full{1024, 800, 1, 0, 800, 816};

void ExpectRect(const StencilClearRect& actual, int32_t x0, int32_t y0, int32_t x1,
                int32_t y1) {
  assert(actual.x0 == x0 && actual.y0 == y0 && actual.x1 == x1 && actual.y1 == y1);
}

void ExpectRejected(const StencilClearLayout& source, const StencilClearLayout& destination,
                    const StencilClearRect& source_rect) {
  StencilClearRect destination_rect{1, 2, 3, 4};
  assert(!MapStencilClearRectangle(source, destination, source_rect, destination_rect));
  ExpectRect(destination_rect, 0, 0, 0, 0);
}

}  // namespace

int main() {
  StencilClearRect mapped{};

  // Reproduces clear 1107: 400-wide 4x surface to 800-wide 2x surface.
  assert(MapStencilClearRectangle(kMsaa4Left, kMsaa2Full, {0, 0, 360, 720}, mapped));
  ExpectRect(mapped, 0, 0, 720, 720);

  // Reproduces clear 1101's right-side strip in the reverse direction.
  assert(MapStencilClearRectangle(kMsaa2Full, kMsaa4Left, {720, 0, 768, 720}, mapped));
  ExpectRect(mapped, 360, 0, 384, 720);

  // Any rectangle aligned to the 2x destination's sample edges round-trips.
  constexpr StencilClearRect kAligned{10, 8, 350, 712};
  assert(MapStencilClearRectangle(kMsaa4Left, kMsaa2Full, kAligned, mapped));
  StencilClearRect round_trip{};
  assert(MapStencilClearRectangle(kMsaa2Full, kMsaa4Left, mapped, round_trip));
  ExpectRect(round_trip, kAligned.x0, kAligned.y0, kAligned.x1, kAligned.y1);

  // An odd edge cannot be represented on the 4x destination sample grid.
  ExpectRejected(kMsaa2Full, kMsaa4Left, {0, 0, 359, 720});

  // The 1x to 2x conversion changes only Y: two 2x guest rows occupy one 1x
  // EDRAM row. Reverse mapping returns to the same rectangle.
  constexpr StencilClearLayout kMsaa1{1024, 800, 0, 0, 800, 816};
  constexpr StencilClearLayout kMsaa2HalfHeight{1024, 800, 1, 0, 800, 408};
  assert(MapStencilClearRectangle(kMsaa1, kMsaa2HalfHeight,
                                  {8, 20, 120, 80}, mapped));
  ExpectRect(mapped, 8, 10, 120, 40);
  assert(MapStencilClearRectangle(kMsaa2HalfHeight, kMsaa1,
                                  {8, 10, 120, 40}, round_trip));
  ExpectRect(round_trip, 8, 20, 120, 80);

  // A partial Y sample edge cannot be represented in the 2x destination.
  ExpectRejected(kMsaa1, kMsaa2HalfHeight, {8, 20, 120, 81});

  // Clip to source bounds first, then clamp to the destination extent.
  assert(MapStencilClearRectangle(kMsaa4Left, kMsaa2Full,
                                  {-50, -8, 500, 900}, mapped));
  ExpectRect(mapped, 0, 0, 800, 816);
  constexpr StencilClearLayout kNarrowDestination{1024, 800, 1, 0, 400, 816};
  assert(MapStencilClearRectangle(kMsaa4Left, kNarrowDestination,
                                  {0, 0, 400, 720}, mapped));
  ExpectRect(mapped, 0, 0, 400, 720);

  ExpectRejected(kMsaa4Left, kMsaa2Full, {0, 0, 0, 720});
  ExpectRejected(kMsaa4Left, kMsaa2Full, {-20, 0, -1, 720});

  auto different_base = kMsaa2Full;
  different_base.base_tiles++;
  ExpectRejected(kMsaa4Left, different_base, {0, 0, 360, 720});

  auto different_pitch = kMsaa2Full;
  different_pitch.pitch_pixels = 801;
  ExpectRejected(kMsaa4Left, different_pitch, {0, 0, 360, 720});

  auto different_format = kMsaa2Full;
  different_format.format++;
  ExpectRejected(kMsaa4Left, different_format, {0, 0, 360, 720});

  auto invalid_source_msaa = kMsaa4Left;
  invalid_source_msaa.guest_msaa = 3;
  ExpectRejected(invalid_source_msaa, kMsaa2Full, {0, 0, 360, 720});

  auto invalid_destination_msaa = kMsaa2Full;
  invalid_destination_msaa.guest_msaa = 3;
  ExpectRejected(kMsaa4Left, invalid_destination_msaa, {0, 0, 360, 720});

  return 0;
}
