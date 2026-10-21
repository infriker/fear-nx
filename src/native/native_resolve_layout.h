// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>

namespace fear_nx::native::resolve_layout {

struct Point {
  uint32_t x = 0;
  uint32_t y = 0;
};

struct CopyRegion {
  uint32_t source_x = 0;
  uint32_t source_y = 0;
  uint32_t destination_x = 0;
  uint32_t destination_y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

struct CompositeState {
  bool exists = false;
  bool active = false;

  void BeginRootWrite(bool retain) {
    active = false;
    if (!retain) exists = false;
  }
  void Activate() {
    exists = true;
    active = true;
  }
  bool Usable() const { return exists && active; }
};

// Xenos 2D textures are tiled in 32x32 blocks. At tile boundaries, each tile
// occupies 1024 * bytes-per-pixel bytes and tile addresses are row-major.
// The copy-destination base may already include this offset, so a split
// resolve can point at a tile within the texture instead of its first tile.
inline std::optional<Point> FindTileOrigin(uint32_t address_delta, uint32_t pitch,
                                           uint32_t height, uint32_t bytes_per_pixel_log2) {
  constexpr uint32_t kTileEdge = 32;
  // The simple row-major tile mapping below applies to 4-byte and larger
  // texels. Xenos 1/2-byte formats use overlapping address spans and a
  // different repeating tile layout, so do not guess an origin for them.
  if (!pitch || !height || bytes_per_pixel_log2 < 2 || bytes_per_pixel_log2 > 3) {
    return std::nullopt;
  }
  const uint32_t tile_bytes = 1u << (10 + bytes_per_pixel_log2);
  if (address_delta % tile_bytes) return std::nullopt;

  const uint32_t tiles_per_row = (pitch + kTileEdge - 1) / kTileEdge;
  const uint32_t tiles_per_column = (height + kTileEdge - 1) / kTileEdge;
  const uint32_t tile_index = address_delta / tile_bytes;
  if (uint64_t(tile_index) >= uint64_t(tiles_per_row) * tiles_per_column) return std::nullopt;
  return Point{(tile_index % tiles_per_row) * kTileEdge,
               (tile_index / tiles_per_row) * kTileEdge};
}

inline std::optional<Point> FindSegmentOrigin(uint32_t root_address, uint32_t segment_address,
                                              uint32_t pitch, uint32_t height,
                                              uint32_t bytes_per_pixel_log2) {
  if (segment_address < root_address) return std::nullopt;
  return FindTileOrigin(segment_address - root_address, pitch, height,
                        bytes_per_pixel_log2);
}

inline std::optional<uint32_t> TileAddressOffset(Point origin, uint32_t pitch, uint32_t height,
                                                 uint32_t bytes_per_pixel_log2) {
  constexpr uint32_t kTileEdge = 32;
  if (!pitch || !height || bytes_per_pixel_log2 < 2 || bytes_per_pixel_log2 > 3 ||
      origin.x % kTileEdge || origin.y % kTileEdge || origin.x >= pitch || origin.y >= height) {
    return std::nullopt;
  }
  const uint64_t tiles_per_row = (uint64_t(pitch) + kTileEdge - 1) / kTileEdge;
  const uint64_t tile_index = uint64_t(origin.y / kTileEdge) * tiles_per_row +
                              origin.x / kTileEdge;
  const uint64_t offset = tile_index * (uint64_t(1) << (10 + bytes_per_pixel_log2));
  if (offset > UINT32_MAX) return std::nullopt;
  return uint32_t(offset);
}

inline std::optional<CopyRegion> MakeCopyRegion(Point tile_origin, uint32_t source_x,
                                                uint32_t source_y, uint32_t width,
                                                uint32_t height, uint32_t source_width,
                                                uint32_t source_height, uint32_t destination_width,
                                                uint32_t destination_height) {
  if (!width || !height || source_x > source_width || source_y > source_height ||
      width > source_width - source_x || height > source_height - source_y ||
      tile_origin.x > destination_width || tile_origin.y > destination_height ||
      source_x > destination_width - tile_origin.x ||
      source_y > destination_height - tile_origin.y) {
    return std::nullopt;
  }
  const uint32_t destination_x = tile_origin.x + source_x;
  const uint32_t destination_y = tile_origin.y + source_y;
  if (width > destination_width - destination_x ||
      height > destination_height - destination_y) {
    return std::nullopt;
  }
  return CopyRegion{source_x, source_y, destination_x, destination_y, width, height};
}

// One guest surface, addressed either at its base or at a validated tile origin.
// This plans a direct RT -> shared destination write, not an image composition.
struct SharedWrite {
  uint32_t root_address = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  Point origin;
  CopyRegion region;
};

inline std::optional<SharedWrite> PlanSharedWrite(
    uint32_t address, uint32_t pitch, uint32_t height, Point origin,
    bool window_mapping_valid, uint32_t source_x, uint32_t source_y,
    uint32_t width, uint32_t copy_height, uint32_t source_width, uint32_t source_height) {
  // Only the verified 32-bit tiled address layout is supported here.
  if (!window_mapping_valid || !address || (address & 4095u) || !pitch || !height ||
      pitch > 8192 || height > 8192 || (pitch & 31u)) return std::nullopt;
  auto offset = TileAddressOffset(origin, pitch, height, 2);
  if (!offset || address < *offset) return std::nullopt;
  const uint32_t root = address - *offset;
  const uint64_t span = uint64_t(pitch) * ((uint64_t(height) + 31u) & ~uint64_t(31u)) * 4;
  if (!root || uint64_t(root) + span > 0x20000000ull) return std::nullopt;
  auto region = MakeCopyRegion(origin, source_x, source_y, width, copy_height,
                               source_width, source_height, pitch, height);
  if (!region) return std::nullopt;
  return SharedWrite{root, pitch, height, origin, *region};
}

}  // namespace fear_nx::native::resolve_layout
