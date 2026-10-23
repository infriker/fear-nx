// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "deko_index_write.h"

namespace fear_nx::deko {

// Preserve guest vertex bytes exactly: F.E.A.R.'s translated vertex fetch does
// the endian conversion in the shader. Only the sequential store technique is
// shared with the index writer, not index conversion or Dante's CPU byte swap.
inline uint32_t WriteVertexBytes(uint8_t* destination,const uint8_t* source,
                                 uint32_t bytes,uint32_t owned_bytes) {
  return WriteIndexBytes(destination,source,bytes,owned_bytes);
}

class ResidentVertexUploadBatch {
 public:
  static constexpr uint32_t kCapacity=256u<<10;
  static constexpr uint32_t kMaxPayloadBytes=64u<<10;
  template <typename Write>
  bool Append(uint8_t* destination,uint64_t gpu,std::span<const uint8_t> source,
              uint32_t owned_bytes,Write&& write) {
    // Large blocks go directly through the sequential writer, without an
    // extra CPU snapshot. Small new blocks can amortize several GPU writes.
    if(source.size()>kMaxPayloadBytes || owned_bytes>kMaxPayloadBytes)return false;
    return batch_.Append(destination,gpu,source,owned_bytes,write);
  }
  template <typename Write> void Flush(Write&& write) {batch_.Flush(write);}
  bool pending() const {return batch_.pending();}
 private:
  ResidentUploadBatch<kCapacity> batch_;
};
} // namespace fear_nx::deko
