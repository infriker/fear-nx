// SPDX-License-Identifier: GPL-3.0-or-later
#include "deko/deko_image_barriers.h"
#include "deko/deko_image_clear.h"
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>
using namespace fear_nx::deko;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::to_string(__LINE__)+": " #x);}while(false)

struct Pixel {float color=0,z=0;uint8_t stencil=0;bool operator==(const Pixel&)const=default;};
using Grid=std::array<Pixel,16*16>;
template<class F> void pixels(ClearRect r,F f){for(uint32_t y=r.y;y<r.y+r.h;++y)for(uint32_t x=r.x;x<r.x+r.w;++x)f(y*16+x);}
void apply(Grid& grid,const ImageClear& c){pixels(c.rect,[&](auto i){
  if(!c.depth)grid[i].color=c.color[0];
  else {if(c.clear_z)grid[i].z=c.z;grid[i].stencil=(grid[i].stencil&~c.mask)|(c.stencil&c.mask);}
});}
void emit(Grid& gpu,const ImageClearQueue::Batch& batch){for(uint32_t i=0;i<batch.count;++i)apply(gpu,batch.items[i]);}

void differential(bool depth,bool coalesce=false) {
  Grid eager{},gpu{};ImageClearQueue queue;
  std::mt19937 rng(depth?0xD24F2005:0xFEA2005);
  uint64_t drains=0,repeats=0,partial=0;
  auto rect=[&]{const uint32_t x=rng()%16,y=rng()%16;return ClearRect{x,y,uint32_t(1+rng()%(16-x)),uint32_t(1+rng()%(16-y))};};
  auto read=[&](ClearRect r){const auto b=queue.Consume(r,coalesce);drains+=b.drained;emit(gpu,b);partial+=!queue.empty();pixels(r,[&](auto i){CHECK(gpu[i]==eager[i]);});};
  for(unsigned i=0;i<100000;++i){
    const auto r=rect();
    switch(rng()%7){
      case 0:case 1:case 2:{
        ImageClear c{r,depth,bool(rng()%2),uint8_t(rng()),uint8_t(rng()),float(rng()%1000),{float(rng()%1000)}};
        if(!depth)c.clear_z=false;
        if(i%7==0)c.rect={0,0,16,16};
        uint32_t replaced=0;
        auto result=queue.Queue(c,replaced,coalesce);
        if(result==ImageClearQueue::Result::Full){emit(gpu,queue.TakeAll(coalesce));++drains;result=queue.Queue(c,replaced,coalesce);}
        CHECK(result!=ImageClearQueue::Result::Full);apply(eager,c);
        if(i%3==0){CHECK(queue.Queue(c,replaced,coalesce)==ImageClearQueue::Result::Repeated);++repeats;}
        break;
      }
      case 3:read(r);break;
      case 4:{ // read-modify-write, as blend/depth/stencil draw operations do
        read(r);pixels(r,[&](auto p){gpu[p].color+=1;gpu[p].z+=2;gpu[p].stencil^=0x77;eager[p]=gpu[p];});queue.Written(r);break;
      }
      case 5:{ // copy overwrite must not discard pending depth/stencil aspects
        if(!depth)queue.DiscardForFullColorOverwrite(true);
        else {CHECK(!queue.DiscardForFullColorOverwrite(true));emit(gpu,queue.TakeAll(coalesce));}
        for(auto& p:gpu){p.color=float(rng()%1000);}eager=gpu;queue.Written({0,0,16,16});break;
      }
      case 6:{auto moved=std::move(queue);queue=std::move(moved);break;} // work/frame/image ownership
    }
    CHECK(queue.size()<=ImageClearQueue::kLimit);
  }
  read({0,0,16,16});CHECK(gpu==eager);CHECK(queue.empty());CHECK(drains>0 && repeats>0 && partial>0);
  std::cout<<(depth?"depth/stencil":"color")<<" coalesce="<<coalesce<<": 100000 eager-vs-deferred events, drains "<<drains<<", partial consumes "<<partial<<"\n";
}

void coalescing(){
  ImageClearQueue queue;uint32_t replaced,merged;
  ImageClear c{{0,0,16,4},false,false,0,0,0,{.5f}};
  CHECK(queue.Queue(c,replaced,true,&merged)==ImageClearQueue::Result::Queued && merged==0);
  c.rect.y=4;CHECK(queue.Queue(c,replaced,true,&merged)==ImageClearQueue::Result::Queued && merged==1 && queue.size()==1);
  auto all=queue.TakeAll(true);CHECK(all.count==1 && all.items[0].rect.h==8);
  c.rect={0,0,16,16};queue.Written(c.rect);queue.Queue(c,replaced,true);
  auto almost=queue.Consume({0,0,16,13},true);
  CHECK(almost.count==1 && almost.items[0].rect==c.rect && almost.expanded && almost.early_pixels==48 && queue.empty());
  queue.Written(c.rect);queue.Queue(c,replaced,true);
  auto hole=queue.Consume({3,4,6,5},true);CHECK(hole.count==1 && !hole.expanded && queue.size()==4);
  queue.Written({3,4,6,5});
  auto rest=queue.TakeAll(true);for(uint32_t i=0;i<rest.count;++i)CHECK(IntersectClearRect(rest.items[i].rect,{3,4,6,5}).empty());
  // Equal values separated by a different overlapping operation may not be
  // combined or reordered; different masks may not expand into each other.
  Grid eager{},gpu{};ImageClearQueue ordered;
  for(const auto value:{1.f,2.f,1.f}){c.rect={0,0,16,16};c.color[0]=value;ordered.Queue(c,replaced,true);apply(eager,c);}
  emit(gpu,ordered.TakeAll(true));CHECK(gpu==eager);
  ordered.Written({0,0,16,16});
  for(unsigned i=0;i<3;++i) {
    c.rect={0,i*4,8,8};c.color[0]=i==1?2.f:1.f;
    ordered.Queue(c,replaced,true);apply(eager,c);
  }
  CHECK(ordered.size()==3); // first+third could form a rectangle, but crossing the middle clear is unsafe
  emit(gpu,ordered.TakeAll(true));CHECK(gpu==eager);
  ImageClear depth{{0,0,16,16},true,true,255,0xA5,.75f,{}};
  ImageClear stencil{{0,0,16,4},true,false,15,3,0,{}};
  ordered.Queue(depth,replaced,true);apply(eager,depth);
  ordered.Queue(stencil,replaced,true);apply(eager,stencil);
  emit(gpu,ordered.Consume({0,0,16,13},true));CHECK(gpu==eager); // expansion consumes older and newer writes in order
  std::cout<<"Clear merging preserves holes, aspect masks and overlap ordering; 13/16 rows need one command instead of two.\n";
}

void clear_edges(){
  CHECK((ClipClearRect(8,8,-3,-2,5,4)==ClearRect{0,0,2,2}));
  CHECK((ClipClearRect(8,8,1,2,UINT32_MAX,UINT32_MAX)==ClearRect{1,2,7,6}));
  CHECK(ClipClearRect(8,8,100,100,1,1).empty());
  ImageClearQueue queue;uint32_t n;
  ImageClear c{{0,0,16,16},true,true,255,0xA5,.75f,{}};
  CHECK(queue.Queue(c,n)==ImageClearQueue::Result::Queued);
  auto stencil=c;stencil.clear_z=false;stencil.mask=15;stencil.stencil=3;
  CHECK(queue.Queue(stencil,n)==ImageClearQueue::Result::Queued && n==0); // preserve depth and upper stencil
  CHECK(queue.size()==2);
  auto both=c;both.z=.25f;
  CHECK(queue.Queue(both,n)==ImageClearQueue::Result::Queued && n==2);
  auto band=queue.Consume({0,0,16,5});CHECK(band.count==1 && band.items[0].rect.h==5 && queue.size()==1);
  queue.Written({0,0,16,5});
  auto remaining=queue.Consume({0,0,16,16});CHECK(remaining.count==1 && remaining.items[0].rect.y==5 && remaining.items[0].rect.h==11);
  CHECK(queue.Queue(both,n)==ImageClearQueue::Result::Queued); // a write prevents false repeated-clear hits
  auto all=queue.TakeAll();CHECK(all.count==1);CHECK(queue.Queue(both,n)==ImageClearQueue::Result::Repeated);
  queue.Written({16,0,1,1});CHECK(queue.Queue(both,n)==ImageClearQueue::Result::Repeated); // disjoint write
  queue.Written({15,15,1,1});CHECK(queue.Queue(both,n)==ImageClearQueue::Result::Queued);
  // An interior consumer produces four disjoint remainders; their total area
  // plus the emitted area must exactly cover the original clear.
  auto hole=queue.Consume({3,4,6,5});CHECK(hole.count==1 && queue.size()==4);
  uint64_t area=hole.items[0].rect.pixels();const auto rest=queue.TakeAll();for(uint32_t i=0;i<rest.count;++i)area+=rest.items[i].rect.pixels();CHECK(area==256);
}

void barrier_sequences(){
  using A=ImageBarrierPolicy::Action;
  // Exhaustive sequences of draws, sampling copies, clears, transfer jobs,
  // vertex texture reads, compute drains and queue boundaries. The oracle
  // retains unresolved producer/reader hazards until an emitted barrier.
  for(uint32_t sequence=0;sequence<823543;++sequence){ // 7^7
    ImageBarrierPolicy policy;bool unknown=true,graphics=false,copy=false,transfer=false,vertex=false;
    uint32_t code=sequence;
    for(unsigned step=0;step<7;++step){
      const auto op=code%7;code/=7;
      if(op==5){policy.FullBarrier();unknown=graphics=copy=transfer=vertex=false;continue;}
      if(op==6){policy.BeginWork();unknown=true;continue;}
      const bool is_transfer=op==3,samples=op==1,helper=op==1||op==2;
      const A a=is_transfer?policy.BeforeTransfer(true):policy.BeforeGraphics(samples,helper,true);
      if(is_transfer){if(unknown||graphics)CHECK(a==A::Full);}
      else if(unknown||transfer)CHECK(a==A::Full);
      else if(copy||(samples&&graphics)||(helper&&vertex))CHECK(a==A::Full||a==A::Primitives||(!vertex&&a==A::Fragments));
      if(a==A::Full){unknown=graphics=copy=transfer=vertex=false;}
      else if(a==A::Fragments||a==A::Primitives){CHECK(!transfer&&!unknown);graphics=copy=vertex=false;}
      if(is_transfer){policy.Transfer();transfer=true;}
      else{policy.Graphics(samples,op==4);graphics=true;copy|=samples;vertex|=op==4;}
    }
  }
  ImageBarrierPolicy p;CHECK(p.BeforeGraphics(false,false,true)==A::Full);
  p.Graphics();CHECK(p.BeforeGraphics(false,false,true)==A::None);
  CHECK(p.BeforeGraphics(false,true,true)==A::None); // ROP draw -> clear
  CHECK(p.BeforeGraphics(true,true,true)==A::Fragments); // ROP -> sample
  p.Graphics(true);CHECK(p.BeforeGraphics(false,false,true)==A::Fragments); // copy -> consumer
  CHECK(p.BeforeGraphics(false,true,true)==A::None); // adjacent helper coalescing
  CHECK(p.BeforeGraphics(false,false,false)==A::Full);CHECK(p.BeforeTransfer(false)==A::Full);
  std::cout<<"823543 barrier sequences checked, including vertex readers and work rotation\n";
}
int main(){try{clear_edges();coalescing();differential(false);differential(true);differential(false,true);differential(true,true);barrier_sequences();}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
