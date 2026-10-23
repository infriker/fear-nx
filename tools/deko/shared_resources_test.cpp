// SPDX-License-Identifier: GPL-3.0-or-later
#include "deko/deko_shared_geometry.h"
#include "deko/deko_shared_constants.h"
#include "deko/deko_draw_texture_cache.h"
#include "deko/deko_vertex_range.h"
#include <iostream>
#include <random>

namespace {
using namespace fear_nx::deko;
#define CHECK(x) do {if(!(x)){std::cerr<<__func__<<':'<<__LINE__<<": " #x "\n";return false;}}while(false)

bool WorkZeroAndCrossSlotReaders() {
  SharedGeometryCache c(4,1024,64,128);
  c.BeginWork(0,0);
  CHECK(!c.FindOrAllocate(10,128,1).valid);
  auto a=c.FindOrAllocate(10,128,1);CHECK(a.valid&&!a.hit&&a.offset==64);
  c.BeginWork(1,0);auto b=c.FindOrAllocate(10,128,1);
  CHECK(b.hit&&b.offset==a.offset&&c.cross_slot_hits()==1&&c.cross_work_hits()==1);
  // Fill the remaining regions, while work zero and one are still in flight.
  for(uint32_t i=2;i<=4;++i){c.FindOrAllocate(i,128,i);CHECK(c.FindOrAllocate(i,128,i).valid);}
  c.FindOrAllocate(99,128,99);CHECK(!c.FindOrAllocate(99,128,99).valid);
  CHECK(!c.BlockRegion(0));
  c.BeginWork(4,1);CHECK(!c.FindOrAllocate(99,128,99).valid); // work 1 pins all four
  c.BeginWork(5,2);CHECK(c.FindOrAllocate(99,128,99).valid);
  CHECK(c.stats().evictions==1);
  // Different contents never mutate the old entry, even at the same address.
  auto old=c.FindOrAllocate(3,128,3);CHECK(old.hit);
  c.FindOrAllocate(3,128,100);auto changed=c.FindOrAllocate(3,128,100);
  CHECK(changed.valid&&!changed.hit&&old.offset!=changed.offset);
  CHECK(c.FindOrAllocate(3,128,3).offset==old.offset);
  return true;
}

bool PlacementAndTransientRetry() {
  SharedGeometryCache c(4,1024,64,512);
  c.BeginWork(0,0);c.FindOrAllocate(1,65,1,1200);
  auto a=c.FindOrAllocate(1,65,1,1200);
  CHECK(a.valid&&a.offset>=1200&&a.offset%64==0&&a.offset/1024==1);
  CHECK(!c.FindOrAllocate(1,65,1,a.offset+64).valid);
  CHECK(!c.FindOrAllocate(2,513,2).valid);
  CHECK(!c.FindOrAllocate(2,128,2,UINT32_MAX).valid);
  c.BeginWork(1,1);CHECK(c.BlockRegion(1));CHECK(c.size()==0);
  // In-flight transient work owns region 1, despite having no resident entry.
  c.FindOrAllocate(3,512,3,1024);
  auto b=c.FindOrAllocate(3,512,3,1024);CHECK(b.valid&&b.offset/1024==2);
  c.BeginWork(2,1);CHECK(!c.FindOrAllocate(1,65,1,1200).hit);
  c.FindOrAllocate(5,512,5,1024);
  auto remaining=c.FindOrAllocate(5,512,5,1024);CHECK(remaining.valid&&remaining.offset/1024==3);
  CHECK(!c.BlockRegion(1)); // transient work 1 has not completed yet
  c.BeginWork(3,2);
  c.FindOrAllocate(4,512,4,1024);
  auto d=c.FindOrAllocate(4,512,4,1024);CHECK(d.valid&&d.offset/1024==1);
  return true;
}

bool RandomizedGpuLifetime(bool sequential) {
  // Model delayed GPU reads independently of the allocator. Any early reuse
  // or coalescing across unreserved ring space corrupts this reference queue.
  SharedGeometryCache c(4,4096,256,1536);
  c.PreferSequentialAllocations(sequential);
  std::vector<uint8_t> heap(4*4096,0xDD);
  struct Read {uint64_t work;uint32_t offset,bytes;uint8_t value;};
  std::vector<Read> reads;
  std::mt19937 random(20261022);
  uint64_t boundary=0;
  for(uint64_t work=0;work<160;++work) {
    boundary=work>3?work-3:0;
    for(const auto& r:reads)for(uint32_t i=0;i<r.bytes;++i)CHECK(heap[r.offset+i]==r.value);
    std::erase_if(reads,[&](auto& r){return r.work<boundary;});
    c.BeginWork(work,boundary);
    if(work%17==16) {
      // A full-slot retry first drains everything. Its ordinary upload may
      // overwrite the entire prefix, protected through the new work's fence.
      reads.clear();boundary=work;c.BeginWork(work,boundary);
      CHECK(c.BlockRegion(work%4));
      uint32_t at=uint32_t(work%4)*4096+256;
      std::fill_n(heap.begin()+at,1536,uint8_t(0xE3));
      reads.push_back({work,at,1536,0xE3});
      continue;
    }
    for(unsigned draw=0;draw<80;++draw) {
      uint32_t key=random()%40,bytes=1+(key%5)*63;
      uint8_t content=uint8_t(1+key+(work/9)*41);
      auto result=c.FindOrAllocate(key,bytes,content);
      if(!result.valid)continue;
      CHECK(result.offset%64==0&&result.offset+bytes<=heap.size());
      CHECK(result.offset%4096>=256&&result.offset%4096+bytes<=1792);
      if(!result.hit)std::fill_n(heap.begin()+result.offset,bytes,content);
      for(const auto& r:reads)for(uint32_t i=0;i<r.bytes;++i)CHECK(heap[r.offset+i]==r.value);
      for(uint32_t i=0;i<bytes;++i)CHECK(heap[result.offset+i]==content);
      reads.push_back({work,result.offset,bytes,content});
    }
    for(uint32_t slot=0;slot<4;++slot) {
      for(uint32_t i=0;i<256;++i)CHECK(heap[slot*4096+i]==0xDD);
      for(uint32_t i=1792;i<4096;++i)CHECK(heap[slot*4096+i]==0xDD);
    }
  }
  CHECK(c.stats().hits>0&&c.stats().evictions>0&&c.stats().capacity_misses>0);
  CHECK(c.cross_work_hits()>0&&c.cross_slot_hits()>0);
  return true;
}

bool SequentialPlacementPreservesFallback() {
  for(bool sequential:{false,true}) {
    SharedGeometryCache c(2,1024,64,512);
    c.PreferSequentialAllocations(sequential);c.BeginWork(0,0);
    c.FindOrAllocate(1,64,1,256);
    auto a=c.FindOrAllocate(1,64,1,256);CHECK(a.valid&&a.offset==256);
    c.FindOrAllocate(2,64,2);
    auto b=c.FindOrAllocate(2,64,2);CHECK(b.valid&&b.offset==(sequential?320u:64u));
    CHECK(c.FindOrAllocate(1,64,1).hit);
    // The next request cannot fit the preferred extent: use another region,
    // never evict the live earlier payloads just to maintain linear placement.
    c.FindOrAllocate(3,512,3);
    auto d=c.FindOrAllocate(3,512,3);CHECK(d.valid&&d.offset==1088);
    CHECK(c.stats().evictions==0);
    CHECK(c.FindOrAllocate(2,64,2).offset==b.offset);
    if(sequential)CHECK(c.sequential_allocations()==1);
    else CHECK(c.sequential_allocations()==0);
    CHECK(!c.BlockRegion(0));
  }
  return true;
}

bool CompatibleLayoutsAndOrderedUpdates() {
  struct State {ConstantPackingPlan plan;GenerationUpload<uint32_t> upload;ConstantDeltaShadow shadow;};
  ConstantLayoutPool<State> pool;
  std::array<ConstantCopyRange,2> split{{{0,0,16},{16,16,16}}};
  std::array<ConstantCopyRange,1> joined{{{0,0,32}}};
  auto* a=pool.Intern(1,0,64,48,split);
  auto* b=pool.Intern(1,0,64,48,joined);CHECK(a&&a==b&&pool.size()==1);
  CHECK(pool.Intern(2,0,64,48,joined)!=a); // VS and PS banks differ
  CHECK(pool.Intern(1,0,64,64,joined)!=a); // zero padding is ABI
  CHECK(pool.Intern(1,4,64,48,joined)!=a); // diagnostics: disable sharing
  std::array<ConstantCopyRange,1> different{{{16,0,32}}};
  CHECK(pool.Intern(1,0,64,48,different)!=a);
  std::array<ConstantCopyRange,1> bad{{{SIZE_MAX,0,32}}};
  CHECK(!pool.Intern(1,0,64,48,bad));
  std::array<uint8_t,64> source{};for(size_t i=0;i<source.size();++i)source[i]=uint8_t(i+1);
  CHECK(a->plan.Update(source));CHECK(!b->plan.Update(source));
  CHECK(a->shadow.Initialize(0,a->plan.bytes()));
  a->upload.Store(a->plan.revision(),0,1234);
  CHECK(b->upload.Find(b->plan.revision(),0)&&!b->upload.Find(b->plan.revision(),1));
  std::vector<uint8_t> gpu(a->shadow.bytes().begin(),a->shadow.bytes().end());
  const auto first_draw=gpu;
  source[17]=201;CHECK(b->plan.Update(source));
  CHECK(!a->upload.Find(a->plan.revision(),0));
  const auto changes=b->shadow.Update(b->plan.bytes(),[&](uint32_t offset,std::span<const uint8_t> bytes){
    std::copy(bytes.begin(),bytes.end(),gpu.begin()+offset);
  });
  CHECK(changes.commands==1&&first_draw[17]==18&&gpu[17]==201);
  for(size_t i=32;i<gpu.size();++i)CHECK(gpu[i]==0);
  CHECK(!a->shadow.Ready(1,48)); // a new work always allocates fresh storage
  CHECK(pool.shared_layouts()==1);
  return true;
}

bool WholeTextureSetsAndInvalidations() {
  PreviousDrawTextures<uint32_t> cache;
  using C=decltype(cache);
  C::Shaders shaders{1,2,3,4};C::Stamp stamp{0,1,20,0,2,8,1,0};
  std::array<uint32_t,192> fetch{};
  cache.Store(shaders,stamp,stamp,fetch.data(),(1u<<2)|(1u<<31),123,true);
  CHECK(cache.Find(shaders,stamp,fetch.data()));
  fetch[0]=71;CHECK(cache.Find(shaders,stamp,fetch.data())); // unused vertex fetch
  for(uint32_t slot:{2u,31u})for(uint32_t word=0;word<6;++word){
    fetch[slot*6+word]++;CHECK(!cache.Find(shaders,stamp,fetch.data()));fetch[slot*6+word]--;
  }
  for(size_t i=0;i<shaders.size();++i){auto other=shaders;other[i]++;CHECK(!cache.Find(other,stamp,fetch.data()));}
  for(size_t i=0;i<stamp.size();++i){auto other=stamp;other[i]++;CHECK(!cache.Find(shaders,other,fetch.data()));}
  cache.Store(shaders,stamp,stamp,fetch.data(),0,321,false);
  CHECK(!cache.Find(shaders,stamp,fetch.data())); // strict/small/quarantined
  auto changed=stamp;changed[3]++;
  cache.Store(shaders,stamp,changed,fetch.data(),0,321,true);
  CHECK(!cache.Find(shaders,changed,fetch.data())); // guard discovered mid-build
  cache.Store(shaders,stamp,stamp,fetch.data(),0,321,true);
  cache.Invalidate();CHECK(!cache.Find(shaders,stamp,fetch.data())); // copy/clear/resolve
  return true;
}
}
int main(){
  if(!WorkZeroAndCrossSlotReaders()||!PlacementAndTransientRetry()||!RandomizedGpuLifetime(false)||!RandomizedGpuLifetime(true)||!SequentialPlacementPreservesFallback()||
     !CompatibleLayoutsAndOrderedUpdates()||!WholeTextureSetsAndInvalidations())return 1;
  std::cout<<"shared resources: GPU lifetime stress, transient retry, layouts, ordered updates and texture invalidations passed\n";
}
