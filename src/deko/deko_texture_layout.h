// SPDX-License-Identifier: GPL-3.0-or-later
// Small, independently testable Deko3D texture transfer layout helpers.
#pragma once

#include <deko3d.h>

#include <cstdint>
#include <limits>

namespace fear_nx::deko::detail {

// Deko3D's DkCopyBuf rowLength and imageHeight are byte strides. In
// particular, rowLength is not the Vulkan bufferRowLength texel count.
inline bool MakeTextureCopyBuffer(DkGpuAddr address, uint64_t row_bytes,
                                  uint32_t block_rows, uint64_t layer_bytes,
                                  DkCopyBuf& result) {
  if (address == DK_GPU_ADDR_INVALID || !row_bytes || !block_rows || !layer_bytes ||
      row_bytes > std::numeric_limits<uint32_t>::max() ||
      layer_bytes > std::numeric_limits<uint32_t>::max() ||
      row_bytes > std::numeric_limits<uint64_t>::max() / block_rows ||
      layer_bytes < row_bytes * block_rows) {
    return false;
  }
  result = {address, uint32_t(row_bytes), uint32_t(layer_bytes)};
  return true;
}

}  // namespace fear_nx::deko::detail
