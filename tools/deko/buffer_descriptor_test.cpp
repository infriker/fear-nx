// SPDX-License-Identifier: GPL-3.0-or-later
#include "deko/deko_buffer_descriptor.h"
#include <algorithm>
#include <iostream>

using namespace fear_nx::deko;
#define CHECK(x) do {if(!(x)){std::cerr<<__LINE__<<": " #x "\n";return 1;}}while(false)

// Model the size selection in Eden 58c1e20ee5 StorageBufferBinding. The
// source-extracted reproducer in the check artifacts verifies the same rule
// against the actual installed Eden implementation without a GPU/game launch.
static uint32_t EdenSize(const DkBufExtents& descriptor,uint32_t mapped_bytes) {
  std::array<uint64_t,2> wire;
  std::memcpy(wire.data(),&descriptor,sizeof(wire));
  if(!(wire[1]>>32))return uint32_t(wire[1]);
  return std::min(mapped_bytes,8u<<20);
}

int main() {
  constexpr uint64_t address=0x1234000000ull;
  constexpr uint32_t capacity=256u<<20;
  struct Guarded {uint64_t before=0xDEADBEEFull;DkBufExtents descriptor;uint64_t after=0xBADC0FFEEull;} value;
  for(uint32_t poison:{1u,0x3F800000u,0xAAAAAAAAu,UINT32_MAX}) {
    const std::array<uint64_t,2> old_wire{address,(uint64_t(poison)<<32)|capacity};
    std::memcpy(&value.descriptor,old_wire.data(),sizeof(old_wire));
    CHECK(value.descriptor.addr==address&&value.descriptor.size==capacity);
    CHECK(EdenSize(value.descriptor,capacity)==(8u<<20)); // reproduces bad extent
    InitializeStorageBufferDescriptor(value.descriptor,address,capacity);
    CHECK(value.before==0xDEADBEEFull&&value.after==0xBADC0FFEEull);
    std::array<uint64_t,2> fixed_wire;
    std::memcpy(fixed_wire.data(),&value.descriptor,sizeof(fixed_wire));
    CHECK(fixed_wire[0]==address&&fixed_wire[1]==capacity);
    CHECK(EdenSize(value.descriptor,capacity)==capacity);
    // Transient vertices begin at 24 MiB in each slot; all four must remain
    // visible, as must cached geometry in a different slot from the draw.
    for(uint32_t slot=0;slot<4;++slot) {
      const uint32_t vertex=slot*(64u<<20)+(24u<<20);
      CHECK(vertex+65536<=EdenSize(value.descriptor,capacity));
      CHECK(vertex>=(8u<<20));
    }
  }
  for(uint32_t size:{0u,16u,4096u,64u<<20,capacity,UINT32_MAX}) {
    InitializeStorageBufferDescriptor(value.descriptor,address,size);
    CHECK(EdenSize(value.descriptor,capacity)==size);
  }
  std::cout<<"descriptor ABI: poisoned tail reproduces Eden 8 MiB fallback; zero tail preserves 256 MiB and all four slots\n";
}
