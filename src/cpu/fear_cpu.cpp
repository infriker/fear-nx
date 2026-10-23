// SPDX-License-Identifier: GPL-3.0-or-later
#include "fear_cpu.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
REXCVAR_DEFINE_BOOL(nx_native_cpu_local_interrupts,true,"F.E.A.R. NX","Use per-thread MSR only in verified CAS-only guest regions").lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(nx_native_cpu_fixed_flush,true,"F.E.A.R. NX","Keep guest FP flush enabled; scalar subnormals become zero").lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(nx_native_cpu_yield_backoff,true,"F.E.A.R. NX","Sleep 100 us after sustained repeated yields").lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(nx_native_cpu_adapter_profile,false,"F.E.A.R. NX","Collect guest CPU adapter counters; adds work to every FP-mode request").lifecycle(rex::cvar::Lifecycle::kInitOnly);
namespace fear_nx::cpu {
bool local_interrupts=false,fixed_flush=false,yield_backoff=false,counters_enabled=false;
constinit thread_local Counters counters;
static std::array<std::atomic<uint64_t>,7> totals{};
void Publish() {
  if(!counters_enabled)return;
  const uint64_t values[]={counters.local_regions,counters.global_regions,counters.flush_elided,counters.flush_written,counters.yields,counters.sleeps,counters.sleep_ns};
  for(unsigned i=0;i<7;++i)if(values[i])totals[i].fetch_add(values[i],std::memory_order_relaxed);
  counters={};
}
void Configure() {
  local_interrupts=REXCVAR_GET(nx_native_cpu_local_interrupts);
  fixed_flush=REXCVAR_GET(nx_native_cpu_fixed_flush);
  yield_backoff=REXCVAR_GET(nx_native_cpu_yield_backoff);
  counters_enabled=REXCVAR_GET(nx_native_cpu_adapter_profile);
  REXLOG_INFO("[fear-cpu] adapter v2: local interrupts {} (89 CAS-only regions, original fences); fixed flush {}; yield backoff {}; adapter profile={}; SDK unchanged",
      local_interrupts,fixed_flush,yield_backoff,counters_enabled);
}
void Report() {
  if(!counters_enabled)return;
  std::array<uint64_t,7> v{};for(unsigned i=0;i<7;++i)v[i]=totals[i].exchange(0,std::memory_order_relaxed);
  REXLOG_INFO("[fear-cpu] published counters: local/global regions {}/{}; FP writes {} elided {}; yields {} sleeps {}, requested sleep {:.3f} ms; batched per thread, not exact frame timing",
      v[0],v[1],v[3],v[2],v[4],v[5],double(v[6])/1000000.);
}
static uint64_t Now(){return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
}
extern "C" void __real__ZN3rex6thread10MaybeYieldEv();
extern "C" void __wrap__ZN3rex6thread10MaybeYieldEv() {
  using namespace fear_nx::cpu;
  if(!yield_backoff){__real__ZN3rex6thread10MaybeYieldEv();return;}
  thread_local YieldBackoff backoff;
  const bool sleep=backoff.Polling(Now());
  if(counters_enabled)++counters.yields;
  if(sleep) {
    const timespec delay{0,100000};nanosleep(&delay,nullptr);
    if(counters_enabled){++counters.sleeps;counters.sleep_ns+=100000;}
    __sync_synchronize();
  } else __real__ZN3rex6thread10MaybeYieldEv();
  backoff.Returned(Now(),sleep);
  // Publish after a bounded batch, without contending on counters per yield.
  Count();
}
