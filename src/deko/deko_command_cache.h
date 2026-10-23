// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

namespace fear_nx::deko {
// Tracks the last commands actually emitted, not the guest register state.
// Reset after helpers or command-list rotation, even if guest state is equal.
class CommandStateCache {
 public:
  enum Slot { Targets, Shaders, UniformVS, UniformPS, UniformGS, Storage,
    TexturesVS, TexturesPS, Raster, Multisample, SampleMask, DepthStencil,
    Stencil, Color, Blends, ColorWrite, BlendConstant, Viewport, Scissor,
    DepthBias, Restart, EmptyVertex, Index, Count };
  void Invalidate() {
    if (++epoch_ == 0) { for(auto& entry:entries_)entry.epoch=0;epoch_=1; }
  }
  bool Update(Slot slot, const void* data, size_t size, bool enabled = true) {
    if (unsigned(slot) >= Count) return true;
    auto& entry=entries_[slot];
    if (size > kMaxBytes) {entry.epoch=0;return true;} // Unretained emission replaces prior state.
    if(enabled && entry.epoch==epoch_ && entry.size==size &&
       (!size || !std::memcmp(entry.bytes.data(),data,size)))return false;
    if(size)std::memcpy(entry.bytes.data(),data,size);
    entry.size=size;entry.epoch=epoch_;return true;
  }
  template<class T> bool Update(Slot slot,const T& value,bool enabled=true) {
    static_assert(std::is_trivially_copyable_v<T>);
    return Update(slot,&value,sizeof(value),enabled);
  }
 private:
  static constexpr size_t kMaxBytes=256;
  struct Entry { std::array<uint8_t,kMaxBytes> bytes{};size_t size=0;uint64_t epoch=0; };
  std::array<Entry,Count> entries_{};
  uint64_t epoch_=1;
};

// The guest draw path samples immutable guest textures or separate resolved
// images, never its current attachment. ImageBarrierPolicy separately orders
// image operations (including helpers). Consecutive guest draws therefore
// require no full execution drain; fresh CPU uploads still invalidate shader
// data caches. Ordered PushConstants commands have their own ordering.
class DrawBarrierPolicy {
 public:
  enum class Action { None, InvalidateData, Full };
  void BeginWork() { synchronized_=false;cpu_upload_=false; }
  void CpuUpload() { cpu_upload_=true; }
  void FullBarrier() { synchronized_=true;cpu_upload_=false; }
  Action BeforeDraw(bool optimized) {
    if(!optimized || !synchronized_) {FullBarrier();return Action::Full;}
    if(cpu_upload_) {cpu_upload_=false;return Action::InvalidateData;}
    return Action::None;
  }
 private:
  bool synchronized_=false,cpu_upload_=false;
};
} // namespace fear_nx::deko
