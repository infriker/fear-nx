// SPDX-License-Identifier: GPL-3.0-or-later
#include "deko/deko_draw_dependencies.h"
#include "deko/deko_draw_upload_cache.h"
#include "deko/deko_draw_texture_cache.h"
#include "deko/deko_image_barriers.h"
#include <iostream>
#include <random>
#include <stdexcept>
using namespace fear_nx::deko;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::to_string(__LINE__)+": " #x);}while(false)

void LocalCache() {
  LocalVertexLookup<uint64_t,8> cache;std::map<decltype(cache)::Key,uint64_t> oracle;
  std::mt19937 rng(2005);
  for(uint32_t i=0;i<100000;++i) {
    const decltype(cache)::Key key{rng()%64,rng()%8,rng()%64};
    if(i%419==0){oracle.clear();cache.Clear();}
    if(rng()%3==0){cache.Store(key,i);oracle.insert_or_assign(key,i);}
    else {
      const auto* hit=cache.Find(key);auto it=oracle.find(key);
      CHECK(bool(hit)==(it!=oracle.end()));if(hit)CHECK(*hit==it->second);
    }
  }
  decltype(cache)::Key key{1,2,3};cache.Store(key,7);CHECK(*cache.Find(key)==7);
  CHECK(cache.fast_hits);cache.Store(key,9);CHECK(*cache.Find(key)==9);
  cache.Clear();CHECK(!cache.Find(key));
}

void Constants() {
  std::mt19937 rng(42);
  for(unsigned trial=0;trial<80;++trial) {
    const size_t size=33+rng()%4096;
    std::vector<uint8_t> source(size);
    std::vector<uint64_t> versions((size+15)/16,0);
    std::array<ConstantPackingPlan,3> exact,versioned;
    for(unsigned layout=0;layout<3;++layout) {
      std::vector<ConstantCopyRange> ranges;
      for(unsigned i=0;i<12;++i) {
        size_t bytes=rng()%32;
        ranges.push_back({rng()%(size-bytes),rng()%(256-bytes),bytes});
      }
      CHECK(exact[layout].Configure(size,256,ranges));CHECK(versioned[layout].Configure(size,256,ranges));
    }
    for(unsigned step=0;step<800;++step) {
      const size_t offset=rng()%size;
      if(step%7)source[offset]^=uint8_t(1+rng()%255);
      ++versions[offset/16]; // includes writes that leave the bytes unchanged
      unsigned layout=rng()%3; // each shader keeps its own dirty observations
      CHECK(exact[layout].Update(source)==versioned[layout].UpdateVersioned(source,versions));
      CHECK(exact[layout].revision()==versioned[layout].revision());
      CHECK(std::equal(exact[layout].bytes().begin(),exact[layout].bytes().end(),versioned[layout].bytes().begin()));
      if(step%67==0) { // mixed callers must invalidate the version shortcut
        source[0]^=1;++versions[0];
        CHECK(exact[layout].Update(source)==versioned[layout].Update(source));
      }
    }
  }
  CpuRegisterVersions tracker;std::array<uint8_t,33> source{};
  tracker.Update(source);auto initial=std::vector<uint64_t>(tracker.versions().begin(),tracker.versions().end());
  tracker.Update(source);CHECK(std::equal(initial.begin(),initial.end(),tracker.versions().begin()));
  source[32]=0xFF;tracker.Update(source);CHECK(tracker.versions()[2]!=initial[2] && tracker.versions()[0]==initial[0]);
  ConstantPackingPlan used;std::array<ConstantCopyRange,1> range{{{16,0,16}}};
  CHECK(used.Configure(source.size(),16,range));CHECK(used.UpdateVersioned(source,tracker.versions()));
  source[0]++;tracker.Update(source);CHECK(!used.UpdateVersioned(source,tracker.versions()));
  CHECK(used.version_checks()==1 && used.version_skips()==1);
  source[17]++;tracker.Update(source);CHECK(used.UpdateVersioned(source,tracker.versions()) && used.bytes()[1]==1);
}

void Textures() {
  ImageDescriptorVersions ids;const auto a=ids.NewImage(),b=ids.NewImage();
  ids.Bind(7,a,3);ids.Bind(8,b,4);
  struct Binding {uint32_t slot;uint64_t identity,version;};
  PreviousDrawTextures<Binding> memo;decltype(memo)::Shaders shaders{1,2,3,4};
  decltype(memo)::Stamp stamp{1,1,5};std::array<uint32_t,192> fetch{};
  memo.Store(shaders,stamp,stamp,fetch.data(),1,{7,a,3},true);
  auto current=[&]{auto* p=memo.Find(shaders,stamp,fetch.data());return p && ids.Alive(p->slot,p->identity,p->version);};
  CHECK(current());ids.Bind(8,b,5);CHECK(current()); // unrelated content write
  ids.Retire(8);CHECK(current()); // unrelated retirement
  ids.Bind(7,a,4);CHECK(!current()); // queued clear or write to our image
  ids.Retire(7);ids.Bind(7,ids.NewImage(),3);CHECK(!current()); // recycled descriptor
  CHECK(!ids.Alive(40000,a,3));
}

void Barriers() {
  using A=ImageAccessTracker::Action;using I=ImageAccess;
  // A fragment drain must not forget a vertex reader of an unrelated image.
  ImageAccessTracker tracker;
  std::array<I,2> first{{{1,I::Vertex},{2,I::Write}}};tracker.Record(first);
  const std::array<I,1> sample{{{2,I::Fragment}}},write{{{1,I::Write}}},other{{{3,I::Write}}};
  CHECK(tracker.Before(other)==A::None);CHECK(tracker.Before(sample)==A::Fragments);
  tracker.Complete(A::Fragments);CHECK(tracker.Before(write)==A::Primitives);
  tracker.Complete(A::Primitives);CHECK(tracker.Before(write)==A::None);
  // Differential model holds unresolved accesses, rather than reproducing
  // the implementation's timestamp/table algorithm.
  std::mt19937 rng(132);
  std::array<uint32_t,97> pending{};
  for(unsigned step=0;step<100000;++step) {
    if(step%131==0){tracker.Complete(A::Full);pending={};}
    std::array<I,3> op{};
    A needed=A::None;
    for(auto& access:op) {
      access={1+rng()%96,uint32_t(1u<<(rng()%3))};
      const auto old=pending[access.image];
      const bool raw=(old&I::Write) && (access.flags&(I::Fragment|I::Vertex));
      const bool war=(access.flags&I::Write) && (old&(I::Fragment|I::Vertex));
      if(raw || war) {
        const auto n=(raw && (access.flags&I::Vertex)) || (war && (old&I::Vertex))?A::Primitives:A::Fragments;
        needed=std::max(needed,n);
      }
    }
    const auto actual=tracker.Before(op);CHECK(actual==needed);
    tracker.Complete(actual);
    for(auto& p:pending) {
      if(actual==A::Primitives)p=0;
      else if(actual==A::Fragments)p&=I::Vertex;
    }
    tracker.Record(op);for(auto a:op)pending[a.image]|=a.flags;
  }
  tracker.Complete(A::Full);
  for(uint64_t id=1;id<10000;++id) {const I a{id,I::Write};tracker.Record({&a,1});}
  CHECK(tracker.overflow());CHECK(tracker.Before(other)==A::Full);
  tracker.Complete(A::Full);CHECK(tracker.Before(other)==A::None);
  ImageBarrierPolicy policy;
  CHECK(policy.BeforeImages(sample,true)==A::Full);policy.Images(sample);
  CHECK(policy.BeforeImages(other,true)==A::None);
  CHECK(policy.BeforeTransfer(true)==A::Full);policy.Transfer();
  CHECK(policy.BeforeImages(sample,true)==A::Full);
  policy.Graphics();CHECK(policy.BeforeImages(sample,true)==A::Full); // untracked graphics
  CHECK(policy.BeforeImages(sample,false)==A::Full);
  policy.BeginWork();CHECK(policy.BeforeImages(other,true)==A::Full);
}

int main() {
  LocalCache();Constants();Textures();Barriers();
  std::cout<<"Draw dependency regressions passed: collisions/retirement, sparse alternating layouts, descriptor ABA, image RAW/WAR and overflow.\n";
}
