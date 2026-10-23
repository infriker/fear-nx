// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace fear_nx::native {

// All inputs describe the same reporting interval on the ring thread. Packet
// time contains renderer calls, external calls, waits and pacing. WAIT_REG_MEM
// also contains its external KickRecorder call, so that overlap is removed
// before the wait is subtracted from the dispatch remainder.
struct PacketDispatchBreakdown {
  bool remainder_valid = false;
  bool wait_breakdown_valid = false;
  uint64_t outside_renderer_ns = 0;
  uint64_t wait_excluding_kick_ns = 0;
  uint64_t other_dispatch_ns = 0;
};

constexpr PacketDispatchBreakdown BreakDownPacketDispatch(
    uint64_t packet_ns, uint64_t renderer_ns, uint64_t external_ns,
    uint64_t wait_ns, uint64_t wait_kick_ns, uint64_t pacing_ns) {
  PacketDispatchBreakdown result;
  if (renderer_ns > packet_ns || external_ns > packet_ns - renderer_ns) {
    return result;
  }
  result.remainder_valid = true;
  result.outside_renderer_ns = packet_ns - renderer_ns - external_ns;
  if (wait_kick_ns > wait_ns || wait_kick_ns > external_ns) {
    return result;
  }
  result.wait_excluding_kick_ns = wait_ns - wait_kick_ns;
  if (result.wait_excluding_kick_ns > result.outside_renderer_ns) {
    return result;
  }
  const uint64_t after_wait =
      result.outside_renderer_ns - result.wait_excluding_kick_ns;
  if (pacing_ns > after_wait) {
    return result;
  }
  result.other_dispatch_ns = after_wait - pacing_ns;
  result.wait_breakdown_valid = true;
  return result;
}

}  // namespace fear_nx::native
