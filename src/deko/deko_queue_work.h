// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace fear_nx::deko {
// A partial send advances GPU execution, never ownership. Only the final
// fence permits reuse/retirement of this slot's commands, constants and images.
class QueueWorkState {
 public:
  static constexpr uint32_t kMaxPartialSends=3;
  void Begin() {pending_=true;partials_=0;}
  void Changed() {pending_=true;}
  bool pending() const {return pending_;}
  bool CanSendPartial() const {return partials_<kMaxPartialSends;}
  void SentPartial() {pending_=false;++partials_;}
  void Closed() {pending_=false;partials_=0;}
 private:
  uint32_t partials_=0;
  bool pending_=false;
};
} // namespace fear_nx::deko
