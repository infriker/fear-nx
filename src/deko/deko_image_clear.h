// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace fear_nx::deko {
struct ClearRect {
  uint32_t x=0,y=0,w=0,h=0;
  bool empty() const { return !w || !h; }
  uint64_t pixels() const { return uint64_t(w)*h; }
  bool operator==(const ClearRect&) const = default;
};
inline ClearRect ClipClearRect(uint32_t width,uint32_t height,int32_t x,int32_t y,uint32_t w,uint32_t h) {
  const int64_t x0=std::clamp<int64_t>(x,0,width), y0=std::clamp<int64_t>(y,0,height);
  const int64_t x1=std::clamp<int64_t>(int64_t(x)+w,0,width), y1=std::clamp<int64_t>(int64_t(y)+h,0,height);
  return {uint32_t(x0),uint32_t(y0),uint32_t(std::max<int64_t>(0,x1-x0)),uint32_t(std::max<int64_t>(0,y1-y0))};
}
inline ClearRect IntersectClearRect(ClearRect a,ClearRect b) {
  const auto x=std::max(a.x,b.x),y=std::max(a.y,b.y);
  const uint64_t right=std::min(uint64_t(a.x)+a.w,uint64_t(b.x)+b.w);
  const uint64_t bottom=std::min(uint64_t(a.y)+a.h,uint64_t(b.y)+b.h);
  return {x,y,uint32_t(right>x?right-x:0),uint32_t(bottom>y?bottom-y:0)};
}
inline bool ContainsClearRect(ClearRect a,ClearRect b) { return IntersectClearRect(a,b)==b; }
inline ClearRect BoundingClearRect(ClearRect a,ClearRect b) {
  const auto x=std::min(a.x,b.x),y=std::min(a.y,b.y);
  return {x,y,uint32_t(std::max(uint64_t(a.x)+a.w,uint64_t(b.x)+b.w)-x),
              uint32_t(std::max(uint64_t(a.y)+a.h,uint64_t(b.y)+b.h)-y)};
}
// Rectangular union only. Never fill a hole left by an intervening draw.
inline bool MergeClearRect(ClearRect a,ClearRect b,ClearRect& merged) {
  const auto box=BoundingClearRect(a,b);
  if(box.pixels()!=a.pixels()+b.pixels()-IntersectClearRect(a,b).pixels())return false;
  merged=box;return true;
}
struct ImageClear {
  ClearRect rect;
  bool depth=false,clear_z=false;
  uint8_t mask=0,stencil=0;
  float z=0;
  std::array<float,4> color{};
  bool CoversAspects(const ImageClear& old) const {
    return depth==old.depth && (!depth || ((!old.clear_z || clear_z) && (mask&old.mask)==old.mask));
  }
  bool SameValueFor(const ImageClear& other) const {
    if (!CoversAspects(other)) return false;
    if (!depth) return !std::memcmp(color.data(),other.color.data(),sizeof(color));
    return (!other.clear_z || !std::memcmp(&z,&other.z,sizeof(z))) && !((stencil^other.stencil)&other.mask);
  }
};

// Ordered, bounded rectangles belong to the image, not the command-list slot.
// Consume only the area about to be observed or modified. Newly exposed bands
// are therefore initialized before use, without assuming allocated RT padding
// is visible. Different depth/stencil masks retain their original order.
class ImageClearQueue {
 public:
  static constexpr uint32_t kLimit=16;
  struct Batch {
    std::array<ImageClear,kLimit> items{};
    uint32_t count=0,merged=0;
    bool drained=false,expanded=false;
    uint64_t early_pixels=0;
  };
  enum class Result { Queued, Repeated, Full };
  uint32_t size() const { return pending_.count; }
  bool empty() const { return !size(); }
  Result Queue(const ImageClear& clear,uint32_t& replaced,bool coalesce=false,uint32_t* merged=nullptr) {
    if(merged)*merged=0;
    replaced=0;
    if (known_valid_ && ContainsClearRect(known_.rect,clear.rect) && known_.SameValueFor(clear)) return Result::Repeated;
    for(uint32_t i=0;i<size();++i) {
      const auto& old=pending_.items[i];
      if(ContainsClearRect(clear.rect,old.rect) && clear.CoversAspects(old))++replaced;
    }
    // Merge the last operation with the new one before declaring overflow.
    // No other pending operation is crossed or reordered.
    if(coalesce && size() && Equivalent(pending_.items[size()-1],clear)) {
      ClearRect joined;
      if(MergeClearRect(pending_.items[size()-1].rect,clear.rect,joined)) {
        pending_.items[size()-1].rect=joined;known_=clear;known_.rect=joined;known_valid_=true;
        replaced=0;if(merged)*merged=1;return Result::Queued;
      }
    }
    if(size()-replaced==kLimit) {replaced=0;return Result::Full;}
    uint32_t kept=0;
    for(uint32_t i=0;i<size();++i) {
      const auto old=pending_.items[i];
      if(!ContainsClearRect(clear.rect,old.rect) || !clear.CoversAspects(old))pending_.items[kept++]=old;
    }
    pending_.items[kept++]=clear;pending_.count=kept;known_=clear;known_valid_=true;
    if(coalesce){const auto n=Compact(pending_);if(merged)*merged+=n;}
    return Result::Queued;
  }
  Batch TakeAll(bool coalesce=false) {auto result=pending_;pending_.count=0;if(coalesce)result.merged=Compact(result);return result;}
  Batch Consume(ClearRect area,bool coalesce=false) {
    const auto requested=area;
    if(coalesce && !area.empty()) {
      // When >=75% of a pending rectangle is needed, clearing it as one piece
      // avoids almost-certain later bands. Apply the expanded area to EVERY
      // pending operation, preserving ordering of overlapping aspect writes.
      for(uint32_t i=0;i<size();++i) {
        const auto rect=pending_.items[i].rect,hit=IntersectClearRect(rect,area);
        if(!hit.empty() && hit.pixels()*4>=rect.pixels()*3)area=BoundingClearRect(area,rect);
      }
    }
    Batch emit,keep;
    for(uint32_t i=0;i<size();++i) {
      auto clear=pending_.items[i];const auto rect=clear.rect;
      const auto hit=IntersectClearRect(rect,area);
      auto retain=[&](ClearRect r) {
        if(r.empty())return true;
        if(keep.count==kLimit)return false;
        clear.rect=r;keep.items[keep.count++]=clear;return true;
      };
      bool fits=true;
      if(hit.empty())fits=retain(rect);
      else {
        clear.rect=hit;emit.items[emit.count++]=clear;
        // Disjoint top/bottom strips, then left/right within the hit's rows.
        fits=retain({rect.x,rect.y,rect.w,hit.y-rect.y}) &&
             retain({rect.x,hit.y+hit.h,rect.w,rect.y+rect.h-hit.y-hit.h}) &&
             retain({rect.x,hit.y,hit.x-rect.x,hit.h}) &&
             retain({hit.x+hit.w,hit.y,rect.x+rect.w-hit.x-hit.w,hit.h});
      }
      // No commands have escaped yet. On fragmentation, execute the original
      // list in order rather than dropping an area or growing without bound.
      if(!fits){auto all=TakeAll(coalesce);all.drained=true;return all;}
    }
    if(coalesce)emit.merged=Compact(emit)+Compact(keep);
    if(area!=requested) {
      emit.expanded=true;
      for(uint32_t i=0;i<emit.count;++i)emit.early_pixels+=emit.items[i].rect.pixels()-IntersectClearRect(emit.items[i].rect,requested).pixels();
    }
    pending_=keep;return emit;
  }
  uint32_t DiscardForFullColorOverwrite(bool full) {
    if(!full)return 0;
    for(uint32_t i=0;i<size();++i)if(pending_.items[i].depth)return 0;
    const auto count=size();pending_.count=0;known_valid_=false;return count;
  }
  void Written(ClearRect area) {
    if(known_valid_ && !IntersectClearRect(known_.rect,area).empty())known_valid_=false;
  }
 private:
  static bool Equivalent(const ImageClear& a,const ImageClear& b) {return a.SameValueFor(b)&&b.SameValueFor(a);}
  static uint32_t Compact(Batch& batch) {
    uint32_t out=0,merged=0;
    for(uint32_t i=0;i<batch.count;++i) {
      auto clear=batch.items[i];
      while(out && Equivalent(batch.items[out-1],clear)) {
        ClearRect joined;if(!MergeClearRect(batch.items[out-1].rect,clear.rect,joined))break;
        clear.rect=joined;--out;++merged;
      }
      batch.items[out++]=clear;
    }
    batch.count=out;return merged;
  }
  Batch pending_;
  ImageClear known_{};
  bool known_valid_=false;
};
} // namespace fear_nx::deko
