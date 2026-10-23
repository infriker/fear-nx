// SPDX-License-Identifier: GPL-3.0-or-later
#include "deko/deko_draw_upload_cache.h"
#include "deko/deko_binding_cache.h"
#include "deko/deko_queue_work.h"
#include <iostream>
#include <random>
#include <cstdlib>
using namespace fear_nx::deko;
#define CHECK(x) do {if(!(x)){std::cerr<<__LINE__<<": " #x "\n";std::abort();}}while(false)
int main() {
  std::mt19937 rng(61);
  uint64_t narrow=0,full=0,updates=0;
  // Sparse, non-contiguous, overlapping, out-of-order and partial-register
  // reflection. Reference always replays the original complete copy sequence.
  const std::vector<std::vector<ConstantCopyRange>> layouts={
    {{0,0,16},{64,16,32},{160,64,33}},
    {{0,0,64},{48,24,40},{144,88,9}},
    {{192,0,16},{0,32,16},{96,64,33}},
    {{0,0,97}},{{128,0,0}}};
  for(const auto& ranges:layouts) {
    ConstantPackingPlan plan;CHECK(plan.Configure(257,97,ranges));
    std::vector<uint8_t> source(257),reference(97);
    std::array<uint64_t,17> versions{};std::array<uint64_t,3> groups{};
    std::array<ConstantDeltaShadow,4> shadows;
    std::array<std::vector<uint8_t>,4> gpu;
    std::array<uint64_t,4> revision{};
    for(unsigned i=0;i<40000;++i) {
      const unsigned changes=rng()%10;
      for(unsigned n=0;n<changes;++n) {
        const size_t at=rng()%source.size();source[at]=uint8_t(rng());
        ++versions[at/16];++groups[at/128];
      }
      plan.UpdateVersioned(source,versions,groups);
      for(const auto& r:ranges)std::memcpy(reference.data()+r.destination_offset,source.data()+r.source_offset,r.size);
      CHECK(std::equal(reference.begin(),reference.end(),plan.bytes().begin()));
      // Sometimes a layout changes more than once before any GPU consumer.
      if(i%11==0)continue;
      const size_t slot=(i%7)?0:rng()%4;
      auto& shadow=shadows[slot];auto& memory=gpu[slot];
      if(!shadow.Ready(1,97)) {
        CHECK(shadow.Initialize(1,plan.bytes()));memory.assign(shadow.bytes().begin(),shadow.bytes().end());
      } else {
        const auto range=plan.dirty_since(revision[slot]);
        if(range.end-range.begin<97)++narrow;else ++full;
        // Every actual change must be inside the declared envelope.
        for(size_t at=0;at<97;++at)if(memory[at]!=reference[at])CHECK(at>=range.begin && at<range.end);
        shadow.UpdateRange(plan.bytes(),range,[&](uint32_t at,std::span<const uint8_t> bytes){
          CHECK(at%16==0 && bytes.size()%16==0 && at+bytes.size()<=memory.size());
          std::copy(bytes.begin(),bytes.end(),memory.begin()+at);
        });
      }
      revision[slot]=plan.revision();++updates;
      CHECK(std::equal(reference.begin(),reference.end(),memory.begin()));
      for(size_t at=97;at<memory.size();++at)CHECK(memory[at]==0);
    }
  }
  CHECK(narrow>10000 && full>1000);
  BindingMemo<int,2,1,4> memo;
  for(uint32_t i=0;i<4;++i)memo.Store(1,{i,17},{1,0,0},int(i));
  for(uint32_t i=0;i<4;++i){auto* value=memo.Find(1,{i,17},{1,0,0});CHECK(value && *value==int(i));}
  CHECK(!memo.Find(1,{3,18},{1,0,0}));CHECK(!memo.Find(1,{3,17},{2,0,0}));
  memo.Store(1,{4,17},{1,0,0},4);CHECK(memo.Find(1,{3,17},{1,0,0}));
  memo.Store(1,{3,17},{1,0,0},33);CHECK(*memo.Find(1,{3,17},{1,0,0})==33);
  QueueWorkState queue;CHECK(!queue.pending());queue.Begin();
  for(unsigned i=0;i<3;++i){CHECK(queue.CanSendPartial() && queue.pending());queue.SentPartial();CHECK(!queue.pending());queue.Changed();}
  CHECK(!queue.CanSendPartial());queue.Closed();CHECK(!queue.pending());queue.Begin();CHECK(queue.CanSendPartial());
  std::cout<<"PASS: "<<updates<<" GPU shadow updates, "<<narrow<<" bounded / "<<full<<" full scans; collisions and partial-send policy\n";
}
