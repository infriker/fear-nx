// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include "fear_cpu_policy.h"
namespace fear_nx::cpu {
extern bool local_interrupts,fixed_flush,yield_backoff,counters_enabled;
struct Counters {uint64_t local_regions=0,global_regions=0,flush_elided=0,flush_written=0,yields=0,sleeps=0,sleep_ns=0;uint32_t pending=0;};
// The zero-initialized counters never need a per-thread initialization thunk.
extern constinit thread_local Counters counters;
void Publish();
inline void Count(){if(counters_enabled && ++counters.pending>=4096)Publish();}
template<class Register> inline void SetFlush(Register& fp,bool enabled,bool unconditional) {
  const bool written=ApplyFlush(fp,enabled,unconditional,fixed_flush);
  // Keep the FP policy active while avoiding TLS writes in normal play.
  if(!counters_enabled)return;
  if(written)++counters.flush_written;
  else ++counters.flush_elided;
  Count();
}
void Configure();
void Report();
}
