// SPDX-License-Identifier: GPL-3.0-or-later
#include <rex/input/switch/button_bindings.h>
#include <cassert>
#include <cstdio>

using namespace rex::input::nx;
int main() {
  assert(ResolveBinding("default", 0x1000) == 0x1000);
  assert(ResolveBinding("", 0x2000) == 0x2000);
  assert(ResolveBinding("typo", kBindingLT) == kBindingLT);
  assert(ResolveBinding("none", kBindingLT) == 0);
  assert(ResolveBinding("RS", kBindingLT) == 0x80);
  assert(ResolveBinding("lt", 0x80) == kBindingLT);
  // ZL->RS and R3->LT may be held together; triggers aren't truncated into buttons.
  const auto mapped = ResolveBinding("rs", kBindingLT) | ResolveBinding("lt", 0x80);
  assert(static_cast<uint16_t>(mapped) == 0x80);
  assert((mapped & kBindingLT) && !(mapped & kBindingRT));
  assert((ResolveBinding("rt", 0) | ResolveBinding("rt", 0)) == kBindingRT);
  const char* names[] = {"up", "down", "left", "right", "start", "back", "ls", "rs",
                         "lb", "rb", "a", "b", "x", "y", "lt", "rt"};
  uint32_t all = 0;
  for (const auto* name : names) {
    const auto bit = ResolveBinding(name, 0);
    assert(bit && !(all & bit));
    all |= bit;
  }
  assert(all == 0x3f3ff);
  std::puts("Bindings tests passed: defaults, invalid names, disabling, triggers, swaps, simultaneous keys.");
}
