#include "native_color_clear.h"

#include <cassert>

using fear_nx::native::MapZeroColorAliasClearRectangle;
using fear_nx::native::StencilClearLayout;
using fear_nx::native::StencilClearRect;

namespace {

constexpr StencilClearLayout kMsaa4{1024, 400, 2, 0, 400, 816};
constexpr StencilClearLayout kMsaa2{1024, 800, 1, 0, 800, 816};

void ExpectRect(const StencilClearRect& actual, int32_t x0, int32_t y0, int32_t x1,
                int32_t y1) {
  assert(actual.x0 == x0 && actual.y0 == y0 && actual.x1 == x1 && actual.y1 == y1);
}

void ExpectRejected(const StencilClearLayout& source, const StencilClearLayout& destination,
                    const StencilClearRect& source_rect) {
  StencilClearRect destination_rect{1, 2, 3, 4};
  assert(!MapZeroColorAliasClearRectangle(source, destination, source_rect, destination_rect));
  ExpectRect(destination_rect, 0, 0, 0, 0);
}

}  // namespace

int main() {
  StencilClearRect mapped{};
  assert(MapZeroColorAliasClearRectangle(kMsaa4, kMsaa2, {0, 0, 360, 720}, mapped));
  ExpectRect(mapped, 0, 0, 720, 720);

  assert(MapZeroColorAliasClearRectangle(kMsaa2, kMsaa4, {720, 0, 800, 720}, mapped));
  ExpectRect(mapped, 360, 0, 400, 720);

  ExpectRejected(kMsaa4, kMsaa2, {0, 0, 359, 720});
  ExpectRejected(kMsaa2, kMsaa4, {720, 0, 768, 720});
  ExpectRejected(kMsaa4, kMsaa2, {0, 0, 360, 719});

  auto different_base = kMsaa2;
  ++different_base.base_tiles;
  ExpectRejected(kMsaa4, different_base, {0, 0, 360, 720});

  auto different_format = kMsaa2;
  different_format.format = 1;
  ExpectRejected(kMsaa4, different_format, {0, 0, 360, 720});

  auto same_layout = kMsaa4;
  ExpectRejected(kMsaa4, same_layout, {0, 0, 360, 720});

  return 0;
}
