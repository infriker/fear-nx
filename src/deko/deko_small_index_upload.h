// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

namespace fear_nx::deko {

// Only transient, freshly reserved destinations belong here. Resident cache
// hits/admissions keep their existing allocation and lifetime. Every queued
// payload is snapshotted now, before its CPU conversion cache can be reset.
// The caller MUST Flush before submitting commands or recycling a work slot.
template <uint32_t ChunkBytes, uint32_t MaxPayloadBytes>
class SmallUploadBatch {
 public:
  static_assert(ChunkBytes >= MaxPayloadBytes && (ChunkBytes % 16) == 0);
  static constexpr uint32_t kChunkBytes = ChunkBytes;
  static constexpr uint32_t kMaxPayloadBytes = MaxPayloadBytes;
  struct Upload {
    uint8_t* cpu = nullptr;
    uint64_t gpu = 0;
    explicit operator bool() const { return cpu != nullptr; }
  };

  SmallUploadBatch() = default;
  SmallUploadBatch(const SmallUploadBatch&) = delete;
  SmallUploadBatch& operator=(const SmallUploadBatch&) = delete;

  template <typename Allocate, typename Write>
  Upload Append(std::span<const uint8_t> source, Allocate&& allocate, Write&& write,
                uint64_t minimum_gpu = 0) {
    if (source.empty() || source.size() > kMaxPayloadBytes) return {};
    uint32_t offset = (used_ + 15u) & ~15u;
    if (!chunk_ || source.size() > kChunkBytes - offset || chunk_.gpu + offset < minimum_gpu) {
      Flush(write);
      chunk_ = allocate(kChunkBytes, 16u);
      if (!chunk_) return {};  // Caller may still fit an exact small allocation.
      // Vertex SSBO rebasing cannot represent a negative fetch base. The
      // allocator must reserve beyond the proven descriptor-relative start.
      if (chunk_.gpu < minimum_gpu) { chunk_ = {}; return {}; }
      offset = 0;
    }
    std::memcpy(shadow_.data() + offset, source.data(), source.size());
    used_ = offset + uint32_t(source.size());
    return {chunk_.cpu + offset, chunk_.gpu + offset};
  }

  template <typename Write>
  void Flush(Write&& write) {
    if (used_) write(chunk_.cpu, shadow_.data(), used_);
    used_ = 0;
    chunk_ = {};
  }

  bool pending() const { return used_ != 0; }

 private:
  alignas(16) std::array<uint8_t, kChunkBytes> shadow_{};
  Upload chunk_{};
  uint32_t used_ = 0;
};

using SmallIndexUpload = SmallUploadBatch<4096, 2048>;
using SmallVertexUpload = SmallUploadBatch<128u << 10, 64u << 10>;

}  // namespace fear_nx::deko
