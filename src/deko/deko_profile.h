#pragma once

#include <cstdint>

#if !defined(__aarch64__)
#include <chrono>
#endif

namespace fear_nx::deko {

// Horizon's 19.2 MHz monotonic counter; no service calls in the draw hot path.
inline uint64_t ProfileClockNs() {
#if defined(__aarch64__)
  uint64_t ticks;
  asm volatile("mrs %0, cntpct_el0" : "=r"(ticks) :: "memory");
  return ticks * 625 / 12;
#else
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

// Ring-thread wall time, including waits inside a measured phase. Nested
// counters are a breakdown, not additional time to sum with their parent.
class ProfileScope {
 public:
  explicit ProfileScope(uint64_t& ns, bool enabled = true)
      : counter_(enabled ? &ns : nullptr), enabled_(enabled),
        start_(enabled ? ProfileClockNs() : 0) {}
  ~ProfileScope() { Stop(); }
  ProfileScope(const ProfileScope&) = delete;
  ProfileScope& operator=(const ProfileScope&) = delete;
  void Stop() {
    if (!counter_) return;
    *counter_ += ProfileClockNs() - start_;
    counter_ = nullptr;
  }
  void Next(uint64_t& ns) {
    if (!enabled_) return;
    const uint64_t now = ProfileClockNs();
    if (counter_) *counter_ += now - start_;
    counter_ = &ns;
    start_ = now;
  }
 private:
  uint64_t* counter_;
  bool enabled_;
  uint64_t start_;
};

}  // namespace fear_nx::deko
