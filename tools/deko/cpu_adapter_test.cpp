// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>
struct PPCContext {uint32_t msr=0x8000;};
namespace rex::thread {
struct global_critical_region {
 static std::recursive_mutex& mutex(){static std::recursive_mutex m;return m;}
 static auto AcquireDirect(){return std::unique_lock(mutex());}
};
}
inline std::atomic<int>& ppc_global_lock_count_(){static std::atomic<int> n{0};return n;}
#include "cpu/fear_cpu_guest.h"
namespace fear_nx::cpu {
bool local_interrupts=true,fixed_flush=true,yield_backoff=true;
thread_local Counters counters;
void Publish(){counters={};}
}
struct FP {
 static constexpr uint32_t FlushMask=1u<<24;
 uint32_t csr=0x400123;int writes=0;
 void setcsr(uint32_t v){csr=v;++writes;}
 void enableFlushMode(){if(!(csr&FlushMask))enableFlushModeUnconditional();}
 void disableFlushMode(){if(csr&FlushMask)disableFlushModeUnconditional();}
 void enableFlushModeUnconditional(){setcsr(csr|FlushMask);}
 void disableFlushModeUnconditional(){setcsr(csr&~FlushMask);}
};
int main() {
 using namespace fear_nx::cpu;
 FP fp;const auto other=fp.csr;
 assert(ApplyFlush(fp,false,true,true));assert(fp.csr==(other|FP::FlushMask));
 for(int i=0;i<1000;++i)assert(!ApplyFlush(fp,i%2,i%3==0,true));
 assert(fp.writes==1);
 assert(ApplyFlush(fp,false,false,false));assert(fp.csr==other);
 assert(!ApplyFlush(fp,false,false,false));
 assert(ApplyFlush(fp,false,true,false));assert(fp.writes==3);
 // SDK host-call restoration may clear FZ: the next guest request repairs it.
 fp.csr&=~FP::FlushMask;assert(ApplyFlush(fp,false,false,true));
 YieldBackoff poll;uint64_t time=100000;
 for(int i=0;i<18;++i){const bool sleep=poll.Polling(time);assert(sleep==(i==17));poll.Returned(time,sleep);time+=1000;}
 assert(poll.Polling(time));poll.Returned(time+100000,true);
 assert(poll.Polling(time+101000));
 assert(!poll.Polling(time+1000000));poll.Returned(time+1000000,false);
 assert(!poll.Polling(1));
 std::atomic<uint32_t> value=0;
 std::vector<std::thread> threads;
 for(int t=0;t<4;++t)threads.emplace_back([&]{
  PPCContext ctx;
  for(int i=0;i<10000;++i){
   const uint32_t saved=CheckInterrupts(ctx);assert(saved==0x8000);
   ctx.msr&=~0x8000;EnterInterrupts();assert(CheckInterrupts(ctx)==0);
   uint32_t old=value.load();while(!value.compare_exchange_weak(old,old+1)){}
   ctx.msr|=saved;LeaveInterrupts();
  }
 });
 for(auto& t:threads)t.join();
 assert(value==40000);assert(ppc_global_lock_count_()==0);
 local_interrupts=false;PPCContext ctx;
 assert(CheckInterrupts(ctx)==0x8000);EnterInterrupts();assert(CheckInterrupts(ctx)==0);
 EnterInterrupts();assert(ppc_global_lock_count_()==2);LeaveInterrupts();LeaveInterrupts();
 assert(CheckInterrupts(ctx)==0x8000);
}
