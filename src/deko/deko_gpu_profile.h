// SPDX-License-Identifier: GPL-3.0-or-later
// Sparse asynchronous GPU-work timing for the deko3d renderer.
#pragma once

#include <rex/ui/deko3d/nucleo.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

namespace fear_nx::deko {

// Samples one of every sixteen submitted work slots. Each sampled slot gets
// two 16-byte timestamp reports in Nucleo's CPU-uncached query block. Results
// are read only after EmpezarTrabajo has waited for that slot's previous fence.
// This measures elapsed GPU timestamp time between the two markers for sampled
// submissions, including queue-idle gaps; it is not active-engine time or a
// full-frame timer. ReportCounter flushes tiled cache state, so the sparse
// sample still has some measurement overhead.
class GpuProfile {
 public:
  static constexpr uint64_t kSamplePeriod = 16;
  static constexpr uint32_t kReportBytes = 16;
  static constexpr uint32_t kPairBytes = 2 * kReportBytes;
  static constexpr uint32_t kMaxWorkSlots = 64;

  // Call after Nucleo::EmpezarTrabajo(work), when the selected slot's prior
  // fence is complete and its previous report pair can safely be read.
  void BeginWork(rex::deko3d::Nucleo& core, uint64_t work, bool enabled) noexcept {
    if (active_) {
      ++rejected_samples;
      active_ = false;
    }

    // A fixed modulo always sampled slot 3 of our four-slot ring. Periodic
    // helper/present work could therefore dominate all samples. Deterministic
    // jitter covers every slot and work size without extra fences/readbacks.
    uint64_t sample_key=work+0x9E3779B97F4A7C15ull;
    sample_key=(sample_key^(sample_key>>30))*0xBF58476D1CE4E5B9ull;
    sample_key=(sample_key^(sample_key>>27))*0x94D049BB133111EBull;
    sample_key^=sample_key>>31;
    const bool sample_this_work = sample_key % kSamplePeriod == 0;
    if (!core.TrabajoAbierto()) {
      if (enabled && sample_this_work) ++rejected_samples;
      return;
    }

    const uint32_t slot_count = core.config().ranuras;
    if (!slot_count || slot_count > kMaxWorkSlots) {
      RejectAllPending();
      if (enabled && sample_this_work) ++rejected_samples;
      return;
    }
    if (!HasReportCapacity(core, slot_count)) {
      for (uint32_t pending_slot = 0; pending_slot < slot_count; ++pending_slot) {
        RejectPending(pending_slot);
      }
      if (enabled && sample_this_work) ++rejected_samples;
      return;
    }

    const uint32_t slot = static_cast<uint32_t>(work % slot_count);
    if (!core.ConsultasCpu()) {
      RejectPending(slot);
      if (enabled && sample_this_work) ++rejected_samples;
      return;
    }
    CollectCompleted(core, slot);
    if (!enabled || !sample_this_work) return;
    if (core.ConsultasGpu() == DK_GPU_ADDR_INVALID) {
      ++rejected_samples;
      return;
    }

    const uint32_t offset = slot * kPairBytes;
    std::memset(core.ConsultasCpu() + offset, 0xFF, kPairBytes);
    dkCmdBufReportCounter(core.comandos(), DkCounter_Timestamp,
                          core.ConsultasGpu() + offset);
    active_slot_ = slot;
    active_work_ = work;
    active_ = true;
  }

  // Call immediately before Nucleo::TerminarTrabajo(). The fence submitted
  // by Nucleo then guarantees both reports are complete before the next reuse.
  void EndWork(rex::deko3d::Nucleo& core, uint64_t work,uint64_t draws=0) noexcept {
    if (!active_) return;
    if (active_work_ != work || !core.TrabajoAbierto() || !core.ConsultasCpu() ||
        core.ConsultasGpu() == DK_GPU_ADDR_INVALID) {
      ++rejected_samples;
      active_ = false;
      return;
    }

    const uint32_t offset = active_slot_ * kPairBytes + kReportBytes;
    dkCmdBufReportCounter(core.comandos(), DkCounter_Timestamp,
                          core.ConsultasGpu() + offset);
    slots_[active_slot_].pending = true;
    slots_[active_slot_].draws = draws;
    active_ = false;
  }

  // Cumulative completed samples. gpu_submissions counts sampled work items,
  // not every renderer submission; gpu_ticks preserves the raw device units.
  uint64_t gpu_ns = 0;
  uint64_t gpu_ticks = 0;
  uint64_t gpu_submissions = 0;
  uint64_t rejected_samples = 0;
  uint64_t sampled_draws = 0,draw_work_ns = 0,draw_work_samples = 0;

 private:
  struct SlotState {
    bool pending = false;
    uint64_t draws = 0;
  };

  static bool HasReportCapacity(const rex::deko3d::Nucleo& core,
                                uint32_t slot_count) noexcept {
    const uint64_t allocated_bytes = uint64_t(core.config().kb_consultas) * 1024;
    return allocated_bytes >= uint64_t(slot_count) * kPairBytes;
  }

  static uint64_t TimestampToNs(uint64_t ticks) noexcept {
    // Exact deko3d conversion, (ticks * 625) / 384, without overflowing the
    // multiplication for ordinary accumulated sample deltas.
    return (ticks / 384) * 625 + ((ticks % 384) * 625) / 384;
  }

  void CollectCompleted(rex::deko3d::Nucleo& core, uint32_t slot) noexcept {
    SlotState& state = slots_[slot];
    if (!state.pending) return;
    state.pending = false;

    const uint8_t* report = core.ConsultasCpu() + slot * kPairBytes;
    uint64_t start = 0;
    uint64_t end = 0;
    // For DkCounter_Timestamp the second 64-bit word of each 16-byte report
    // is its GPU timestamp (deko3d Primer; also documented in Nucleo).
    std::memcpy(&start, report + sizeof(uint64_t), sizeof(start));
    std::memcpy(&end, report + kReportBytes + sizeof(uint64_t), sizeof(end));
    if (start == std::numeric_limits<uint64_t>::max() ||
        end == std::numeric_limits<uint64_t>::max() || (start == 0 && end == 0) ||
        end < start) {
      ++rejected_samples;
      return;
    }

    const uint64_t delta = end - start;
    gpu_ticks += delta;
    gpu_ns += TimestampToNs(delta);
    ++gpu_submissions;
    sampled_draws+=state.draws;
    if(state.draws>=64){draw_work_ns+=TimestampToNs(delta);++draw_work_samples;}
  }

  void RejectPending(uint32_t slot) noexcept {
    if (slots_[slot].pending) {
      slots_[slot].pending = false;
      ++rejected_samples;
    }
  }

  void RejectAllPending() noexcept {
    for (uint32_t slot = 0; slot < kMaxWorkSlots; ++slot) RejectPending(slot);
  }

  std::array<SlotState, kMaxWorkSlots> slots_{};
  uint64_t active_work_ = 0;
  uint32_t active_slot_ = 0;
  bool active_ = false;
};

}  // namespace fear_nx::deko
