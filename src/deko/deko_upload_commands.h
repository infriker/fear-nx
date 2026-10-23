// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace fear_nx::deko {

// dkCmdBufPushData accepts at most 0x7FFC bytes. Keeping chunks 16-byte
// aligned also preserves the padding/ownership contract of WriteVertexBytes.
inline constexpr uint32_t kMaxUploadCommandBytes = 0x7FF0;

// Emit must consume/copy each source span synchronously: the final padded
// span lives on the stack. Complete bounds are checked before the first emit.
// As with WriteVertexBytes, the caller owns capacity bytes at destination and
// supplies at least bytes readable source bytes. Source alignment is arbitrary.
template <typename Emit>
uint32_t EmitUploadCommands(uint64_t destination, const uint8_t* source,
                            uint32_t bytes, uint32_t capacity, Emit&& emit) {
  if (!destination || !source || !bytes) return 0;
  const uint64_t written = (uint64_t(bytes) + 15) & ~uint64_t(15);
  const uintptr_t source_address = reinterpret_cast<uintptr_t>(source);
  if (written > capacity || written > std::numeric_limits<uint32_t>::max() ||
      destination > std::numeric_limits<uint64_t>::max() - written ||
      bytes > std::numeric_limits<uintptr_t>::max() - source_address) {
    return 0;
  }

  const uint32_t full_bytes = bytes & ~uint32_t(15);
  uint32_t offset = 0;
  while (offset < full_bytes) {
    const uint32_t remaining = full_bytes - offset;
    const uint32_t chunk = remaining < kMaxUploadCommandBytes
                               ? remaining : kMaxUploadCommandBytes;
    emit(destination + offset, source + offset, chunk);
    offset += chunk;
  }
  if (full_bytes != bytes) {
    alignas(16) uint8_t tail[16]{};
    std::memcpy(tail, source + full_bytes, bytes - full_bytes);
    emit(destination + full_bytes, tail, uint32_t(16));
  }
  return static_cast<uint32_t>(written);
}

// A prefix can stay zero across fenced slot reuse only while it stays outside
// the allocator. Record a shrink even when no clear is needed: the released
// suffix may be allocated/written before a later request grows the prefix.
// Call after selecting/reserving the real slot. A true result obligates the
// caller to initialize that prefix before use; this object does no GPU work.
template <size_t Slots>
class ZeroPrefixTracker {
 public:
  bool NeedsInitialize(size_t slot, const void* cpu, uint64_t gpu,
                       uint32_t protected_bytes) {
    if (slot >= Slots) return true;
    Entry& entry = entries_[slot];
    const bool needs_initialize =
        !entry.valid || entry.cpu != cpu || entry.gpu != gpu ||
        protected_bytes > entry.protected_bytes;
    entry = {cpu, gpu, protected_bytes, true};
    return needs_initialize;
  }

 private:
  struct Entry {
    const void* cpu = nullptr;
    uint64_t gpu = 0;
    uint32_t protected_bytes = 0;
    bool valid = false;
  };
  std::array<Entry, Slots> entries_{};
};

}  // namespace fear_nx::deko
