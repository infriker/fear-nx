// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "deko_vertex_range.h"
#include <array>
#include <vector>
#include <rex/graphics/pipeline/shader/shader.h>

namespace fear_nx::deko {

struct VertexFetchRangePlan {
  bool valid = false;
  std::vector<FetchAccess> accesses;
};
using VertexRangePlan = std::array<VertexFetchRangePlan, 96>;

// Retaining the fetch base (copying a prefix) avoids changing guest vertex IDs
// or the shared-memory shader ABI. Every full fetch must capture the incoming
// index before a guest instruction modifies it. A subsequent mini-fetch uses
// that captured address, so writes after the full fetch do not invalidate it.
inline VertexRangePlan BuildVertexRangePlan(const rex::graphics::Shader& shader) {
  using namespace rex::graphics;
  VertexRangePlan plan{};
  if (!shader.vertex_fetch_indices_unmodified() ||
      !shader.vertex_fetch_addresses_linear() ||
      shader.uses_register_dynamic_addressing()) return plan;
  std::array<bool, 96> rejected{};
  for (const auto& binding : shader.vertex_bindings()) {
    for (const auto& attribute : binding.attributes) {
      const auto& fetch = attribute.fetch_instr;
      const uint32_t words = xenos::GetVertexFormatNeededWords(
          fetch.attributes.data_format, fetch.result.GetUsedResultComponents());
      if (!words) continue;
      // The SDK's legacy binding grouping uses raw mini-fetch fields, but
      // the translator inherits its actual constant from the preceding full
      // fetch. Group by the parsed operand to include every mini access.
      if (fetch.operand_count < 2 ||
          fetch.operands[1].storage_source != InstructionStorageSource::kVertexFetchConstant ||
          fetch.operands[1].storage_index >= plan.size()) return {};
      const uint32_t fetch_slot = fetch.operands[1].storage_index;
      auto& slot = plan[fetch_slot];
      const auto& index = fetch.operands[0];
      if (index.storage_source != InstructionStorageSource::kRegister ||
          index.storage_addressing_mode != InstructionStorageAddressingMode::kAbsolute ||
          index.storage_index != 0 || index.component_count != 1 ||
          index.components[0] != SwizzleSource::kX || index.is_negated) {
        rejected[fetch_slot] = true;
        continue;
      }
      slot.accesses.push_back({fetch.attributes.stride, words,
                               fetch.attributes.offset, fetch.attributes.is_index_rounded});
    }
  }
  for (size_t i = 0; i < plan.size(); ++i)
    plan[i].valid = !rejected[i] && !plan[i].accesses.empty();
  return plan;
}

}  // namespace fear_nx::deko
