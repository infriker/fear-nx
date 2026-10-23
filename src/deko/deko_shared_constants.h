// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "deko_draw_upload_cache.h"
#include <map>

namespace fear_nx::deko {
// Sharing requires the same source bank, destination extent, padding, and copy
// order. Neither a hash nor an equal byte count proves ABI compatibility.
template<class State> class ConstantLayoutPool {
 public:
  State* Intern(uint64_t bank,uint64_t private_identity,size_t source_bytes,
                size_t destination_bytes,std::span<const ConstantCopyRange> ranges) {
    std::vector<ConstantCopyRange> canonical;
    for(auto r:ranges) {
      if(r.source_offset>source_bytes || r.size>source_bytes-r.source_offset ||
         r.destination_offset>destination_bytes || r.size>destination_bytes-r.destination_offset)return nullptr;
      if(!canonical.empty() && canonical.back().source_offset+canonical.back().size==r.source_offset &&
         canonical.back().destination_offset+canonical.back().size==r.destination_offset)
        canonical.back().size+=r.size;
      else canonical.push_back(r);
    }
    std::vector<uint64_t> key{bank,private_identity,source_bytes,destination_bytes};
    for(auto r:canonical)key.insert(key.end(),{r.source_offset,r.destination_offset,r.size});
    if(auto it=states_.find(key);it!=states_.end()){++shared_layouts_;return it->second.get();}
    auto state=std::make_unique<State>();
    if(!state->plan.Configure(source_bytes,destination_bytes,canonical))return nullptr;
    auto* result=state.get();states_.emplace(std::move(key),std::move(state));return result;
  }
  size_t size() const {return states_.size();}
  uint64_t shared_layouts() const {return shared_layouts_;}
 private:
  std::map<std::vector<uint64_t>,std::unique_ptr<State>> states_;
  uint64_t shared_layouts_=0;
};
} // namespace fear_nx::deko
