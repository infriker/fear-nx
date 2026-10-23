// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "deko_vertex_cache.h"
#include <array>
#include <map>

namespace fear_nx::deko {

// Immutable payloads in reserved regions of ONE contiguous GPU upload heap.
// Offsets are relative to the whole heap, not the active ring slot. A cache hit
// pins the payload through the consuming work, regardless of its home region.
class SharedGeometryCache {
 public:
  using Result = PersistentVertexCache::Result;
  using Stats = PersistentVertexCache::Stats;
  SharedGeometryCache(uint32_t slots, uint32_t stride, uint32_t start, uint32_t bytes)
      : slots_(slots), stride_(stride), start_(start), bytes_(bytes), blocked_(slots, UINT64_MAX) {
    for (uint32_t i=0;i<slots_;++i) AddFree(i*stride_+start_,bytes_);
  }
  void BeginWork(uint64_t work, uint64_t completed_before) {
    work_=work;
    // Work zero is real work. A zero completion boundary frees nothing.
    completed_before_=std::max(completed_before_,std::min(work,completed_before));
    for(uint32_t i=0;i<slots_;++i) if(blocked_[i]!=UINT64_MAX && blocked_[i]<completed_before_) {
      blocked_[i]=UINT64_MAX;AddFree(i*stride_+start_,bytes_);
    }
  }
  // Rare full-ring retry: caller has waited for all submitted work. Do not
  // reopen the region until the transient work which overwrites it completes.
  bool BlockRegion(uint32_t slot) {
    if(slot>=slots_ || (blocked_[slot]!=UINT64_MAX && blocked_[slot]>=completed_before_))return false;
    for(const auto& [key,e]:entries_)
      if(e.offset/stride_==slot && e.last_work>=completed_before_)return false;
    for(auto it=entries_.begin();it!=entries_.end();) {
      if(it->second.offset/stride_!=slot){++it;continue;}
      ForgetFast(it->first,&it->second);
      order_.erase(it->second.order);it=entries_.erase(it);++stats_.evictions;
    }
    for(auto it=free_.lower_bound(slot*stride_);it!=free_.end()&&it->first/stride_==slot;)
      it=RemoveFree(it);
    blocked_[slot]=work_;return true;
  }
  Result FindOrAllocate(uint32_t address,uint32_t bytes,uint64_t hash,uint32_t minimum=0) {
    const Key key{address,bytes,hash};
    auto& fast=fast_[Hash{}(key)&(fast_.size()-1)];
    Entry* found=nullptr;
    if(fast_enabled_ && fast.entry && fast.key==key) {found=fast.entry;++fast_hits_;}
    else {
      ++table_lookups_;
      if(auto it=entries_.find(key);it!=entries_.end()) {
        found=&it->second;
        if(fast_enabled_)fast={key,found};
      }
    }
    if(found) {
      auto& e=*found;
      if(e.offset<minimum){++stats_.misses;++stats_.capacity_misses;return {};}
      if(e.last_work!=work_)++cross_work_hits_;
      if(e.offset/stride_!=work_%slots_)++cross_slot_hits_;
      e.last_work=work_;order_.splice(order_.end(),order_,e.order);++stats_.hits;
      return {e.offset,0,true,true};
    }
    ++stats_.misses;
    const uint64_t allocation=(uint64_t(bytes)+63)&~uint64_t(63);
    if(!bytes || allocation>bytes_ || minimum>uint64_t(slots_)*stride_) {
      ++stats_.capacity_misses;return {};
    }
    auto candidate=candidates_.find(key);
    if(candidate==candidates_.end()) {
      if(candidates_.size()>=16384) {
        candidates_.erase(candidate_order_.front());candidate_order_.pop_front();++stats_.candidate_evictions;
      }
      candidate_order_.push_back(key);candidates_.emplace(key,std::prev(candidate_order_.end()));return {};
    }
    candidate_order_.splice(candidate_order_.end(),candidate_order_,candidate->second);
    uint32_t offset=0;
    while(entries_.size()>=32768 || !Allocate(uint32_t(allocation),minimum,offset)) {
      if(order_.empty()){++stats_.capacity_misses;return {};}
      auto victim=entries_.find(order_.front());
      // Last-use order is monotonic, so if the oldest is in flight all are.
      if(victim->second.last_work>=completed_before_){++stats_.capacity_misses;return {};}
      Release(victim->second.offset,victim->second.bytes);
      ForgetFast(victim->first,&victim->second);
      entries_.erase(victim);order_.pop_front();++stats_.evictions;
    }
    candidate_order_.erase(candidate->second);candidates_.erase(candidate);
    order_.push_back(key);
    auto added=entries_.emplace(key,Entry{offset,uint32_t(allocation),work_,std::prev(order_.end())});
    if(fast_enabled_)fast={key,&added.first->second};
    stats_.copied_bytes+=bytes;return {offset,bytes,false,true};
  }
  const Stats& stats() const {return stats_;}
  void FastLookup(bool enabled) {fast_enabled_=enabled;}
  uint64_t fast_hits() const {return fast_hits_;}
  uint64_t table_lookups() const {return table_lookups_;}
  // Try continuing the last fresh allocation before falling back to best fit.
  // Used by index and vertex upload policies. Never reserve extra space or
  // evict a live block merely to preserve locality.
  void PreferSequentialAllocations(bool enabled) {sequential_=enabled;}
  uint64_t sequential_allocations() const {return sequential_allocations_;}
  uint64_t best_fit_allocations() const {return best_fit_allocations_;}
  uint64_t cross_work_hits() const {return cross_work_hits_;}
  uint64_t cross_slot_hits() const {return cross_slot_hits_;}
  size_t size() const {return entries_.size();}
 private:
  struct Key {
    uint32_t address,bytes;uint64_t hash;
    bool operator==(const Key&) const = default;
  };
  struct Hash {size_t operator()(const Key& k) const {
    uint64_t h=k.hash^(uint64_t(k.address)<<32)^k.bytes;
    h^=h>>30;h*=0xBF58476D1CE4E5B9ull;h^=h>>27;h*=0x94D049BB133111EBull;return size_t(h^(h>>31));
  }};
  using Order=std::list<Key>;
  struct Entry {uint32_t offset,bytes;uint64_t last_work;Order::iterator order;};
  struct FastEntry {Key key{};Entry* entry=nullptr;};
  // unordered_map rehash preserves element pointers. Every erase invalidates
  // its fast slot before destroying the entry; hits still update LRU/fences.
  void ForgetFast(const Key& key,const Entry* entry) {
    auto& f=fast_[Hash{}(key)&(fast_.size()-1)];
    if(f.entry==entry)f.entry=nullptr;
  }
  using Sizes=std::multimap<uint32_t,uint32_t>;
  struct Free {uint32_t bytes;Sizes::iterator size;};
  using Frees=std::map<uint32_t,Free>;
  void AddFree(uint32_t offset,uint32_t bytes) {
    if(bytes)free_.emplace(offset,Free{bytes,sizes_.emplace(bytes,offset)});
  }
  Frees::iterator RemoveFree(Frees::iterator it) {
    sizes_.erase(it->second.size);return free_.erase(it);
  }
  bool Allocate(uint32_t bytes,uint32_t minimum,uint32_t& result) {
    if(sequential_ && next_!=UINT32_MAX && next_>=minimum) {
      auto after=free_.upper_bound(next_);
      if(after!=free_.begin()) {
        auto it=std::prev(after);
        const uint32_t start=it->first,size=it->second.bytes;
        if(next_>=start && uint64_t(next_)+bytes<=uint64_t(start)+size) {
          const uint32_t at=next_;
          RemoveFree(it);AddFree(start,at-start);AddFree(at+bytes,start+size-at-bytes);
          result=at;next_=at+bytes;++sequential_allocations_;return true;
        }
      }
    }
    // Best fit visits free extents, never the content/admission table. Normal
    // minimum=0 allocations select the first adequate extent in logarithmic time.
    for(auto it=sizes_.lower_bound(bytes);it!=sizes_.end();++it) {
      uint32_t start=it->second,size=it->first;
      uint64_t at=(std::max<uint64_t>(start,minimum)+63)&~uint64_t(63);
      if(at+bytes>uint64_t(start)+size)continue;
      RemoveFree(free_.find(start));
      AddFree(start,uint32_t(at-start));AddFree(uint32_t(at)+bytes,uint32_t(start+size-at-bytes));
      result=uint32_t(at);next_=result+bytes;++best_fit_allocations_;return true;
    }
    return false;
  }
  void Release(uint32_t offset,uint32_t bytes) {
    auto next=free_.lower_bound(offset);
    if(next!=free_.begin()) {
      auto prev=std::prev(next);
      if(prev->first/stride_==offset/stride_ && prev->first+prev->second.bytes==offset) {
        offset=prev->first;bytes+=prev->second.bytes;RemoveFree(prev);
      }
    }
    if(next!=free_.end() && next->first/stride_==offset/stride_ && offset+bytes==next->first) {
      bytes+=next->second.bytes;RemoveFree(next);
    }
    AddFree(offset,bytes);
  }
  uint32_t slots_,stride_,start_,bytes_;
  uint32_t next_=UINT32_MAX;
  bool sequential_=false;
  uint64_t sequential_allocations_=0,best_fit_allocations_=0;
  uint64_t work_=0,completed_before_=0,cross_work_hits_=0,cross_slot_hits_=0;
  std::vector<uint64_t> blocked_;
  Order order_,candidate_order_;
  std::unordered_map<Key,Entry,Hash> entries_;
  std::array<FastEntry,8192> fast_{};
  bool fast_enabled_=true;
  uint64_t fast_hits_=0,table_lookups_=0;
  std::unordered_map<Key,Order::iterator,Hash> candidates_;
  Frees free_;Sizes sizes_;Stats stats_{};
};
} // namespace fear_nx::deko
