#pragma once

#include <cstdint>

namespace fear_nx::native {

// Guest MSAA register encodings used by the native renderer:
//   0 = 1x, 1 = 2x, 2 = 4x.
// The Xenos EDRAM samples are laid out 1x/1x, 1x/2y, and 2x/2y.
struct StencilClearLayout {
  uint32_t base_tiles;
  uint32_t pitch_pixels;
  uint32_t guest_msaa;
  uint32_t format;
  uint32_t width;
  uint32_t height;
};

struct StencilClearRect {
  int32_t x0;
  int32_t y0;
  int32_t x1;
  int32_t y1;
};

namespace stencil_clear_detail {

constexpr uint32_t SampleScaleX(uint32_t guest_msaa) {
  return guest_msaa == 2 ? 2u : 1u;
}

constexpr uint32_t SampleScaleY(uint32_t guest_msaa) {
  return guest_msaa == 0 ? 1u : 2u;
}

constexpr uint64_t EdramPitchTiles(const StencilClearLayout& layout) {
  return (uint64_t(layout.pitch_pixels) * SampleScaleX(layout.guest_msaa) + 79u) / 80u;
}

constexpr int64_t Clamp(int64_t value, int64_t lower, int64_t upper) {
  return value < lower ? lower : (value > upper ? upper : value);
}

constexpr bool MapEdge(int64_t source_edge, uint32_t source_samples,
                       uint32_t destination_samples, int32_t& destination_edge) {
  const int64_t scaled = source_edge * int64_t(source_samples);
  if (scaled % int64_t(destination_samples) != 0) return false;
  const int64_t mapped = scaled / int64_t(destination_samples);
  if (mapped < INT32_MIN || mapped > INT32_MAX) return false;
  destination_edge = int32_t(mapped);
  return true;
}

}  // namespace stencil_clear_detail

// Maps a clear rectangle between two native images that alias the same Xenos
// EDRAM range. Coordinates are guest logical pixels. Return false if the
// images cannot safely alias, the sample-edge mapping is fractional, or the
// mapped/clipped rectangle is empty. `destination_rect` is zeroed on failure.
constexpr bool MapStencilClearRectangle(const StencilClearLayout& source,
                                       const StencilClearLayout& destination,
                                       const StencilClearRect& source_rect,
                                       StencilClearRect& destination_rect) {
  destination_rect = {};

  if (source.base_tiles != destination.base_tiles || source.format != destination.format ||
      source.pitch_pixels == 0 || destination.pitch_pixels == 0 || source.width == 0 ||
      source.height == 0 || destination.width == 0 || destination.height == 0 ||
      source.guest_msaa > 2 || destination.guest_msaa > 2 ||
      stencil_clear_detail::EdramPitchTiles(source) !=
          stencil_clear_detail::EdramPitchTiles(destination)) {
    return false;
  }

  if (source_rect.x1 <= source_rect.x0 || source_rect.y1 <= source_rect.y0) return false;

  // Vulkan already clips the original clear to the source image. Apply the
  // same clipping before mapping, using 64-bit intermediates for signed input.
  const int64_t sx0 = stencil_clear_detail::Clamp(source_rect.x0, 0, source.width);
  const int64_t sy0 = stencil_clear_detail::Clamp(source_rect.y0, 0, source.height);
  const int64_t sx1 = stencil_clear_detail::Clamp(source_rect.x1, 0, source.width);
  const int64_t sy1 = stencil_clear_detail::Clamp(source_rect.y1, 0, source.height);
  if (sx1 <= sx0 || sy1 <= sy0) return false;

  StencilClearRect mapped{};
  if (!stencil_clear_detail::MapEdge(sx0, stencil_clear_detail::SampleScaleX(source.guest_msaa),
                                     stencil_clear_detail::SampleScaleX(destination.guest_msaa),
                                     mapped.x0) ||
      !stencil_clear_detail::MapEdge(sx1, stencil_clear_detail::SampleScaleX(source.guest_msaa),
                                     stencil_clear_detail::SampleScaleX(destination.guest_msaa),
                                     mapped.x1) ||
      !stencil_clear_detail::MapEdge(sy0, stencil_clear_detail::SampleScaleY(source.guest_msaa),
                                     stencil_clear_detail::SampleScaleY(destination.guest_msaa),
                                     mapped.y0) ||
      !stencil_clear_detail::MapEdge(sy1, stencil_clear_detail::SampleScaleY(source.guest_msaa),
                                     stencil_clear_detail::SampleScaleY(destination.guest_msaa),
                                     mapped.y1)) {
    return false;
  }

  mapped.x0 = int32_t(stencil_clear_detail::Clamp(mapped.x0, 0, destination.width));
  mapped.y0 = int32_t(stencil_clear_detail::Clamp(mapped.y0, 0, destination.height));
  mapped.x1 = int32_t(stencil_clear_detail::Clamp(mapped.x1, 0, destination.width));
  mapped.y1 = int32_t(stencil_clear_detail::Clamp(mapped.y1, 0, destination.height));
  if (mapped.x1 <= mapped.x0 || mapped.y1 <= mapped.y0) return false;

  destination_rect = mapped;
  return true;
}

}  // namespace fear_nx::native
