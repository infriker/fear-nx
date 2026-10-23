// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <deko3d.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace fear_nx::deko {

// deko3d copies sizeof(DkBufExtents), including its four trailing padding
// bytes, into the graphics driver constant buffer. Maxwell ignores that tail,
// but Eden uses it to distinguish a size from another GPU address: a nonzero
// tail selects its 8 MiB fallback instead of our complete geometry heap.
// Serialize all 16 bytes in place; returning/assigning a struct by value would
// again permit its C++ padding to become unspecified.
inline void InitializeStorageBufferDescriptor(DkBufExtents& descriptor,
                                               DkGpuAddr address,uint32_t size) {
  static_assert(std::is_trivially_copyable_v<DkBufExtents>);
  static_assert(sizeof(DkBufExtents)==16 && offsetof(DkBufExtents,addr)==0 &&
                offsetof(DkBufExtents,size)==8 && sizeof(descriptor.size)==4);
  const std::array<uint64_t,2> words{address,uint64_t(size)};
  std::memcpy(&descriptor,words.data(),sizeof(words));
}

} // namespace fear_nx::deko
