// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "deko_image_access.h"

namespace fear_nx::deko {
// Per-image graphics dependencies with conservative queue-engine boundaries. A work-slot
// boundary is deliberately unknown: the presenter and SDK also use the queue.
// Graphics clears/draws are ordered by the ROPs. Sampling their results needs
// a fragment barrier; copy-engine/compute dependencies still need a full drain.
class ImageBarrierPolicy {
 public:
  using Action=ImageAccessTracker::Action;
  void BeginWork() { unknown_=true; }
  void FullBarrier() { unknown_=graphics_=copy_=transfer_=vertex_reads_=untracked_graphics_=false;images_.Complete(Action::Full); }
  Action BeforeImages(std::span<const ImageAccess> accesses,bool optimized) {
    if(!optimized || unknown_ || transfer_ || untracked_graphics_) {FullBarrier();return Action::Full;}
    const auto action=images_.Before(accesses);
    if(action==Action::Full){++capacity_drains;FullBarrier();}
    else images_.Complete(action);
    return action;
  }
  void Images(std::span<const ImageAccess> accesses) {
    images_.Record(accesses);graphics_=true;
    for(const auto& access:accesses)vertex_reads_|=bool(access.flags&ImageAccess::Vertex);
  }
  uint64_t capacity_drains=0;
  Action BeforeGraphics(bool samples_rendered, bool helper, bool optimized) {
    if (!optimized || unknown_ || transfer_) { FullBarrier(); return Action::Full; }
    if (copy_ || (samples_rendered && graphics_) || (helper && vertex_reads_)) {
      const auto action=vertex_reads_?Action::Primitives:Action::Fragments;
      graphics_=copy_=vertex_reads_=untracked_graphics_=false;
      images_.Complete(action);return action;
    }
    return Action::None;
  }
  Action BeforeTransfer(bool optimized) {
    if (!optimized || unknown_ || graphics_) { FullBarrier(); return Action::Full; }
    return Action::None;
  }
  void Graphics(bool copy=false, bool vertex_reads=false) {
    graphics_=true;copy_|=copy;vertex_reads_|=vertex_reads;untracked_graphics_=true;
  }
  void Transfer() { transfer_=true; }
 private:
  ImageAccessTracker images_;
  bool unknown_=true,graphics_=false,copy_=false,transfer_=false,vertex_reads_=false,untracked_graphics_=false;
};
} // namespace fear_nx::deko
