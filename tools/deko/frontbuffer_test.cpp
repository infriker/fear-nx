// SPDX-License-Identifier: GPL-3.0-or-later
#include "deko/deko_frontbuffer.h"
#include "deko/deko_image_clear.h"
#include <array>
#include <cassert>
using namespace fear_nx::deko;
struct SimImage {
 ImageClearQueue pending_clear;
 std::array<int,16> pixels{};
};
int main(){
 FrontbufferResolve p{0x2000,0x2000,1280,736,1280,720,0,0,0,0,1280,720,true,false,false};
 assert(CanDeferFrontbuffer(p));
 auto invalid=p;invalid.last_frontbuffer=0x3000;assert(!CanDeferFrontbuffer(invalid));
 invalid=p;invalid.alias=true;assert(!CanDeferFrontbuffer(invalid));
 invalid=p;invalid.swapped_channels=true;assert(!CanDeferFrontbuffer(invalid));
 invalid=p;invalid.compatible_color=false;assert(!CanDeferFrontbuffer(invalid));
 invalid=p;invalid.height=704;assert(!CanDeferFrontbuffer(invalid));
 invalid=p;invalid.x=8;assert(!CanDeferFrontbuffer(invalid));
 invalid=p;invalid.dx=8;assert(!CanDeferFrontbuffer(invalid));
 invalid=p;invalid.source_height=719;assert(!CanDeferFrontbuffer(invalid));
 SimImage image;image.pixels.fill(7);
 ImageClear c{};c.rect={0,0,4,4};c.color={0,0,0,0};
 uint32_t replaced=0;image.pending_clear.Queue(c,replaced,true);
 std::array<int,16> snapshot{};
 ReadBeforeQueuedClears(image,[&]{
   assert(image.pending_clear.empty());snapshot=image.pixels;
 });
 assert(!image.pending_clear.empty());
 const auto clear=image.pending_clear.TakeAll(true);
 assert(clear.count==1);image.pixels.fill(0);
 for(int pixel:snapshot)assert(pixel==7);
 for(int pixel:image.pixels)assert(pixel==0);
 // A second snapshot preserves the next generation and its later pending clear.
 image.pixels.fill(9);image.pending_clear.Written({0,0,4,4});image.pending_clear.Queue(c,replaced,true);
 ReadBeforeQueuedClears(image,[&]{snapshot=image.pixels;});
 for(int pixel:snapshot)assert(pixel==9);
 assert(!image.pending_clear.empty());
  try {ReadBeforeQueuedClears(image,[]{throw 1;});}catch(int){}
  assert(!image.pending_clear.empty());
 // Visible sampling of a padded source retains pixel centers, not scaled height.
 for(int y=0;y<720;++y){
   float uv=(float(y)+.5f)/720.f*(720.f/736.f);
   assert(int(uv*736.f)==y);
 }
}
