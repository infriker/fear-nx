#pragma once

#include "native_stencil_clear.h"

namespace fear_nx::native {

// Format enum values are interpreted according to the render-target kind.
// These are the only cross-kind formats for which the current depth-zero
// clear mirror is known to preserve the guest's packed 32-bit zero value.
constexpr uint32_t kDepthClearFormatD24S8 = 0;
constexpr uint32_t kColorClearFormatR8G8B8A8 = 0;
constexpr uint32_t kColorClearFormatR8G8B8A8Gamma = 1;

// With an identity viewport Z transform, a predivided Z is already the
// rasterized depth. Otherwise require W=1 before comparing a guest D24 clear.
constexpr bool IsConstantDepthClearVertex(float z, float w, bool z_predivided,
                                          float expected_depth) {
  return z == expected_depth && (z_predivided || w == 1.0f);
}

namespace depth_color_clear_detail {

constexpr uint32_t kEdramTileWidthSamples = 80;
constexpr uint32_t kEdramTileHeightSamples = 16;

constexpr bool IsTileEdge(int32_t edge, uint32_t tile_size_pixels) {
  return edge >= 0 && tile_size_pixels != 0 &&
         uint32_t(edge) % tile_size_pixels == 0;
}

constexpr bool IsFullTileRect(const StencilClearRect& rect, uint32_t tile_width_pixels,
                              uint32_t tile_height_pixels) {
  return rect.x1 > rect.x0 && rect.y1 > rect.y0 &&
         IsTileEdge(rect.x0, tile_width_pixels) && IsTileEdge(rect.x1, tile_width_pixels) &&
         IsTileEdge(rect.y0, tile_height_pixels) && IsTileEdge(rect.y1, tile_height_pixels);
}

}  // namespace depth_color_clear_detail

// Maps a depth-zero clear to the corresponding color alias only when it covers
// whole physical Xenos EDRAM tiles. Depth and color targets have separate host
// images, so callers must additionally prove that both layouts refer to the
// same guest EDRAM owner and that the depth and stencil clear values are zero.
// Coordinates are guest logical pixels; for identical guest MSAA layouts they
// remain unchanged. `destination_rect` is zeroed on failure.
constexpr bool MapZeroDepthColorClearRectangle(const StencilClearLayout& source_depth,
                                              const StencilClearLayout& destination_color,
                                              const StencilClearRect& source_rect,
                                              StencilClearRect& destination_rect) {
  destination_rect = {};

  if (source_depth.format != kDepthClearFormatD24S8 ||
      (destination_color.format != kColorClearFormatR8G8B8A8 &&
       destination_color.format != kColorClearFormatR8G8B8A8Gamma) ||
      source_depth.base_tiles != destination_color.base_tiles ||
      source_depth.pitch_pixels == 0 || source_depth.pitch_pixels != destination_color.pitch_pixels ||
      source_depth.guest_msaa != destination_color.guest_msaa || source_depth.guest_msaa > 2 ||
      source_depth.width == 0 || source_depth.height == 0 || destination_color.width == 0 ||
      destination_color.height == 0 || source_rect.x1 <= source_rect.x0 ||
      source_rect.y1 <= source_rect.y0) {
    return false;
  }

  // The enum encoding determines the Xenos sample grid: 1x = 1x1, 2x = 1x2,
  // and 4x = 2x2. One EDRAM tile is always 80x16 samples.
  const uint32_t tile_width_pixels =
      depth_color_clear_detail::kEdramTileWidthSamples /
      stencil_clear_detail::SampleScaleX(source_depth.guest_msaa);
  const uint32_t tile_height_pixels =
      depth_color_clear_detail::kEdramTileHeightSamples /
      stencil_clear_detail::SampleScaleY(source_depth.guest_msaa);

  // The source draw is clipped to its image before Vulkan applies the clear.
  // Reject if that clipping leaves a partial EDRAM tile: mirroring the clear
  // would otherwise erase bytes that the guest draw did not touch.
  const int64_t sx0 = stencil_clear_detail::Clamp(source_rect.x0, 0, source_depth.width);
  const int64_t sy0 = stencil_clear_detail::Clamp(source_rect.y0, 0, source_depth.height);
  const int64_t sx1 = stencil_clear_detail::Clamp(source_rect.x1, 0, source_depth.width);
  const int64_t sy1 = stencil_clear_detail::Clamp(source_rect.y1, 0, source_depth.height);
  if (sx1 <= sx0 || sy1 <= sy0 || sx0 > INT32_MAX || sy0 > INT32_MAX || sx1 > INT32_MAX ||
      sy1 > INT32_MAX) {
    return false;
  }

  StencilClearRect source_clipped{int32_t(sx0), int32_t(sy0), int32_t(sx1), int32_t(sy1)};
  if (!depth_color_clear_detail::IsFullTileRect(source_clipped, tile_width_pixels,
                                                tile_height_pixels)) {
    return false;
  }

  // Clip to the color image's extent as well. This is safe only when the
  // resulting rectangle still consists exclusively of complete EDRAM tiles.
  const int64_t dx0 = stencil_clear_detail::Clamp(source_clipped.x0, 0, destination_color.width);
  const int64_t dy0 = stencil_clear_detail::Clamp(source_clipped.y0, 0, destination_color.height);
  const int64_t dx1 = stencil_clear_detail::Clamp(source_clipped.x1, 0, destination_color.width);
  const int64_t dy1 = stencil_clear_detail::Clamp(source_clipped.y1, 0, destination_color.height);
  if (dx1 <= dx0 || dy1 <= dy0 || dx0 > INT32_MAX || dy0 > INT32_MAX || dx1 > INT32_MAX ||
      dy1 > INT32_MAX) {
    return false;
  }

  StencilClearRect mapped{int32_t(dx0), int32_t(dy0), int32_t(dx1), int32_t(dy1)};
  if (!depth_color_clear_detail::IsFullTileRect(mapped, tile_width_pixels, tile_height_pixels)) {
    return false;
  }

  destination_rect = mapped;
  return true;
}

// Maps a constant-value D24S8 clear to a packed RGBA8 color alias. The value
// itself is checked by the caller; this function only proves that the source
// and destination images cover the same whole EDRAM tiles. Unlike the legacy
// zero-depth path above, the guest sample grids may differ (for example a 4x
// depth image and a 1x color image), provided their physical tile pitch agrees.
constexpr bool MapConstantDepthColorClearRectangle(
    const StencilClearLayout& source_depth, const StencilClearLayout& destination_color,
    const StencilClearRect& source_rect, StencilClearRect& destination_rect) {
  destination_rect = {};

  if (source_depth.format != kDepthClearFormatD24S8 ||
      (destination_color.format != kColorClearFormatR8G8B8A8 &&
       destination_color.format != kColorClearFormatR8G8B8A8Gamma) ||
      source_depth.base_tiles != destination_color.base_tiles ||
      source_depth.pitch_pixels == 0 || destination_color.pitch_pixels == 0 ||
      source_depth.guest_msaa > 2 || destination_color.guest_msaa > 2 ||
      stencil_clear_detail::EdramPitchTiles(source_depth) !=
          stencil_clear_detail::EdramPitchTiles(destination_color) ||
      source_depth.width == 0 || source_depth.height == 0 || destination_color.width == 0 ||
      destination_color.height == 0 || source_rect.x1 <= source_rect.x0 ||
      source_rect.y1 <= source_rect.y0) {
    return false;
  }

  // Vulkan clips the original clear to the source image. Reject any clipped
  // source rectangle that no longer covers whole physical EDRAM tiles.
  const int64_t sx0 = stencil_clear_detail::Clamp(source_rect.x0, 0, source_depth.width);
  const int64_t sy0 = stencil_clear_detail::Clamp(source_rect.y0, 0, source_depth.height);
  const int64_t sx1 = stencil_clear_detail::Clamp(source_rect.x1, 0, source_depth.width);
  const int64_t sy1 = stencil_clear_detail::Clamp(source_rect.y1, 0, source_depth.height);
  if (sx1 <= sx0 || sy1 <= sy0 || sx1 > INT32_MAX || sy1 > INT32_MAX) return false;

  const StencilClearRect clipped_source{int32_t(sx0), int32_t(sy0), int32_t(sx1),
                                        int32_t(sy1)};
  const uint32_t source_tile_width =
      depth_color_clear_detail::kEdramTileWidthSamples /
      stencil_clear_detail::SampleScaleX(source_depth.guest_msaa);
  const uint32_t source_tile_height =
      depth_color_clear_detail::kEdramTileHeightSamples /
      stencil_clear_detail::SampleScaleY(source_depth.guest_msaa);
  if (!depth_color_clear_detail::IsFullTileRect(clipped_source, source_tile_width,
                                                source_tile_height)) {
    return false;
  }

  StencilClearRect mapped{};
  if (!stencil_clear_detail::MapEdge(
          sx0, stencil_clear_detail::SampleScaleX(source_depth.guest_msaa),
          stencil_clear_detail::SampleScaleX(destination_color.guest_msaa), mapped.x0) ||
      !stencil_clear_detail::MapEdge(
          sx1, stencil_clear_detail::SampleScaleX(source_depth.guest_msaa),
          stencil_clear_detail::SampleScaleX(destination_color.guest_msaa), mapped.x1) ||
      !stencil_clear_detail::MapEdge(
          sy0, stencil_clear_detail::SampleScaleY(source_depth.guest_msaa),
          stencil_clear_detail::SampleScaleY(destination_color.guest_msaa), mapped.y0) ||
      !stencil_clear_detail::MapEdge(
          sy1, stencil_clear_detail::SampleScaleY(source_depth.guest_msaa),
          stencil_clear_detail::SampleScaleY(destination_color.guest_msaa), mapped.y1)) {
    return false;
  }

  mapped.x0 = int32_t(stencil_clear_detail::Clamp(mapped.x0, 0, destination_color.width));
  mapped.y0 = int32_t(stencil_clear_detail::Clamp(mapped.y0, 0, destination_color.height));
  mapped.x1 = int32_t(stencil_clear_detail::Clamp(mapped.x1, 0, destination_color.width));
  mapped.y1 = int32_t(stencil_clear_detail::Clamp(mapped.y1, 0, destination_color.height));
  const uint32_t destination_tile_width =
      depth_color_clear_detail::kEdramTileWidthSamples /
      stencil_clear_detail::SampleScaleX(destination_color.guest_msaa);
  const uint32_t destination_tile_height =
      depth_color_clear_detail::kEdramTileHeightSamples /
      stencil_clear_detail::SampleScaleY(destination_color.guest_msaa);
  if (!depth_color_clear_detail::IsFullTileRect(mapped, destination_tile_width,
                                                destination_tile_height)) {
    return false;
  }

  destination_rect = mapped;
  return true;
}

}  // namespace fear_nx::native
