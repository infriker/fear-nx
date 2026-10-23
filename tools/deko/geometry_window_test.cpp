// SPDX-License-Identifier: GPL-3.0-or-later
#include "deko/deko_validation_cache.h"
#include "deko/deko_vertex_cache.h"
#include "deko/deko_vertex_range.h"

#include <array>
#include <cstring>
#include <iostream>
#include <map>
#include <vector>

namespace {
using namespace fear_nx::deko;
#define CHECK(expression) do { if (!(expression)) { \
  std::cerr << __func__ << ':' << __LINE__ << ": " #expression "\n"; return false; \
} } while (false)

VertexByteRange Range(uint32_t first, uint32_t end) {
  return {first, end, end, end - first, true, true, false};
}

bool WindowCoverageAndFallbacks() {
  for (uint32_t original : {65536u, 65540u, 196608u, 1048564u, 67108864u}) {
    uint32_t random = 0x12345678;
    for (uint32_t i = 0; i < 20000; ++i) {
      random = random * 1664525 + 1013904223;
      const uint32_t first = (random % (original / 4)) * 4;
      random = random * 1664525 + 1013904223;
      const uint32_t end = first + (1 + random % ((original - first) / 4)) * 4;
      const auto window = SelectVertexWindow(original, Range(first, end));
      const auto legacy = SelectVertexWindowLegacy(original, Range(first, end));
      CHECK(window.bytes <= legacy.bytes);
      CHECK(window.bytes < uint64_t(end - first) + 2 * 65536);
      CHECK(window.first_byte <= first);
      CHECK(uint64_t(window.first_byte) + window.bytes >= end);
      CHECK(uint64_t(window.first_byte) + window.bytes <= original);
      CHECK(!(window.first_byte & 3) && !(window.bytes & 3));
    }
  }
  // Both sides of an alignment boundary must be present, including at the
  // descriptor tail. Invalid proof must never clip a fetch.
  auto crossing = SelectVertexWindow(1 << 20, Range(65532, 65540));
  CHECK(crossing.first_byte == 0 && crossing.bytes == 131072);
  auto tail = SelectVertexWindow(196612, Range(196608, 196612));
  CHECK(tail.first_byte == 196608 && tail.bytes == 4);
  auto invalid = SelectVertexWindow(1 << 20, {});
  CHECK(invalid.first_byte == 0 && invalid.bytes == 1 << 20);
  auto outside = SelectVertexWindow(65536, Range(65532, 65540));
  CHECK(outside.first_byte == 0 && outside.bytes == 65536);
  return true;
}

bool LargeBoundaryDoesNotInflateSmallDraws() {
  constexpr uint32_t original=16u<<20;
  for(uint32_t boundary=1u<<20;boundary<original;boundary*=2) {
    for(uint32_t movement=0;movement<4096;movement+=4) {
      const auto range=Range(boundary-8192+movement,boundary+4+movement);
      const auto old=SelectVertexWindowLegacy(original,range);
      const auto now=SelectVertexWindow(original,range);
      CHECK(old.bytes==boundary*2);
      CHECK(now.first_byte==boundary-65536&&now.bytes==131072);
      // Precise changing min/max values still select one cache key.
    }
  }
  std::cout << "Boundary example: 8 KiB used across 1 MiB, window 2 MiB -> 128 KiB\n";
  return true;
}

bool UploadedWordsMatchOriginalShaderAddresses() {
  constexpr uint32_t original = 1 << 20, upload_capacity = 2 << 20;
  std::vector<uint32_t> guest(original / 4), upload(upload_capacity / 4, 0xDEADBEEF);
  for (size_t i = 0; i < guest.size(); ++i) guest[i] = uint32_t(i * 37 + 17);
  // Signed offsets, sparse component masks and a secondary full/mini-fetch
  // access using the same constant all contribute to the proven window.
  const FetchAccess accesses[] = {{8, 0xB, -5, false}, {8, 0x3, 6, true}};
  IndexBounds indices;
  indices.Include(20000, 0);
  indices.Include(20700, 0);
  const auto range = UsedVertexPrefix(original, indices, accesses);
  CHECK(range.valid);
  const auto window = SelectVertexWindow(original, range);
  CHECK(window.first_byte > 0 && window.bytes < original);
  PersistentVertexCache cache(upload_capacity);
  cache.BeginWork(0, 4);
  cache.FindOrAllocate(window.first_byte, window.bytes, 1, window.first_byte);
  auto copy = cache.FindOrAllocate(window.first_byte, window.bytes, 1, window.first_byte);
  CHECK(copy.valid && copy.offset >= window.first_byte);
  std::memcpy(reinterpret_cast<uint8_t*>(upload.data()) + copy.offset,
              reinterpret_cast<uint8_t*>(guest.data()) + window.first_byte, window.bytes);
  uint32_t base = 0;
  CHECK(RebaseVertexWindow(window, copy.offset, upload_capacity, base));
  for (uint32_t index = indices.min(); index <= indices.max(); ++index) {
    for (const auto& access : accesses) {
      for (uint32_t component = 0; component < 32; ++component) {
        if (!(access.needed_words_mask & (1u << component))) continue;
        const uint32_t guest_word = index * access.stride_words + access.offset_words + component;
        // Same logical shift and integer address addition as the translated
        // shader, with the original vertex index unchanged.
        const uint32_t uploaded_word = (base >> 2) + guest_word;
        CHECK(uploaded_word < upload.size());
        CHECK(upload[uploaded_word] == guest[guest_word]);
      }
    }
  }
  CHECK(!RebaseVertexWindow(window, window.first_byte - 4, upload_capacity, base));
  CHECK(!RebaseVertexWindow(window, upload_capacity - window.bytes + 4, upload_capacity, base));
  CHECK(!RebaseVertexWindow(window, UINT64_MAX, upload_capacity, base));
  CHECK(!RebaseVertexWindow(window, copy.offset + 1, upload_capacity, base));
  // Transient placement may leave an uncopied gap before a high-index window.
  CHECK(RebaseVertexWindow(window, window.first_byte, upload_capacity, base) && base == 0);
  return true;
}

bool PlacementConstraintPreservesInFlightCopies() {
  PersistentVertexCache cache(16 * 4096);
  cache.BeginWork(0, 4);
  cache.FindOrAllocate(0x1000, 4096, 1);
  auto old = cache.FindOrAllocate(0x1000, 4096, 1);
  CHECK(old.valid && old.offset == 4096);
  // Same physical bytes through a different descriptor base cannot reuse a
  // too-low address or replace a copy already referenced by queued commands.
  CHECK(!cache.FindOrAllocate(0x1000, 4096, 1, 8192).valid);
  auto retained = cache.FindOrAllocate(0x1000, 4096, 1);
  CHECK(retained.hit && retained.offset == old.offset);
  cache.FindOrAllocate(0x2000, 4096, 2, 8193);
  auto aligned = cache.FindOrAllocate(0x2000, 4096, 2, 8193);
  CHECK(aligned.valid && aligned.offset == 12288);
  CHECK(!cache.FindOrAllocate(0x3000, 4096, 3, 16 * 4096).valid);
  CHECK(!cache.FindOrAllocate(0x3000, 4096, 3, UINT32_MAX).valid);
  cache.BeginWork(4, 4);  // Selected slot's fence has passed.
  CHECK(cache.FindOrAllocate(0x2000, 4096, 2, 8193).hit);
  return true;
}

struct Traffic { uint64_t copies = 0, local_hits = 0, persistent_hits = 0, full_checks = 0; };

Traffic SimulateNeighboringDraws(bool stable_windows) {
  constexpr uint32_t address = 0x100000, original = 1 << 20;
  std::vector<uint8_t> guest(original, 0x5A);
  ValidationCache validation;
  validation.SetEnabled(true);
  PersistentVertexCache persistent;
  Traffic traffic;
  for (uint32_t work = 0; work < 12; ++work) {
    persistent.BeginWork(work, 4);
    validation.BeginFrame(work + 1);
    std::map<std::array<uint64_t, 3>, bool> local;
    for (uint32_t draw = 0; draw < 128; ++draw) {
      IndexBounds indices;
      indices.Include(16384 + draw * 8, 0);
      indices.Include(16384 + draw * 8 + 3, 0);
      const FetchAccess access{8, 0xF, 0, false};
      const auto range = UsedVertexPrefix(original, indices, {&access, 1});
      const VertexWindow window = stable_windows ? SelectVertexWindow(original, range)
          : VertexWindow{0, uint32_t(range.prefix_bytes)};
      const uint32_t physical = address + window.first_byte;
      const auto checked = validation.Hash(guest.data() + window.first_byte, physical, window.bytes);
      const std::array<uint64_t, 3> key{physical, window.bytes, checked.hash};
      if (local.contains(key)) { ++traffic.local_hits; continue; }
      const auto copy = persistent.FindOrAllocate(physical, window.bytes, checked.hash, window.first_byte);
      if (copy.hit) ++traffic.persistent_hits;
      else traffic.copies += window.bytes;
      local.emplace(key, true);
    }
  }
  traffic.full_checks = validation.stats().full_checks;
  return traffic;
}

bool RegressionRepeatedDrawsDoNotFragmentCopies() {
  const auto old = SimulateNeighboringDraws(false);
  const auto fixed = SimulateNeighboringDraws(true);
  CHECK(old.local_hits == 0);
  CHECK(fixed.local_hits == 12 * 127);
  CHECK(fixed.persistent_hits == 4);  // Four slots, first seen / admitted / hit.
  CHECK(fixed.copies == 8 * 65536);
  CHECK(old.copies > fixed.copies * 100);
  CHECK(old.full_checks > fixed.full_checks * 20);
  std::cout << "Repeated-draw regression, synthetic workload: copied bytes "
            << old.copies << " -> " << fixed.copies << "; full checks "
            << old.full_checks << " -> " << fixed.full_checks << '\n';
  return true;
}

bool ChangedWindowDoesNotReuseStaleCopy() {
  std::vector<uint8_t> guest(1 << 20, 0x5A);
  const auto window = SelectVertexWindow(uint32_t(guest.size()), Range(700000, 700128));
  ValidationCache exact;  // Disabled sampling: every call hashes all window bytes.
  PersistentVertexCache cache;
  cache.BeginWork(0, 4);
  const uint32_t address = 0x100000 + window.first_byte;
  auto hash = [&] { return exact.Hash(guest.data() + window.first_byte, address, window.bytes).hash; };
  const auto before = hash();
  cache.FindOrAllocate(address, window.bytes, before, window.first_byte);
  auto old = cache.FindOrAllocate(address, window.bytes, before, window.first_byte);
  CHECK(old.valid);
  guest[0] ^= 1;  // Outside the proven window: does not invalidate this slice.
  CHECK(hash() == before);
  guest[700032] ^= 1;
  const auto after = hash();
  CHECK(after != before);
  CHECK(!cache.FindOrAllocate(address, window.bytes, after, window.first_byte).valid);
  auto changed = cache.FindOrAllocate(address, window.bytes, after, window.first_byte);
  CHECK(changed.valid && !changed.hit && changed.offset != old.offset);
  CHECK(cache.FindOrAllocate(address, window.bytes, before, window.first_byte).offset == old.offset);
  return true;
}
}  // namespace

int main() {
  if (!(WindowCoverageAndFallbacks() && LargeBoundaryDoesNotInflateSmallDraws() && UploadedWordsMatchOriginalShaderAddresses() &&
        PlacementConstraintPreservesInFlightCopies() && RegressionRepeatedDrawsDoNotFragmentCopies() &&
        ChangedWindowDoesNotReuseStaleCopy())) return 1;
  std::cout << "Passed geometry window regression, address mapping and mutation tests.\n";
}
