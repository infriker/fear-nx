// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace fear_nx::native {

// Attachment load clears apply to the active render area, while
// vkCmdClearAttachments applies only to the intersection with its clear rect.
// This conservative check accepts only origin-zero rectangles that cover the
// complete render area. It uses comparisons instead of endpoint addition, so
// oversized extents cannot overflow.
constexpr bool CanUseAttachmentLoadClear(int32_t x, int32_t y,
                                         uint32_t width, uint32_t height,
                                         uint32_t render_width,
                                         uint32_t render_height) noexcept {
  return render_width != 0 && render_height != 0 && x == 0 && y == 0 &&
         width >= render_width && height >= render_height;
}

}  // namespace fear_nx::native
