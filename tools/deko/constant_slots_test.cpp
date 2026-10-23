// SPDX-License-Identifier: GPL-3.0-or-later
#include "deko/deko_constant_slots.h"
#include "deko/deko_draw_upload_cache.h"
#include <algorithm>
#include <deque>
#include <iostream>
#include <stdexcept>
using namespace fear_nx::deko;
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::to_string(__LINE__)+": " #x);}while(false)
using Arena=ConstantSlotArena<4>;
void allocator_edges(){
  Arena arena;
  CHECK(!arena.BeginWork(0,0,3,512,true));
  CHECK(arena.BeginWork(0,0,256,512,true));
  CHECK(!arena.Allocate(0) && !arena.Allocate(257));
  const auto a=arena.Allocate(256),b=arena.Allocate(256);
  CHECK(a && b && b.offset==a.offset+256 && !arena.Allocate(256));
  CHECK(arena.Matches(a,256));CHECK(!arena.BeginWork(4,0,256,512,true));
  CHECK(arena.BeginWork(4,1,256,512,true) && arena.Matches(a,256));
  CHECK(arena.BeginWork(1,0,256,512,true) && !arena.Matches(a,256));
  CHECK(arena.BeginWork(8,5,256,512,false) && !arena.Matches(a,256));
  CHECK(arena.BeginWork(12,9,256,512,true) && !arena.Matches(a,256));
  auto fresh=arena.Allocate(256);CHECK(fresh.offset==a.offset && fresh.epoch!=a.epoch);
  CHECK(arena.BeginWork(16,13,512,512,true) && !arena.Matches(fresh,256));
  CHECK(arena.Allocate(256).offset==512);
}

void delayed_gpu(){
  // CPU writes happen immediately. PushConstants and draws execute later in
  // queue order, up to four works behind. A reused slot waits for its old work;
  // all other slots can still have live draws. No buffer readback is used.
  Arena arena;
  struct Resident {Arena::Lease lease;ConstantDeltaShadow shadow;};
  std::array<std::array<Resident,4>,3> states{};
  constexpr uint32_t stride=8192,prefix=256,capacity=2048;
  std::array<uint8_t,4*stride> gpu{};gpu.fill(0xCD);
  struct Command {bool write;uint32_t address;std::vector<uint8_t> bytes;};
  struct Work {uint64_t id;std::vector<Command> commands;};
  std::deque<Work> pending;
  auto complete=[&](uint64_t before){while(!pending.empty()&&pending.front().id<before){
    for(const auto& c:pending.front().commands){
      if(c.write)std::copy(c.bytes.begin(),c.bytes.end(),gpu.begin()+c.address);
      else CHECK(std::equal(c.bytes.begin(),c.bytes.end(),gpu.begin()+c.address));
    }pending.pop_front();
  }};
  uint64_t initial=0,reused=0,unchanged=0,pushed=0,expected_initial=0;
  for(uint64_t work=0;work<600;++work){
    const uint64_t done=work>=4?work-3:0;complete(done);
    const uint32_t slot=work%4;
    const bool retry=work%53==52,disable=work>=250&&work<260;
    const uint32_t base=work>=400?512:prefix;
    CHECK(arena.BeginWork(work,done,base,capacity,!retry&&!disable));
    Work commands{work,{}};
    if(retry||disable){
      // Simulate a full transient overwrite. Re-enable must invalidate shadows
      // even when the same GPU addresses get allocated again.
      std::fill_n(gpu.begin()+slot*stride,stride,0xE7);
      commands.commands.push_back({false,slot*stride,std::vector<uint8_t>(stride,0xE7)});
    }else for(unsigned draw=0;draw<12;++draw){
      const uint32_t layout=draw%3,bytes=17+layout*289,allocated=(bytes+255)&~255u;
      auto& state=states[layout][slot];
      if(!arena.Matches(state.lease,allocated))state.lease=arena.Allocate(allocated);
      CHECK(state.lease);
      const uint32_t address=slot*stride+state.lease.offset;
      std::vector<uint8_t> source(bytes,uint8_t(0x10+layout));
      // Stable bank, sparse animated bank, and changing system layout.
      if(layout==1)source[16]=uint8_t(work/9);
      if(layout==2)source[draw*3]=uint8_t(work+draw);
      if(!state.shadow.Ready(state.lease.epoch,bytes)){
        CHECK(state.shadow.Initialize(state.lease.epoch,source));
        std::copy(state.shadow.bytes().begin(),state.shadow.bytes().end(),gpu.begin()+address);initial+=allocated;
      }else{
        const auto changed=state.shadow.Update(source,[&](uint32_t offset,std::span<const uint8_t> data){
          commands.commands.push_back({true,address+offset,{data.begin(),data.end()}});
        });pushed+=changed.bytes;++reused;unchanged+=changed.commands==0;
      }
      std::vector<uint8_t> expected(allocated,0);std::copy(source.begin(),source.end(),expected.begin());
      commands.commands.push_back({false,address,std::move(expected)});
      if(draw<3)expected_initial+=allocated; // baseline: one initial copy/layout/work
    }
    pending.push_back(std::move(commands));
  }
  complete(UINT64_MAX);CHECK(pending.empty());
  CHECK(reused>1000 && unchanged>1000 && pushed>0 && initial<expected_initial/5);
  std::cout<<"Delayed GPU reads preserved across 600 works, retries and region changes. Initial CPU bytes "<<expected_initial<<" -> "<<initial<<", ordered push bytes "<<pushed<<".\n";
}
int main(){try{allocator_edges();delayed_gpu();}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
