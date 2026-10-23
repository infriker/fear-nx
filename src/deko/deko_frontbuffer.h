// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <utility>
namespace fear_nx::deko {
struct FrontbufferResolve {
  uint32_t address=0,last_frontbuffer=0,source_width=0,source_height=0;
  uint32_t destination_width=0,destination_height=0,x=0,y=0,dx=0,dy=0,width=0,height=0;
  bool compatible_color=false,swapped_channels=false,alias=false;
};
inline bool CanDeferFrontbuffer(const FrontbufferResolve& p) {
  return p.address && p.address==p.last_frontbuffer && p.compatible_color &&
      !p.swapped_channels && !p.alias && !p.x && !p.y && !p.dx && !p.dy &&
      p.width && p.height && p.width==p.destination_width && p.height==p.destination_height &&
      p.width<=p.source_width && p.height<=p.source_height;
}
// Clears queued after capture belong to the future RT contents, not this snapshot.
template<class Image,class Copy>
void ReadBeforeQueuedClears(Image& image,Copy&& copy) {
  struct Restore {
    Image& target;
    decltype(image.pending_clear) pending;
    ~Restore(){target.pending_clear=std::move(pending);}
  } restore{image,std::move(image.pending_clear)};
  image.pending_clear={};
  copy();
}
}
