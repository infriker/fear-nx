// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>

namespace fear_nx::deko {
// Exclusive constant storage per work-ring slot. Unlike shared geometry, no
// allocation may be bound from a different slot. BeginWork requires its old
// fence to be complete. Full-slot retries invalidate only that slot's leases.
template<uint32_t Slots> class ConstantSlotArena {
  static_assert(Slots>0);
 public:
  struct Lease {
    uint64_t epoch=0;
    uint32_t slot=0,offset=0,bytes=0;
    explicit operator bool() const {return epoch && bytes;}
  };
  bool BeginWork(uint64_t work,uint64_t completed_before,uint32_t offset,uint32_t capacity,bool enabled) {
    active_=false;
    if(offset%256 || capacity%256 || uint64_t(offset)+capacity>UINT32_MAX)return false;
    auto& s=slots_[work%Slots];
    if(s.seen && s.last_work>=completed_before)return false;
    if(!s.seen || !enabled || !s.enabled || s.offset!=offset || s.capacity!=capacity) {
      ++s.epoch;s.used=0;
    }
    s.offset=offset;s.capacity=capacity;s.enabled=enabled;s.seen=true;s.last_work=work;
    slot_=uint32_t(work%Slots);active_=enabled;return true;
  }
  bool Matches(const Lease& lease,uint32_t bytes) const {
    if(!active_ || !lease || lease.slot!=slot_ || lease.bytes!=bytes)return false;
    const auto& s=slots_[slot_];
    return lease.epoch==s.epoch && lease.offset>=s.offset &&
           uint64_t(lease.offset)+lease.bytes<=uint64_t(s.offset)+s.used;
  }
  Lease Allocate(uint32_t bytes) {
    if(!active_ || !bytes || bytes%256)return {};
    auto& s=slots_[slot_];
    if(bytes>s.capacity-s.used || uint64_t(s.offset)+s.used+bytes>UINT32_MAX)return {};
    Lease lease{s.epoch,slot_,s.offset+s.used,bytes};s.used+=bytes;return lease;
  }
  uint32_t slot() const {return slot_;}
  bool active() const {return active_;}
 private:
  struct Slot {uint64_t epoch=0,last_work=0;uint32_t offset=0,capacity=0,used=0;bool enabled=false,seen=false;};
  std::array<Slot,Slots> slots_{};
  uint32_t slot_=0;
  bool active_=false;
};
} // namespace fear_nx::deko
