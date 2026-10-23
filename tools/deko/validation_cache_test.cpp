// SPDX-License-Identifier: GPL-3.0-or-later
#include "deko/deko_validation_cache.h"
#include "deko/deko_binding_cache.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using fear_nx::deko::ValidationCache;
using fear_nx::deko::ValidationKind;

bool Expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "FAIL: " << message << '\n';
  return false;
}

std::vector<uint8_t> MakeRange(size_t size) {
  std::vector<uint8_t> data(size);
  for (size_t i = 0; i < size; ++i)
    data[i] = static_cast<uint8_t>((i * 37 + i / 13 + 11) & 0xFF);
  return data;
}

uint64_t Fold(uint64_t current, uint64_t value) {
  uint64_t mixed = (current ^ value) * 0x9E3779B97F4A7C15ull;
  return mixed ^ (mixed >> 29);
}

uint64_t ReferenceFingerprint(const uint8_t* data, size_t size, uint64_t seed,
                              uint32_t address) {
  uint64_t result = 0x243F6A8885A308D3ull ^ seed ^ (uint64_t(address) << 1) ^ size;
  const size_t line_count = (size + 63) / 64;
  const uint64_t line_base = uint64_t(address) >> 6;
  for (size_t line = 0; line < line_count; ++line) {
    const size_t offset = line * 64;
    const size_t bytes = std::min<size_t>(64, size - offset);
    const uint64_t line_hash = XXH3_64bits_withSeed(data + offset, bytes, seed);
    result = Fold(result, line_hash ^ (line_base + line));
  }
  return result;
}

size_t ReferenceSampleBytes(size_t size, uint32_t address, uint32_t phase) {
  const size_t line_count = (size + 63) / 64;
  const size_t last = line_count - 1;
  const uint64_t line_base = uint64_t(address) >> 6;
  size_t bytes = 0;
  for (size_t line = 0; line < line_count; ++line) {
    const uint64_t absolute_line = line_base + line;
    if ((absolute_line & 7) == phase || line == 0 || line == last)
      bytes += std::min<size_t>(64, size - line * 64);
  }
  return bytes;
}

bool TestRawHashAndDanteFingerprint() {
  bool ok = true;
  auto small = MakeRange(1024);
  ValidationCache exact;
  auto raw = exact.Hash(small.data(), 0x10000, small.size());
  ok &= Expect(raw.hash == XXH3_64bits(small.data(), small.size()),
               "small-range seed-zero hash preserves XXH3_64bits output");

  auto data = MakeRange(16384);
  ValidationCache cache;
  cache.SetEnabled(true);
  cache.BeginFrame(7);
  const uint32_t address = 0x20000;
  const auto first = cache.Hash(data.data(), address, data.size());
  ok &= Expect(first.hash == ReferenceFingerprint(data.data(), data.size(), 0, address),
               "initial full fingerprint matches the independent 64-byte reference");
  ok &= Expect(first.bytes_hashed == data.size(), "initial check reads the range once");

  const auto same_epoch = cache.Hash(data.data(), address, data.size());
  ok &= Expect(same_epoch.hash == first.hash && same_epoch.bytes_hashed == 0,
               "same-epoch lookup reuses the validated digest without reading memory");
  cache.Revalidate();
  const auto sampled = cache.Hash(data.data(), address, data.size());
  const uint32_t phase = uint32_t((7 + (uint64_t(address) >> 6)) & 7);
  ok &= Expect(phase < ValidationCache::kPhaseCount,
               "the selected rotating phase remains within the eight phase groups");
  ok &= Expect(sampled.hash == first.hash && sampled.bytes_hashed > 0,
               "first reuse checks a Dante-style phase plus first and last lines");
  ok &= Expect(sampled.bytes_hashed == data.size() +
                                         ReferenceSampleBytes(data.size(), address, phase),
               "first and last cache lines are counted once in the phase sample");
  ok &= Expect(cache.stats().sampled_checks == 1 && cache.stats().guard_checks == 1,
               "the initial sampled reuse is inside the full-check training window");
  ok &= Expect(cache.stats().training_remaining == ValidationCache::kGuardChecks - 1,
               "training quota counts successful matching full guards");

  const uint64_t seeded = 0x123456789ABCDEF0ull;
  const auto seeded_result = cache.Hash(data.data(), address, data.size(), seeded);
  ok &= Expect(seeded_result.hash == ReferenceFingerprint(data.data(), data.size(), seeded, address),
               "seed remains part of the composite fingerprint");
  return ok;
}

bool TestTrainingQuotaIgnoresChangedSamplesAndPhaseSweep() {
  bool ok = true;
  auto data = MakeRange(8192);
  ValidationCache cache;
  cache.SetEnabled(true);
  cache.BeginFrame(5);
  constexpr uint32_t address = 0x12345;
  (void)cache.Hash(data.data(), address, data.size());

  data[0] ^= 0x45;
  cache.Revalidate();
  (void)cache.Hash(data.data(), address, data.size());
  ok &= Expect(cache.stats().sampled_checks == 1 && cache.stats().guard_checks == 0 &&
                   cache.stats().training_remaining == ValidationCache::kGuardChecks,
               "a changed sample is fully refreshed but does not consume matching-guard quota");

  cache.Revalidate();
  (void)cache.Hash(data.data(), address, data.size());
  ok &= Expect(cache.stats().guard_checks == 1 &&
                   cache.stats().training_remaining == ValidationCache::kGuardChecks - 1,
               "a stable matching sample consumes one successful guard check");

  // Sweep every phase against a partial final line and an unaligned guest
  // address. Each check must match the phase fingerprint built by the full pass.
  auto offset_data = MakeRange(1025);
  ValidationCache sweep;
  sweep.SetEnabled(true);
  sweep.BeginFrame(0);
  (void)sweep.Hash(offset_data.data(), address, offset_data.size());
  std::array<bool, 8> seen{};
  for (uint64_t frame = 1; frame <= 8; ++frame) {
    sweep.BeginFrame(frame);
    const uint32_t phase = uint32_t((frame + (uint64_t(address) >> 6)) & 7);
    seen[phase] = true;
    const auto checked = sweep.Hash(offset_data.data(), address, offset_data.size());
    const size_t sample_bytes = ReferenceSampleBytes(offset_data.size(), address, phase);
    ok &= Expect(checked.bytes_hashed == offset_data.size() + sample_bytes,
                 "phase sample touches selected lines, both edges, and the partial tail once");
  }
  ok &= Expect(std::all_of(seen.begin(), seen.end(), [](bool value) { return value; }),
               "the rotating phase visits all eight line groups");
  ok &= Expect(sweep.stats().sample_changes == 0 && sweep.stats().guard_checks == 8,
               "each sampled phase folds in the same order as its full fingerprint");
  return ok;
}

bool TestGlobalGuardTrainingAndStop() {
  bool ok = true;
  auto data = MakeRange(2048);
  ValidationCache cache;
  cache.SetEnabled(true);
  cache.BeginFrame(1);
  constexpr uint32_t address = 0x30000;
  (void)cache.Hash(data.data(), address, data.size());

  for (uint64_t check = 0; check < ValidationCache::kGuardChecks; ++check) {
    cache.Revalidate();
    (void)cache.Hash(data.data(), address, data.size());
  }
  ok &= Expect(cache.stats().sampled_checks == ValidationCache::kGuardChecks,
               "global training counts sampled checks across soft invalidations");
  ok &= Expect(cache.stats().guard_checks == ValidationCache::kGuardChecks,
               "every matching sample is fully checked during the 20,000-check window");
  ok &= Expect(cache.stats().training_remaining == 0 && !cache.stats().sampling_disabled,
               "successful training ends without disabling sampling");

  // Frame and work progress do not restart the global training window. After
  // it ends, an unsampled mutation can be missed until a future sample or hard
  // content signal; this is the explicitly accepted performance tradeoff.
  data[2 * 64 + 9] ^= 0x51;
  cache.Revalidate();  // Preserve the sampled phase to exercise a post-training miss.
  const uint64_t checks_before = cache.stats().full_checks;
  const auto after_training = cache.Hash(data.data(), address, data.size());
  ok &= Expect(after_training.hash != ReferenceFingerprint(data.data(), data.size(), 0, address),
               "post-training unsampled mutation may remain memoized until its phase rotates");
  ok &= Expect(cache.stats().full_checks == checks_before,
               "no permanent periodic full guard runs after training");

  cache.HardInvalidate();
  ok &= Expect(cache.stats().sampled_checks == ValidationCache::kGuardChecks + 1,
               "hard entry clearing preserves process-wide guard training progress");
  return ok;
}

bool TestGuardDetectsUnsampledMutation() {
  bool ok = true;
  auto data = MakeRange(4096);
  auto unaffected = MakeRange(4096);
  auto adjacent_data = MakeRange(2048);
  ValidationCache cache;
  cache.SetEnabled(true);
  cache.BeginFrame(1);
  constexpr uint32_t address = 0x40000;  // Address line phase is zero.
  (void)cache.Hash(data.data(), address, data.size());
  (void)cache.Hash(unaffected.data(), 0x50000, unaffected.size());
  data[2 * 64 + 5] ^= 0xA7;              // Outside phase 1 and the edge lines.
  cache.Revalidate();
  const uint64_t epoch_before_guard=cache.memo_epoch();
  const auto guarded = cache.Hash(data.data(), address, data.size());
  ok &= Expect(cache.memo_epoch()==epoch_before_guard && cache.stats().guard_misses==1,
               "guard generation changes even when epoch does not");
  ok &= Expect(!cache.TextureRangeCanReuseBinding(address+64,2048) &&
                   cache.TextureRangeCanReuseBinding(0x50000,4096),
               "binding memo excludes overlapping quarantines but allows unaffected ranges");
  ok &= Expect(guarded.hash == XXH3_64bits_withSeed(data.data(), data.size(), 0),
               "training guard detects an unsampled write and returns exact raw XXH3");
  ok &= Expect(guarded.guard_miss && cache.stats().guard_misses == 1 &&
                   !cache.stats().sampling_disabled && cache.stats().quarantined_ranges == 1,
               "a training mismatch quarantines only its physical range");
  ok &= Expect(cache.stats().training_remaining == ValidationCache::kGuardChecks,
               "a failed guard does not consume the global matching-guard quota");

  data[3 * 64 + 7] ^= 0x31;
  const auto strict = cache.Hash(data.data(), address, data.size());
  ok &= Expect(strict.hash == XXH3_64bits_withSeed(data.data(), data.size(), 0),
               "quarantined range full-hashes every use, including same-epoch calls");
  ok &= Expect(cache.stats().strict_lookups == 1 && cache.stats().strict_bytes == data.size(),
               "strict lookup and byte counters advance for a quarantined range");

  cache.Revalidate();
  const uint64_t guards_before = cache.stats().guard_checks;
  const auto unaffected_check = cache.Hash(unaffected.data(), 0x50000, unaffected.size());
  ok &= Expect(!unaffected_check.guard_miss && cache.stats().guard_checks == guards_before + 1 &&
                   !cache.stats().sampling_disabled,
               "unaffected ranges continue sampled checks and matching-guard training");
  const auto unaffected_hit = cache.Hash(unaffected.data(), 0x50000, unaffected.size());
  ok &= Expect(unaffected_hit.bytes_hashed == 0,
               "unaffected ranges still memo-hit within the same epoch");

  const auto alias = cache.Hash(data.data() + 64, address + 64, 2048, 0x1234,
                                ValidationKind::kTexture);
  ok &= Expect(alias.hash == XXH3_64bits_withSeed(data.data() + 64, 2048, 0x1234) &&
                   cache.stats().strict_lookups == 2,
               "a partial overlap with a different seed and kind remains strict");

  const uint32_t adjacent_address = address + uint32_t(data.size());
  (void)cache.Hash(adjacent_data.data(), adjacent_address, adjacent_data.size());
  cache.Revalidate();
  const uint64_t strict_before_adjacent = cache.stats().strict_lookups;
  (void)cache.Hash(adjacent_data.data(), adjacent_address, adjacent_data.size());
  ok &= Expect(cache.stats().strict_lookups == strict_before_adjacent,
               "an exactly adjacent half-open interval remains eligible for sampling");

  cache.HardInvalidate();
  const uint64_t strict_before_clear = cache.stats().strict_lookups;
  (void)cache.Hash(data.data(), address, data.size());
  ok &= Expect(cache.stats().strict_lookups == strict_before_clear + 1 &&
                   cache.stats().quarantined_ranges == 1,
               "hard entry clearing preserves physical-range quarantine state");
  cache.SetEnabled(false);
  cache.SetEnabled(true);
  const uint64_t strict_before_toggle = cache.stats().strict_lookups;
  (void)cache.Hash(data.data(), address, data.size());
  ok &= Expect(cache.stats().strict_lookups == strict_before_toggle + 1,
               "cache configuration toggles preserve known unsafe ranges");
  return ok;
}

bool TestQuarantineSaturationFallback() {
  bool ok = true;
  auto data = MakeRange(2048);
  ValidationCache cache;
  cache.SetEnabled(true);
  cache.BeginFrame(1);

  for (uint32_t i = 0; i <= ValidationCache::kMaxQuarantinedRanges; ++i) {
    const uint32_t address = 0x100000 + i * 0x10000;
    (void)cache.Hash(data.data(), address, data.size());
    data[2 * 64 + 5] ^= 0x01;  // Unsampled in phase 1 for each aligned address.
    cache.Revalidate();
    const auto mismatch = cache.Hash(data.data(), address, data.size());
    ok &= Expect(mismatch.guard_miss, "each newly observed range reports its guard miss");
  }
  ok &= Expect(cache.stats().quarantine_overflows == 1 && cache.stats().sampling_disabled &&
                   cache.stats().quarantined_ranges == ValidationCache::kMaxQuarantinedRanges,
               "only 8,193 disjoint ranges trigger the global saturation fallback");
  const auto exact = cache.Hash(data.data(), 0x40000000, data.size());
  ok &= Expect(exact.hash == XXH3_64bits_withSeed(data.data(), data.size(), 0),
               "saturation fallback restores exact full hashing for every range");
  return ok;
}

bool TestSampleChangeAndVolatileGeometry() {
  bool ok = true;
  auto data = MakeRange(8192);
  ValidationCache cache;
  cache.SetEnabled(true);
  cache.BeginFrame(10);
  constexpr uint32_t address = 0x50000;
  (void)cache.Hash(data.data(), address, data.size());

  data[0] ^= 0x81;
  cache.Revalidate();
  const auto first_change = cache.Hash(data.data(), address, data.size());
  ok &= Expect(first_change.hash == ReferenceFingerprint(data.data(), data.size(), 0, address),
               "a changed sampled line refreshes the complete fingerprint");
  ok &= Expect(!first_change.bypass_cache, "the first observed change remains memoizable");

  cache.Revalidate();
  const auto stable = cache.Hash(data.data(), address, data.size());
  ok &= Expect(!stable.bypass_cache, "a stable check resets the consecutive-change streak");

  data[0] ^= 0x27;
  cache.Revalidate();
  const auto after_stable = cache.Hash(data.data(), address, data.size());
  ok &= Expect(!after_stable.bypass_cache,
               "a change after a stable sample starts a new consecutive-change streak");

  data[0] ^= 0x27;
  cache.Revalidate();
  const auto second_change = cache.Hash(data.data(), address, data.size());
  const uint64_t raw_hash = XXH3_64bits_withSeed(data.data(), data.size(), 0);
  ok &= Expect(second_change.bypass_cache && second_change.volatile_geometry &&
                   second_change.hash == raw_hash && cache.stats().volatile_bypasses == 1,
               "the second consecutive geometry change starts volatility with an exact raw hash");
  const uint32_t phase = uint32_t((10 + (uint64_t(address) >> 6)) & 7);
  ok &= Expect(second_change.bytes_hashed ==
                   ReferenceSampleBytes(data.size(), address, phase) + 2 * data.size(),
               "the transition accounts for its sample, sampled refresh, and exact raw hash");
  ok &= Expect(cache.stats().volatile_exact_checks == 1 &&
                   cache.stats().volatile_full_bytes == data.size(),
               "transition performs and counts one full exact volatile check");

  cache.Revalidate();
  const auto volatile_use = cache.Hash(data.data(), address, data.size());
  ok &= Expect(volatile_use.bypass_cache && volatile_use.volatile_geometry &&
                   volatile_use.hash == raw_hash && volatile_use.bytes_hashed == data.size(),
               "volatile geometry returns an exact raw hash on every use");
  const auto volatile_same = cache.Hash(data.data(), address, data.size());
  ok &= Expect(volatile_same.volatile_geometry && volatile_same.hash == volatile_use.hash &&
                   volatile_same.bytes_hashed == data.size(),
               "unchanged volatile bytes keep the same exact hash even within one epoch");

  data[31] ^= 0x73;
  const auto volatile_changed = cache.Hash(data.data(), address, data.size());
  ok &= Expect(volatile_changed.volatile_geometry &&
                   volatile_changed.hash == XXH3_64bits_withSeed(data.data(), data.size(), 0) &&
                   volatile_changed.hash != volatile_same.hash &&
                   volatile_changed.bytes_hashed == data.size(),
               "same-epoch volatile writes are detected by the exact full-range hash");
  ok &= Expect(cache.stats().volatile_exact_checks == 4 &&
                   cache.stats().volatile_full_bytes == 4 * data.size(),
               "every transition and volatile use contributes exact-check counters");

  cache.BeginFrame(73);
  const auto still_volatile = cache.Hash(data.data(), address, data.size());
  ok &= Expect(still_volatile.bypass_cache && still_volatile.volatile_geometry &&
                   still_volatile.hash == XXH3_64bits_withSeed(data.data(), data.size(), 0),
               "volatile geometry remains exactly hashed through frame 73");
  ok &= Expect(cache.stats().volatile_exact_checks == 5 &&
                   cache.stats().volatile_full_bytes == 5 * data.size(),
               "the final volatile frame is included in exact-check counters");
  cache.BeginFrame(74);
  const auto expired = cache.Hash(data.data(), address, data.size());
  ok &= Expect(!expired.bypass_cache && !expired.volatile_geometry &&
                   expired.bytes_hashed == data.size() &&
                   expired.hash == ReferenceFingerprint(data.data(), data.size(), 0, address),
               "volatile expiry fully reseeds the fingerprint before sampling resumes");
  ok &= Expect(cache.stats().volatile_exact_checks == 5 &&
                   cache.stats().volatile_full_bytes == 5 * data.size(),
               "expiration returns to sampled mode without a volatile raw check");
  return ok;
}

bool TestAdaptiveTextureSchedule() {
  bool ok = true;
  auto data = MakeRange(4096);
  ValidationCache cache;
  cache.SetEnabled(true);
  cache.BeginFrame(1);
  constexpr uint32_t address = 0x60000;
  (void)cache.Hash(data.data(), address, data.size(), 0, ValidationKind::kTexture);

  auto texture_at = [&](uint64_t frame) {
    cache.BeginFrame(frame);
    return cache.Hash(data.data(), address, data.size(), 0, ValidationKind::kTexture);
  };
  (void)texture_at(2);  // interval becomes 2, next due frame 4.
  const uint64_t samples_after_2 = cache.stats().sampled_checks;
  (void)texture_at(3);
  ok &= Expect(cache.stats().sampled_checks == samples_after_2 && cache.stats().texture_deferred == 1,
               "texture revalidation defers work until its next adaptive frame");
  (void)texture_at(4);  // 4
  (void)texture_at(8);  // 8
  (void)texture_at(16); // 16
  (void)texture_at(32); // 32
  const uint64_t at_32 = cache.stats().sampled_checks;
  (void)texture_at(63);
  ok &= Expect(cache.stats().sampled_checks == at_32,
               "texture interval caps at 32 frames");
  (void)texture_at(64);
  ok &= Expect(cache.stats().sampled_checks == at_32 + 1,
               "texture is sampled again when the 32-frame interval expires");
  ok &= Expect(cache.stats().guard_checks == cache.stats().sampled_checks,
               "texture samples participate in the same bounded global guard training");
  return ok;
}

bool TestKeyIsolationCapacityAndBoundaries() {
  bool ok = true;
  auto data = MakeRange(2048);
  ValidationCache keyed;
  keyed.SetEnabled(true);
  keyed.BeginFrame(1);
  (void)keyed.Hash(data.data(), 0x70000, data.size(), 0, ValidationKind::kGeometry);
  (void)keyed.Hash(data.data(), 0x70000, data.size(), 0, ValidationKind::kTexture);
  (void)keyed.Hash(data.data(), 0x70004, data.size(), 0, ValidationKind::kGeometry);
  (void)keyed.Hash(data.data(), 0x70000, data.size() - 1, 0, ValidationKind::kGeometry);
  (void)keyed.Hash(data.data(), 0x70000, data.size(), 1, ValidationKind::kGeometry);
  ok &= Expect(keyed.entry_count() == 5,
               "address, length, seed, and geometry/texture kind are all part of the key");

  ValidationCache capacity;
  capacity.SetEnabled(true);
  capacity.BeginFrame(1);
  for (uint32_t i = 0; i <= ValidationCache::kMaxEntries; ++i)
    (void)capacity.Hash(data.data(), 0x100000 + i * 0x1000, data.size());
  ok &= Expect(capacity.entry_count() == 1, "cache overflow clears entries and remains bounded");

  ValidationCache boundaries;
  boundaries.SetEnabled(true);
  boundaries.BeginFrame(1);
  const auto empty = boundaries.Hash(nullptr, 0, 0);
  ok &= Expect(empty.hash == XXH3_64bits_withSeed(nullptr, 0, 0) && empty.bytes_hashed == 0,
               "empty range is handled safely");
  auto small = MakeRange(ValidationCache::kSmallRangeBytes);
  (void)boundaries.Hash(small.data(), 0x80000, small.size());
  const uint64_t checks_before = boundaries.stats().full_checks;
  (void)boundaries.Hash(small.data(), 0x80000, small.size());
  ok &= Expect(boundaries.stats().full_checks == checks_before + 1,
               "small ranges are checked exactly on each use");

  const size_t entries_before_invalid = boundaries.entry_count();
  (void)boundaries.Hash(data.data(), 0xFFFFFFF0u, data.size());
  (void)boundaries.Hash(data.data(), 0xFFFFFFF0u, data.size());
  ok &= Expect(boundaries.entry_count() == entries_before_invalid,
               "address-wrapping ranges bypass the memo table");
  const auto before_null = boundaries.stats().full_checks;
  const auto invalid = boundaries.Hash(nullptr, 0x81000, data.size());
  ok &= Expect(invalid.hash == 0 && invalid.bytes_hashed == 0 &&
                   boundaries.stats().full_checks == before_null,
               "null nonempty range is rejected without dereferencing");
  return ok;
}

bool TestBindingMemoInvalidation() {
  bool ok=true;
  fear_nx::deko::BindingMemo<uint32_t,18,4> memo;
  fear_nx::deko::BindingMemo<uint32_t,18,4>::Key key{};
  fear_nx::deko::BindingMemo<uint32_t,18,4>::Stamp stamp{1,0,1};
  ok &= Expect(!memo.Find(0,key,stamp),"uninitialized binding cannot hit epoch zero");
  memo.Store(0,key,stamp,99);
  ok &= Expect(memo.Find(0,key,stamp) && *memo.Find(0,key,stamp)==99,"identical binding key hits");
  for(size_t i=0;i<key.size();++i) {
    auto other=key;other[i]=1;
    ok &= Expect(!memo.Find(0,other,stamp),"fetch, sampler, dimension and sign differences cannot alias");
  }
  for(size_t i=0;i<stamp.size();++i) {
    auto changed=stamp;++changed[i];
    ok &= Expect(!memo.Find(0,key,changed),"epoch, guard miss and image retirement each invalidate a binding");
  }
  auto collision=key;collision[3]=1;memo.Store(4,collision,stamp,100);
  ok &= Expect(!memo.Find(0,key,stamp),"direct-mapped hash collision evicts without a false hit");
  ValidationCache validation;validation.SetEnabled(true);
  ok &= Expect(!validation.TextureRangeCanReuseBinding(0x10000,1024) &&
                   validation.TextureRangeCanReuseBinding(0x10000,2048),"small ranges stay exact per use");
  const auto before=validation.memo_epoch();validation.Revalidate();
  ok &= Expect(validation.memo_epoch()!=before,"guest progress invalidates binding leases");
  validation.SetEnabled(false);
  ok &= Expect(!validation.TextureRangeCanReuseBinding(0x10000,2048),"full-check mode cannot issue binding leases");
  validation.SetEnabled(true);
  ok &= Expect(!validation.TextureRangeCanReuseBinding(0xFFFFFFF0u,2048),"wrapping ranges cannot issue leases");
  return ok;
}
}  // namespace

int main() {
  bool ok = true;
  ok &= TestRawHashAndDanteFingerprint();
  ok &= TestTrainingQuotaIgnoresChangedSamplesAndPhaseSweep();
  ok &= TestGlobalGuardTrainingAndStop();
  ok &= TestGuardDetectsUnsampledMutation();
  ok &= TestQuarantineSaturationFallback();
  ok &= TestSampleChangeAndVolatileGeometry();
  ok &= TestAdaptiveTextureSchedule();
  ok &= TestKeyIsolationCapacityAndBoundaries();
  ok &= TestBindingMemoInvalidation();
  if (!ok) return 1;
  std::cout << "validation cache tests passed\n";
  return 0;
}
