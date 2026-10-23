// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace fear_nx::cpu {
struct YieldBackoff {
  uint64_t previous_ns=0;
  uint32_t consecutive=0;
  bool Polling(uint64_t now) {
    if(previous_ns && now>=previous_ns && now-previous_ns<20000) {
      if(consecutive<17)++consecutive;
    } else consecutive=0;
    return consecutive>16;
  }
  void Returned(uint64_t now,bool slept) {previous_ns=slept&&now>1000?now-1000:now;}
};
template<class Register>
inline bool ApplyFlush(Register& fp,bool enabled,bool unconditional,bool fixed) {
  if(fixed) {
    if((fp.csr&Register::FlushMask)==Register::FlushMask)return false;
    fp.csr|=Register::FlushMask;fp.setcsr(fp.csr);return true;
  }
  const bool changed=bool(fp.csr&Register::FlushMask)!=enabled;
  if(enabled) {if(unconditional)fp.enableFlushModeUnconditional();else fp.enableFlushMode();}
  else {if(unconditional)fp.disableFlushModeUnconditional();else fp.disableFlushMode();}
  return changed||unconditional;
}
}
