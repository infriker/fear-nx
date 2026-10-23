// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Included after the original generated PCH: same SDK context and global-lock count.
#include "fear_cpu.h"
namespace fear_nx::cpu {
inline uint64_t CheckInterrupts(PPCContext& ctx) {
  if(local_interrupts)return ctx.msr&0x8000u;
  auto lock=rex::thread::global_critical_region::AcquireDirect();
  return ppc_global_lock_count_().load()?0:0x8000;
}
inline void EnterInterrupts() {
  if(local_interrupts) {
    if(counters_enabled)++counters.local_regions;
  }
  else {
    rex::thread::global_critical_region::mutex().lock();
    ppc_global_lock_count_().fetch_add(1);
    if(counters_enabled)++counters.global_regions;
  }
  Count();
}
inline void LeaveInterrupts() {
  if(local_interrupts)return; // generated mtmsrd restored this thread's ctx.msr.
  const auto old=ppc_global_lock_count_().fetch_sub(1);
  assert(old>=1);
  rex::thread::global_critical_region::mutex().unlock();
}
}
