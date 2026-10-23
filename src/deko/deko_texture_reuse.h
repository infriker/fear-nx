// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace fear_nx::deko::detail {
// Same reuse window as TextureChecks::Check. This only chooses a cheaper way
// to reach that existing check at an epoch/frame boundary; it never extends
// its next deadline. Rewinding frames conservatively takes the normal path.
template<class State>
constexpr bool TextureCheckReusable(const State& state,uint64_t frame) {
  return state.valid && frame>=state.last &&
      (state.last==frame || frame<state.next);
}
} // namespace fear_nx::deko::detail
