// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace fear_nx::native {

// Conservatively permits dropping the old depth image only when this clear,
// after clipping to the active pass, covers every pixel in the allocated
// depth image (including any padded rows). Offset clears are rejected even
// when an oversized rectangle might happen to cover the image after clipping.
// Callers must additionally establish that the clear replaces both depth and
// stencil and that no preservation copy is needed for another alias.
constexpr bool CanDiscardDepthForClear(uint32_t depth_width, uint32_t depth_height,
                                      int32_t x, int32_t y, uint32_t width,
                                      uint32_t height, uint32_t pass_width,
                                      uint32_t pass_height) {
  if (!depth_width || !depth_height || !width || !height || x != 0 || y != 0) {
    return false;
  }

  // Compare in 64 bits so callers can safely pass oversized clear extents.
  const uint64_t clear_right = uint64_t(uint32_t(x)) + uint64_t(width);
  const uint64_t clear_bottom = uint64_t(uint32_t(y)) + uint64_t(height);
  return clear_right >= uint64_t(depth_width) &&
         clear_bottom >= uint64_t(depth_height) &&
         pass_width >= depth_width && pass_height >= depth_height;
}

}  // namespace fear_nx::native
