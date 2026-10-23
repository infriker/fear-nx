// SPDX-License-Identifier: GPL-3.0-or-later
#include "deko/deko_shared_geometry.h"
#include "deko/deko_draw_upload_cache.h"
#include "deko/deko_binding_cache.h"
#include <iostream>
#include <random>
#include <sys/mman.h>
#include <unistd.h>
using namespace fear_nx::deko;
#define CHECK(x) do {if(!(x)){std::cerr<<__func__<<':'<<__LINE__<<": " #x "\n";return false;}}while(false)

bool ExactCompareBounds() {
  const size_t page=sysconf(_SC_PAGESIZE);
  auto* a=static_cast<uint8_t*>(mmap(nullptr,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
  auto* b=static_cast<uint8_t*>(mmap(nullptr,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
  CHECK(a!=MAP_FAILED&&b!=MAP_FAILED);
  CHECK(!mprotect(a+page,page,PROT_NONE)&&!mprotect(b+page,page,PROT_NONE));
  for(size_t n=0;n<=257;++n) {
    auto* p=a+page-n;auto* q=b+page-n;
    for(size_t i=0;i<n;++i)p[i]=q[i]=uint8_t(i*139);
    CHECK(EqualCpuBytes(p,q,n));
    for(size_t i=0;i<n;++i){q[i]^=1;CHECK(!EqualCpuBytes(p,q,n));q[i]^=1;}
  }
  munmap(a,page*2);munmap(b,page*2);return true;
}

bool SparsePackingMatchesReference() {
  std::mt19937 random(42);
  for(unsigned trial=0;trial<150;++trial) {
    std::vector<uint8_t> source(4096),reference(768,0);
    for(auto& v:source)v=uint8_t(random());
    std::vector<ConstantCopyRange> ranges;
    for(unsigned i=0;i<20;++i){size_t size=random()%64;ranges.push_back({random()%(4096-size),random()%(768-size),size});}
    ConstantPackingPlan plan;CHECK(plan.Configure(source.size(),reference.size(),ranges));
    bool first=true;
    for(unsigned step=0;step<150;++step) {
      size_t at=random()%source.size();source[at]^=1;
      bool used=std::any_of(ranges.begin(),ranges.end(),[&](auto r){return at>=r.source_offset&&at<r.source_offset+r.size;});
      auto rev=plan.revision();bool updated=plan.Update(source);
      CHECK(updated==(first||used));CHECK(plan.revision()==rev+unsigned(updated));
      for(auto r:ranges)std::copy_n(source.begin()+r.source_offset,r.size,reference.begin()+r.destination_offset);
      CHECK(std::equal(reference.begin(),reference.end(),plan.bytes().begin()));first=false;
    }
  }
  ConstantPackingPlan p;std::array<ConstantCopyRange,1> r{{{64,0,16}}};
  std::array<uint8_t,4096> bank{};CHECK(p.Configure(bank.size(),256,r));CHECK(p.source_bytes_used()==16);
  CHECK(p.Update(bank));auto rev=p.revision();bank[4000]=1;CHECK(!p.Update(bank)&&p.revision()==rev);
  bank[65]=0xFF;CHECK(p.Update(bank)&&p.bytes()[1]==0xFF);
  // A shader with no float constants still gets a zero block and a valid revision.
  std::array<ConstantCopyRange,1> empty{{{0,0,0}}};CHECK(p.Configure(bank.size(),16,empty));
  CHECK(p.Update(bank)&&p.source_bytes_used()==0);bank[0]++;CHECK(!p.Update(bank));
  return true;
}

bool FastGeometryMatchesMap() {
  auto fast=std::make_unique<SharedGeometryCache>(4,1u<<20,256,(1u<<20)-256);
  auto slow=std::make_unique<SharedGeometryCache>(4,1u<<20,256,(1u<<20)-256);
  slow->FastLookup(false);
  std::mt19937 random(123);
  for(uint64_t work=0;work<200;++work) {
    uint64_t completed=work>3?work-3:0;
    fast->BeginWork(work,completed);slow->BeginWork(work,completed);
    if(work%31==30) {
      fast->BeginWork(work,work);slow->BeginWork(work,work);
      CHECK(fast->BlockRegion(work%4)==slow->BlockRegion(work%4));
    }
    for(unsigned i=0;i<1000;++i) {
      const uint32_t address=random()%20000,bytes=64+(address%10)*64;
      const uint64_t hash=address+(work/20)*100000;
      const uint32_t minimum=i%67==0?1u<<20:0;
      for(int repeat=0;repeat<3;++repeat) {
        const auto a=fast->FindOrAllocate(address,bytes,hash,minimum),b=slow->FindOrAllocate(address,bytes,hash,minimum);
        CHECK(a.offset==b.offset&&a.bytes_to_copy==b.bytes_to_copy&&a.hit==b.hit&&a.valid==b.valid);
      }
    }
  }
  CHECK(fast->stats().hits==slow->stats().hits&&fast->stats().evictions==slow->stats().evictions);
  CHECK(fast->stats().capacity_misses==slow->stats().capacity_misses);
  CHECK(fast->cross_work_hits()==slow->cross_work_hits()&&fast->cross_slot_hits()==slow->cross_slot_hits());
  CHECK(fast->fast_hits()>0&&fast->table_lookups()<slow->table_lookups());
  std::cout<<"fast geometry: "<<fast->fast_hits()<<" exact hits; "<<fast->stats().evictions<<" evictions, map/fast results identical\n";
  return true;
}

bool TextureBucketCollisions() {
  BindingMemo<int,18,1> memo; // Force every key to collide.
  BindingMemo<int,18,1>::Key key{};BindingMemo<int,18,1>::Stamp stamp{1,2,3};
  const auto hash=BindingBucketHash(key);memo.Store(hash,key,stamp,17);
  for(size_t i=0;i<key.size();++i){auto different=key;different[i]=0xFFFFFFFF;CHECK(!memo.Find(BindingBucketHash(different),different,stamp));}
  for(size_t i=0;i<stamp.size();++i){auto stale=stamp;stale[i]++;CHECK(!memo.Find(hash,key,stale));}
  CHECK(memo.Find(hash,key,stamp)&&*memo.Find(hash,key,stamp)==17);return true;
}
int main(){if(!ExactCompareBounds()||!SparsePackingMatchesReference()||!FastGeometryMatchesMap()||!TextureBucketCollisions())return 1;std::cout<<"NFSMW audit: bounded comparisons, sparse/overlapping layouts, cache lifetime and collisions passed\n";}
