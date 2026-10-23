// SPDX-License-Identifier: GPL-3.0-or-later
// Regression checks for Deko3D's byte-stride texture upload ABI.

#include <deko/deko_texture_layout.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::vector<uint8_t> ReconstructDekoCopy(const std::vector<uint8_t>& staging,
                                         const DkCopyBuf& copy, uint64_t row_bytes,
                                         uint32_t block_rows, uint64_t layer_bytes,
                                         uint32_t layers) {
  const uint64_t last_read = uint64_t(layers - 1) * copy.imageHeight +
                             uint64_t(block_rows - 1) * copy.rowLength + row_bytes;
  std::vector<uint8_t> guarded = staging;
  if (guarded.size() < last_read) {
    guarded.resize(size_t(last_read), 0xCD);
  }
  std::vector<uint8_t> reconstructed(size_t(layer_bytes * layers), 0);
  for (uint32_t layer = 0; layer < layers; ++layer) {
    for (uint32_t row = 0; row < block_rows; ++row) {
      const size_t src = size_t(layer) * copy.imageHeight + size_t(row) * copy.rowLength;
      const size_t dst = size_t(layer) * layer_bytes + size_t(row) * row_bytes;
      std::memcpy(reconstructed.data() + dst, guarded.data() + src, size_t(row_bytes));
    }
  }
  return reconstructed;
}

void CheckCopyRoundTrip(const char* name, uint32_t width, uint32_t height,
                        uint32_t block_width, uint32_t block_height,
                        uint32_t bytes_per_block, uint32_t layers) {
  const uint32_t blocks_x = (width + block_width - 1) / block_width;
  const uint32_t blocks_y = (height + block_height - 1) / block_height;
  const uint64_t row_bytes = uint64_t(blocks_x) * bytes_per_block;
  const uint64_t layer_bytes = row_bytes * blocks_y;
  const size_t total_bytes = size_t(layer_bytes * layers);
  std::vector<uint8_t> staging(total_bytes);

  for (uint32_t layer = 0; layer < layers; ++layer) {
    for (uint32_t row = 0; row < blocks_y; ++row) {
      for (uint32_t byte = 0; byte < row_bytes; ++byte) {
        staging[size_t(layer) * layer_bytes + size_t(row) * row_bytes + byte] =
            uint8_t(17 * layer + 31 * row + byte);
      }
    }
  }

  DkCopyBuf copy{};
  Require(fear_nx::deko::detail::MakeTextureCopyBuffer(
              0x1000, row_bytes, blocks_y, layer_bytes, copy),
          "DkCopyBuf construction rejected a valid upload");
  Require(copy.rowLength == row_bytes, "DkCopyBuf rowLength is not a byte pitch");
  Require(copy.imageHeight == layer_bytes, "DkCopyBuf imageHeight is not a byte layer stride");

  // Deko3D's transfer source treats these fields as byte strides. Reconstruct
  // the exact rows/layers the DMA engine reads and compare with the staging
  // image. The row count is in blocks for BC formats, as Deko adjusts the
  // destination dimensions to block dimensions before issuing the copy.
  Require(ReconstructDekoCopy(staging, copy, row_bytes, blocks_y, layer_bytes, layers) == staging,
          "Deko byte-stride reconstruction changed texture rows");

  // Verify the former Vulkan-style texel-count parameters are observably bad
  // for these multi-byte formats, so this test catches a regression to them.
  const DkCopyBuf old_texel_counts = {0x1000, blocks_x * block_width,
                                      blocks_y * block_height};
  Require(ReconstructDekoCopy(staging, old_texel_counts, row_bytes, blocks_y, layer_bytes,
                              layers) != staging,
          "test pattern did not detect Vulkan-style DkCopyBuf strides");
  std::cout << "PASS DkCopyBuf byte strides " << name << '\n';
}

void CheckCopyLayoutRejectsOverflow() {
  DkCopyBuf copy{};
  Require(!fear_nx::deko::detail::MakeTextureCopyBuffer(
              0x1000, uint64_t(UINT32_MAX) + 1, 1, uint64_t(UINT32_MAX) + 1, copy),
          "DkCopyBuf accepted an unrepresentable byte pitch");
  Require(!fear_nx::deko::detail::MakeTextureCopyBuffer(0x1000, 16, 4, 63, copy),
          "DkCopyBuf accepted a short layer stride");
  Require(!fear_nx::deko::detail::MakeTextureCopyBuffer(
              DK_GPU_ADDR_INVALID, 16, 4, 64, copy),
          "DkCopyBuf accepted an invalid GPU address");
  std::cout << "PASS DkCopyBuf bounds\n";
}

}  // namespace

int main() {
  try {
    CheckCopyRoundTrip("RGBA8 5x3 x2", 5, 3, 1, 1, 4, 2);
    CheckCopyRoundTrip("BC1 10x7 x3", 10, 7, 4, 4, 8, 3);
    CheckCopyLayoutRejectsOverflow();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
