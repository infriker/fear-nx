// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fear_nx::deko {
struct ImageAccess {
  enum : uint32_t { Fragment=1,Vertex=2,Write=4 };
  uint64_t image=0;
  uint32_t flags=0;
};

// Whole-image dependencies: views, formats and crops of one allocation share
// an identity. ROP-to-ROP writes stay ordered; RAW/WAR require a texture-cache
// invalidation and the appropriate graphics drain. Untracked engines retain
// the caller's full barriers. A full table never silently forgets a hazard.
class ImageAccessTracker {
 public:
  enum class Action { None,Fragments,Primitives,Full };
  Action Before(std::span<const ImageAccess> accesses) const {
    if(overflow_)return Action::Full;
    Action result=Action::None;
    for(const auto& access:accesses) {
      if(!access.image)continue;
      const auto* e=Find(access.image);
      if(!e)continue;
      const bool raw=(access.flags&(ImageAccess::Fragment|ImageAccess::Vertex)) && e->write>fragments_;
      const bool war_fragment=(access.flags&ImageAccess::Write) && e->fragment>fragments_;
      const bool war_vertex=(access.flags&ImageAccess::Write) && e->vertex>primitives_;
      if(war_vertex || (raw && (access.flags&ImageAccess::Vertex)))return Action::Primitives;
      if(raw || war_fragment)result=Action::Fragments;
    }
    return result;
  }
  void Record(std::span<const ImageAccess> accesses) {
    ++serial_;
    for(const auto& access:accesses) {
      if(!access.image)continue;
      auto* e=Insert(access.image);
      if(!e){overflow_=true;continue;}
      if(access.flags&ImageAccess::Fragment)e->fragment=serial_;
      if(access.flags&ImageAccess::Vertex)e->vertex=serial_;
      if(access.flags&ImageAccess::Write)e->write=serial_;
    }
  }
  void Complete(Action action) {
    if(action==Action::Fragments)fragments_=serial_;
    else if(action==Action::Primitives)fragments_=primitives_=serial_;
    else if(action==Action::Full) {
      fragments_=primitives_=serial_;overflow_=false;
      if(!++epoch_){entries_={};epoch_=1;}
    }
  }
  bool overflow() const {return overflow_;}
 private:
  struct Entry {uint64_t image=0,epoch=0,write=0,fragment=0,vertex=0;};
  static constexpr size_t kEntries=2048,kProbes=8;
  static size_t Bucket(uint64_t image) {image*=0x9E3779B185EBCA87ULL;return (image^(image>>32))&(kEntries-1);}
  const Entry* Find(uint64_t image) const {
    const size_t at=Bucket(image);
    for(size_t i=0;i<kProbes;++i) {
      const auto& e=entries_[(at+i)&(kEntries-1)];
      if(e.epoch!=epoch_)return nullptr;
      if(e.image==image)return &e;
    }
    return nullptr;
  }
  Entry* Insert(uint64_t image) {
    const size_t at=Bucket(image);
    for(size_t i=0;i<kProbes;++i) {
      auto& e=entries_[(at+i)&(kEntries-1)];
      if(e.epoch!=epoch_){e={image,epoch_,0,0,0};return &e;}
      if(e.image==image)return &e;
    }
    return nullptr;
  }
  std::array<Entry,kEntries> entries_{};
  uint64_t epoch_=1,serial_=0,fragments_=0,primitives_=0;
  bool overflow_=false;
};
} // namespace fear_nx::deko
