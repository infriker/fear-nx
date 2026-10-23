// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../src/deko/deko_vertex_cache.h"

#include <cstdint>
#include <iostream>

namespace {

#define CHECK(expression)                                                        \
  do {                                                                           \
    if (!(expression)) {                                                         \
      std::cerr << __func__ << ": check failed at line " << __LINE__ << ": "    \
                << #expression << '\n';                                          \
      return false;                                                              \
    }                                                                            \
  } while (false)

using fear_nx::deko::PersistentVertexCache;

PersistentVertexCache::Result ObserveTwice(PersistentVertexCache& cache, uint32_t address,
                                           uint32_t bytes, uint64_t hash) {
  cache.FindOrAllocate(address, bytes, hash);
  return cache.FindOrAllocate(address, bytes, hash);
}

bool AdmissionThenImmutableHit() {
  PersistentVertexCache cache(4 * PersistentVertexCache::kPageBytes);
  cache.BeginWork(0, 4);
  auto first = cache.FindOrAllocate(0x123400, 1536, 0xABCDEF);
  CHECK(!first.valid && !first.hit);

  const auto admitted = cache.FindOrAllocate(0x123400, 1536, 0xABCDEF);
  CHECK(admitted.valid && !admitted.hit);
  CHECK(admitted.offset == PersistentVertexCache::kReservedPrefixBytes);
  CHECK(admitted.bytes_to_copy == 1536);
  CHECK(admitted.offset % PersistentVertexCache::kPageBytes == 0);

  const auto hit = cache.FindOrAllocate(0x123400, 1536, 0xABCDEF);
  CHECK(hit.valid && hit.hit && hit.offset == admitted.offset);
  CHECK(hit.bytes_to_copy == 0);
  CHECK(cache.stats().hits == 1);
  CHECK(cache.stats().copied_bytes == 1536);
  return true;
}

bool SlotIsolationAndFenceSafeReuse() {
  PersistentVertexCache cache(3 * PersistentVertexCache::kPageBytes);
  cache.BeginWork(0, 4);
  const auto slot0 = ObserveTwice(cache, 0x2000, 128, 0x55);
  CHECK(slot0.valid && !slot0.hit);
  CHECK(cache.active_slot() == 0);

  cache.BeginWork(1, 4);
  CHECK(cache.active_slot() == 1);
  const auto slot1_first = cache.FindOrAllocate(0x2000, 128, 0x55);
  CHECK(!slot1_first.valid);  // Slot 1 has not been populated.
  const auto slot1 = cache.FindOrAllocate(0x2000, 128, 0x55);
  CHECK(slot1.valid && !slot1.hit);
  CHECK(slot1.offset == slot0.offset);  // Equal slot-relative offsets, distinct GPU slices.

  // The caller invokes BeginWork(4) only after Nucleo has waited for slot 0's
  // prior fence. Its immutable payload and metadata can then be reused.
  cache.BeginWork(4, 4);
  CHECK(cache.active_slot() == 0);
  const auto slot0_again = cache.FindOrAllocate(0x2000, 128, 0x55);
  CHECK(slot0_again.valid && slot0_again.hit && slot0_again.offset == slot0.offset);
  CHECK(cache.stats().copied_bytes == 256);
  return true;
}

bool CurrentWorkEntriesAreNeverEvicted() {
  // One usable page after the reserved header page.
  PersistentVertexCache cache(2 * PersistentVertexCache::kPageBytes);
  cache.BeginWork(10, 1);
  const auto live = ObserveTwice(cache, 0x1000, 256, 1);
  CHECK(live.valid && !live.hit);

  cache.FindOrAllocate(0x2000, 256, 2);
  const auto cannot_fit = cache.FindOrAllocate(0x2000, 256, 2);
  CHECK(!cannot_fit.valid);
  CHECK(cache.stats().capacity_misses == 1);
  CHECK(cache.stats().evictions == 0);

  const auto still_live = cache.FindOrAllocate(0x1000, 256, 1);
  CHECK(still_live.valid && still_live.hit && still_live.offset == live.offset);
  return true;
}

bool ChangedContentGetsNewCopyAndOldCopyRetiresSafely() {
  PersistentVertexCache cache(3 * PersistentVertexCache::kPageBytes);
  cache.BeginWork(0, 1);
  const auto old_copy = ObserveTwice(cache, 0x3000, 3000, 0xA);
  CHECK(old_copy.valid && !old_copy.hit);

  // A changed digest is a different key. Keep both copies this work because
  // the old one may already be referenced by an earlier draw.
  const auto new_copy = ObserveTwice(cache, 0x3000, 3000, 0xB);
  CHECK(new_copy.valid && !new_copy.hit);
  CHECK(new_copy.offset != old_copy.offset);
  CHECK(cache.stats().evictions == 0);

  // After the slot fence is waited at BeginWork, the least recently used old
  // digest can be retired to make room for a third version.
  cache.BeginWork(1, 1);
  const auto new_hit = cache.FindOrAllocate(0x3000, 3000, 0xB);
  CHECK(new_hit.valid && new_hit.hit && new_hit.offset == new_copy.offset);
  const auto third_version = ObserveTwice(cache, 0x3000, 3000, 0xC);
  CHECK(third_version.valid && !third_version.hit);
  CHECK(cache.stats().evictions == 1);
  const auto third_hit = cache.FindOrAllocate(0x3000, 3000, 0xC);
  CHECK(third_hit.valid && third_hit.hit && third_hit.offset == third_version.offset);
  return true;
}

bool DiscardActiveSlotPreservesOtherInFlightSlot() {
  PersistentVertexCache cache(3 * PersistentVertexCache::kPageBytes);
  cache.BeginWork(0, 2);
  const auto slot0 = ObserveTwice(cache, 0x4000, 128, 0x40);
  CHECK(slot0.valid && !slot0.hit);

  cache.BeginWork(1, 2);
  const auto slot1 = ObserveTwice(cache, 0x5000, 256, 0x50);
  CHECK(slot1.valid && !slot1.hit);

  // The caller has waited for slot 0's fence before BeginWork(2). The normal
  // upload fallback will overwrite its prefix, so only slot 0 is discarded.
  cache.BeginWork(2, 2);
  const auto before_discard = cache.stats();
  cache.DiscardActiveSlot();
  CHECK(cache.active_slot() == 0);
  CHECK(cache.stats().hits == before_discard.hits);
  CHECK(cache.stats().misses == before_discard.misses);
  CHECK(cache.stats().copied_bytes == before_discard.copied_bytes);
  CHECK(cache.stats().evictions == before_discard.evictions);
  CHECK(cache.stats().capacity_misses == before_discard.capacity_misses);
  CHECK(cache.stats().candidate_evictions == before_discard.candidate_evictions);

  // Do not allocate into a prefix that the ordinary upload ring now owns.
  CHECK(!cache.FindOrAllocate(0x4000, 128, 0x40).valid);

  // Slot 1 may still be in flight and its immutable payload remains available.
  cache.BeginWork(3, 2);
  const auto slot1_hit = cache.FindOrAllocate(0x5000, 256, 0x50);
  CHECK(slot1_hit.valid && slot1_hit.hit && slot1_hit.offset == slot1.offset);

  // A later fence-safe reuse re-enables slot 0 and requires fresh admission.
  cache.BeginWork(4, 2);
  CHECK(!cache.FindOrAllocate(0x4000, 128, 0x40).valid);
  const auto slot0_reloaded = cache.FindOrAllocate(0x4000, 128, 0x40);
  CHECK(slot0_reloaded.valid && !slot0_reloaded.hit);
  CHECK(slot0_reloaded.offset == slot0.offset);
  return true;
}

bool ZeroOversizeAndInvalidWorkFallback() {
  PersistentVertexCache cache(2 * PersistentVertexCache::kPageBytes);
  CHECK(!cache.FindOrAllocate(1, 16, 0).valid);
  cache.BeginWork(0, 2);
  CHECK(!cache.FindOrAllocate(1, 0, 0).valid);
  CHECK(!cache.FindOrAllocate(1, cache.capacity_bytes(), 0).valid);
  CHECK(cache.stats().capacity_misses == 1);
  return true;
}

bool CandidateLruSaturationAndTouch() {
  // One usable page; a pinned entry prevents candidate admission this work.
  PersistentVertexCache cache(2 * PersistentVertexCache::kPageBytes);
  cache.BeginWork(0, 1);
  CHECK(ObserveTwice(cache, 0x1000, 16, 1).valid);
  for (uint32_t i = 0; i < 64; ++i)
    CHECK(!cache.FindOrAllocate(0x2000 + i * 16, 16, i + 2).valid);
  // A failed admission still touches the oldest candidate, keeping it alive.
  CHECK(!cache.FindOrAllocate(0x2000, 16, 2).valid);
  CHECK(!cache.FindOrAllocate(0x9000, 16, 1000).valid);
  CHECK(cache.stats().candidate_evictions == 1);
  cache.BeginWork(1, 1);
  // Entry 1, not the touched entry 0, was evicted from the candidate queue.
  CHECK(!cache.FindOrAllocate(0x2010, 16, 3).valid);
  const auto touched = cache.FindOrAllocate(0x2000, 16, 2);
  CHECK(touched.valid && !touched.hit);
  CHECK(cache.FindOrAllocate(0x2000, 16, 2).hit);
  cache.DiscardActiveSlot();
  cache.BeginWork(2, 1);
  CHECK(!cache.FindOrAllocate(0x2000, 16, 2).valid);
  CHECK(cache.FindOrAllocate(0x2000, 16, 2).valid);
  return true;
}

bool CandidateChurnAtProductionCapacity() {
  PersistentVertexCache cache;
  cache.BeginWork(0, 4);
  for (uint32_t i = 0; i < 50000; ++i)
    CHECK(!cache.FindOrAllocate(0x100000 + i * 16, 16, i).valid);
  CHECK(cache.stats().candidate_evictions == 50000 - 8192);
  const auto recent = cache.FindOrAllocate(0x100000 + 49999 * 16, 16, 49999);
  CHECK(recent.valid);
  CHECK(!cache.FindOrAllocate(0x100000, 16, 0).valid);
  // Reconfiguration must discard the queue and its iterators together.
  cache.BeginWork(0, 2);
  CHECK(!cache.FindOrAllocate(0x100000 + 49999 * 16, 16, 49999).valid);
  return true;
}

}  // namespace

int main() {
  const bool ok = AdmissionThenImmutableHit() && SlotIsolationAndFenceSafeReuse() &&
                  CurrentWorkEntriesAreNeverEvicted() &&
                  ChangedContentGetsNewCopyAndOldCopyRetiresSafely() &&
                  DiscardActiveSlotPreservesOtherInFlightSlot() &&
                  ZeroOversizeAndInvalidWorkFallback() &&
                  CandidateLruSaturationAndTouch() && CandidateChurnAtProductionCapacity();
  if (!ok) return 1;
  std::cout << "Passed persistent vertex cache CPU tests.\n";
  return 0;
}
