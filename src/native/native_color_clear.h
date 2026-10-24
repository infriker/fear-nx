#pragma once

#include "native_stencil_clear.h"

namespace fear_nx::native {

// The renderer normalizes the linear and gamma variants to this shared
// storage format. This mirror is deliberately limited to packed RGBA8, where
// an all-zero shader result has the same exact EDRAM representation.
constexpr uint32_t kColorAliasClearFormatRgba8 = 0;

namespace color_alias_clear_detail {

constexpr uint32_t kEdramTileWidthSamples = 80;
constexpr uint32_t kEdramTileHeightSamples = 16;

constexpr bool IsTileEdge(int32_t edge, uint32_t tile_size_pixels) {
  return edge >= 0 && tile_size_pixels != 0 && uint32_t(edge) % tile_size_pixels == 0;
}

constexpr bool IsFullTileRect(const StencilClearRect& rect, uint32_t tile_width_pixels,
                              uint32_t tile_height_pixels) {
  return rect.x1 > rect.x0 && rect.y1 > rect.y0 &&
         IsTileEdge(rect.x0, tile_width_pixels) && IsTileEdge(rect.x1, tile_width_pixels) &&
         IsTileEdge(rect.y0, tile_height_pixels) && IsTileEdge(rect.y1, tile_height_pixels);
}

constexpr uint32_t TileWidthPixels(uint32_t guest_msaa) {
  return kEdramTileWidthSamples / stencil_clear_detail::SampleScaleX(guest_msaa);
}

constexpr uint32_t TileHeightPixels(uint32_t guest_msaa) {
  return kEdramTileHeightSamples / stencil_clear_detail::SampleScaleY(guest_msaa);
}

}  // namespace color_alias_clear_detail

// Maps a proven all-zero color draw between two host render-target images
// which represent the same EDRAM tiles in different guest MSAA layouts.
// Callers must prove the draw writes all four color components with no blend,
// alpha coverage, discard, or failed depth/stencil test.
constexpr bool MapZeroColorAliasClearRectangle(const StencilClearLayout& source,
                                               const StencilClearLayout& destination,
                                               const StencilClearRect& source_rect,
                                               StencilClearRect& destination_rect) {
  destination_rect = {};
  if (source.format != kColorAliasClearFormatRgba8 ||
      destination.format != kColorAliasClearFormatRgba8 ||
      source.guest_msaa == destination.guest_msaa || source.guest_msaa > 2 ||
      destination.guest_msaa > 2) {
    return false;
  }

  const int64_t sx0 = stencil_clear_detail::Clamp(source_rect.x0, 0, source.width);
  const int64_t sy0 = stencil_clear_detail::Clamp(source_rect.y0, 0, source.height);
  const int64_t sx1 = stencil_clear_detail::Clamp(source_rect.x1, 0, source.width);
  const int64_t sy1 = stencil_clear_detail::Clamp(source_rect.y1, 0, source.height);
  if (sx1 <= sx0 || sy1 <= sy0 || sx1 > INT32_MAX || sy1 > INT32_MAX) return false;

  const StencilClearRect clipped_source{int32_t(sx0), int32_t(sy0), int32_t(sx1), int32_t(sy1)};
  if (!color_alias_clear_detail::IsFullTileRect(
          clipped_source, color_alias_clear_detail::TileWidthPixels(source.guest_msaa),
          color_alias_clear_detail::TileHeightPixels(source.guest_msaa))) {
    return false;
  }

  StencilClearRect mapped{};
  if (!MapStencilClearRectangle(source, destination, clipped_source, mapped) ||
      !color_alias_clear_detail::IsFullTileRect(
          mapped, color_alias_clear_detail::TileWidthPixels(destination.guest_msaa),
          color_alias_clear_detail::TileHeightPixels(destination.guest_msaa))) {
    return false;
  }
  destination_rect = mapped;
  return true;
}

}  // namespace fear_nx::native
