// Host-side proof and fetch-plan coverage for real XESH vertex shaders.
#include <bit>
#include <array>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <utility>
#include <vector>

#include <xxhash.h>

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/string/buffer.h>

#include "deko/deko_vertex_range_shader.h"

namespace fs = std::filesystem;
namespace {

constexpr uint32_t kXeshMagic = 0x48534558;  // "XESH" in little-endian file bytes.
constexpr uint32_t kXeshVersion = 0x20201219;
constexpr uint32_t kMaxShaderWords = 1u << 20;
using Instruction = std::array<uint32_t, 3>;

struct Record {
  uint64_t hash = 0;
  rex::graphics::xenos::ShaderType type = rex::graphics::xenos::ShaderType::kVertex;
  std::vector<uint32_t> words;
};

bool ReadExact(std::ifstream& input, void* destination, size_t bytes) {
  input.read(static_cast<char*>(destination), static_cast<std::streamsize>(bytes));
  return input.good();
}

bool LoadXesh(const fs::path& path, std::vector<Record>& records, std::string& error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "cannot open XESH cache: " + path.string();
    return false;
  }

  uint32_t magic = 0;
  uint32_t swapped_version = 0;
  if (!ReadExact(input, &magic, sizeof(magic)) ||
      !ReadExact(input, &swapped_version, sizeof(swapped_version))) {
    error = "truncated XESH header";
    return false;
  }
  if (magic != kXeshMagic || std::byteswap(swapped_version) != kXeshVersion) {
    error = "unexpected XESH magic or version";
    return false;
  }

  for (;;) {
    uint64_t hash = 0;
    input.read(reinterpret_cast<char*>(&hash), sizeof(hash));
    if (input.eof() && input.gcount() == 0) break;
    if (!input) {
      error = "truncated XESH record header";
      return false;
    }

    uint32_t packed_count_and_type = 0;
    if (!ReadExact(input, &packed_count_and_type, sizeof(packed_count_and_type))) {
      error = "truncated XESH word count";
      return false;
    }
    const uint32_t word_count = packed_count_and_type & 0x7fffffffu;
    if (word_count == 0 || word_count > kMaxShaderWords) {
      error = "invalid XESH microcode word count";
      return false;
    }

    Record record;
    record.hash = hash;
    record.type = (packed_count_and_type >> 31) != 0
                      ? rex::graphics::xenos::ShaderType::kPixel
                      : rex::graphics::xenos::ShaderType::kVertex;
    record.words.resize(word_count);
    const size_t bytes = size_t(word_count) * sizeof(uint32_t);
    if (!ReadExact(input, record.words.data(), bytes)) {
      error = "truncated XESH microcode";
      return false;
    }
    if (XXH3_64bits(record.words.data(), bytes) != record.hash) {
      error = "XESH microcode hash mismatch";
      return false;
    }
    records.push_back(std::move(record));
  }

  if (records.empty()) {
    error = "XESH cache is empty";
    return false;
  }
  return true;
}

std::vector<uint32_t> MakeProgram(const std::vector<Instruction>& instructions,
                                  uint32_t sequence, uint32_t cf_opcode = 2) {
  if (instructions.size() > 7) {
    throw std::runtime_error("synthetic program instruction count is out of range");
  }
  std::vector<uint32_t> words(3 + instructions.size() * 3, 0);
  // One CF instruction at address 1; the second half of the pair is a nop.
  words[0] = 1u | (uint32_t(instructions.size()) << 12) | (sequence << 16);
  words[1] = cf_opcode << 12;
  for (size_t i = 0; i < instructions.size(); ++i) {
    std::copy(instructions[i].begin(), instructions[i].end(), words.begin() + 3 + i * 3);
  }
  return words;
}

Instruction MakeAluWrite(uint32_t destination, bool relative, uint32_t write_mask) {
  // RETAIN_PREV scalar opcode is inert without a scalar destination mask.
  const uint32_t word0 = (destination & 0x3fu) | (uint32_t(relative) << 6) |
                         ((write_mask & 0xfu) << 16) | (50u << 26);
  return {word0, 0, 0};  // vector ADD with zero write mask unless requested
}

Instruction MakeVertexFetch(uint32_t fetch_slot, uint32_t destination,
                            bool mini, uint32_t write_swizzle = 0xff8u,
                            bool destination_relative = false,
                            bool predicated = false) {
  // vfetch_full slot = const_index * 3 + const_index_sel. A mini fetch's
  // encoded slot is deliberately zero; the translator must inherit the slot.
  const uint32_t const_index = mini ? 0u : fetch_slot / 3;
  const uint32_t const_select = mini ? 0u : fetch_slot % 3;
  const uint32_t word0 = ((destination & 0x3fu) << 12) |
                         (uint32_t(destination_relative) << 18) | (1u << 19) |
                         (const_index << 20) | (const_select << 25);
  const uint32_t word1 = (write_swizzle & 0xfffu) | (36u << 16) |
                         (uint32_t(mini) << 30) | (uint32_t(predicated) << 31);  // k_32_FLOAT
  const uint32_t word2 = mini ? (2u << 8) : 4u;  // mini offset 2, full stride 4
  return {word0, word1, word2};
}

bool AnalyzeSynthetic(const std::vector<uint32_t>& words, bool& index_unmodified,
                      bool& fetch_indices_unmodified, bool& addresses_linear,
                      fear_nx::deko::VertexRangePlan* plan = nullptr) {
  const uint64_t hash = XXH3_64bits(words.data(), words.size() * sizeof(uint32_t));
  rex::graphics::SpirvShader shader(rex::graphics::xenos::ShaderType::kVertex, hash,
                                    words.data(), words.size(), std::endian::native);
  rex::string::StringBuffer disassembly;
  shader.AnalyzeUcode(disassembly);
  index_unmodified = shader.vertex_index_register_unmodified();
  fetch_indices_unmodified = shader.vertex_fetch_indices_unmodified();
  addresses_linear = shader.vertex_fetch_addresses_linear();
  if (plan) *plan = fear_nx::deko::BuildVertexRangePlan(shader);
  return true;
}

bool CheckSynthetic(bool condition, std::string_view name) {
  if (!condition) std::cerr << "Synthetic proof regression failed: " << name << '\n';
  return condition;
}

bool RunSyntheticCases() {
  using rex::graphics::ucode::ControlFlowOpcode;
  bool index_unmodified = false;
  bool fetch_indices_unmodified = false;
  bool addresses_linear = false;
  bool passed = true;

  auto analyze_alu = [&](Instruction alu) {
    const auto words = MakeProgram({alu}, 0);
    AnalyzeSynthetic(words, index_unmodified, fetch_indices_unmodified, addresses_linear);
  };

  analyze_alu(MakeAluWrite(1, false, 0));
  passed &= CheckSynthetic(index_unmodified, "no-write ALU preserves r0.x");
  analyze_alu(MakeAluWrite(1, false, 1));
  passed &= CheckSynthetic(index_unmodified, "absolute r1.x write preserves r0.x");
  analyze_alu(MakeAluWrite(1, false, 2));
  passed &= CheckSynthetic(index_unmodified, "r1.y write preserves r0.x");
  analyze_alu(MakeAluWrite(0, false, 1));
  passed &= CheckSynthetic(!index_unmodified, "absolute r0.x write invalidates proof");
  analyze_alu(MakeAluWrite(7, true, 1));
  passed &= CheckSynthetic(!index_unmodified, "relative ALU x write may alias r0");

  {
    const auto words = MakeProgram({MakeVertexFetch(5, 1, false)}, 1);
    AnalyzeSynthetic(words, index_unmodified, fetch_indices_unmodified, addresses_linear);
    passed &= CheckSynthetic(index_unmodified && fetch_indices_unmodified && addresses_linear,
                             "unpredicated full fetch proves linear address");
  }
  {
    const auto words = MakeProgram({MakeVertexFetch(5, 1, false, 0xff8u, false, true)}, 1);
    AnalyzeSynthetic(words, index_unmodified, fetch_indices_unmodified, addresses_linear);
    passed &= CheckSynthetic(index_unmodified && fetch_indices_unmodified && !addresses_linear,
                             "predicated full fetch rejects linear address proof");
  }
  {
    const auto words = MakeProgram({MakeVertexFetch(5, 0, false)}, 1);
    AnalyzeSynthetic(words, index_unmodified, fetch_indices_unmodified, addresses_linear);
    passed &= CheckSynthetic(!index_unmodified && fetch_indices_unmodified,
                             "full fetch captures r0.x before writing its result");
  }
  {
    const auto words = MakeProgram({MakeVertexFetch(5, 7, false, 0xff8u, true)}, 1);
    AnalyzeSynthetic(words, index_unmodified, fetch_indices_unmodified, addresses_linear);
    passed &= CheckSynthetic(!index_unmodified && fetch_indices_unmodified,
                             "relative fetch result follows its captured index");
  }
  {
    const auto words = MakeProgram({MakeVertexFetch(0, 1, true, 0xfffu)}, 1);
    AnalyzeSynthetic(words, index_unmodified, fetch_indices_unmodified, addresses_linear);
    passed &= CheckSynthetic(!index_unmodified && !fetch_indices_unmodified && !addresses_linear,
                             "initial mini fetch is ambiguous");
  }
  {
    auto words = MakeProgram({}, 0, uint32_t(ControlFlowOpcode::kCondExecEnd));
    AnalyzeSynthetic(words, index_unmodified, fetch_indices_unmodified, addresses_linear);
    passed &= CheckSynthetic(!addresses_linear,
                             "conditional exec rejects mini address inheritance");
  }
  {
    // A full fetch from slot 5 followed by a mini fetch with its own encoded
    // slot 0 must plan both accesses against the inherited slot 5.
    const auto words = MakeProgram({MakeVertexFetch(5, 1, false),
                                    MakeVertexFetch(5, 1, true)},
                                   1u | (1u << 2));
    fear_nx::deko::VertexRangePlan plan{};
    AnalyzeSynthetic(words, index_unmodified, fetch_indices_unmodified, addresses_linear, &plan);
    passed &= CheckSynthetic(index_unmodified && fetch_indices_unmodified && addresses_linear,
                             "full plus mini fetch has proven address flow");
    passed &= CheckSynthetic(plan[5].valid && plan[5].accesses.size() == 2,
                             "mini access follows inherited slot 5");
    passed &= CheckSynthetic(!plan[0].valid,
                             "mini encoded slot 0 does not create a slot 0 plan");
  }

  {
    // A full fetch consumes the original index before its own destination can
    // overwrite r0.x, so this one access remains eligible for prefix clipping.
    const auto words = MakeProgram({MakeVertexFetch(5, 0, false)}, 1);
    fear_nx::deko::VertexRangePlan plan{};
    AnalyzeSynthetic(words, index_unmodified, fetch_indices_unmodified, addresses_linear, &plan);
    passed &= CheckSynthetic(!index_unmodified && fetch_indices_unmodified && addresses_linear,
                             "last full fetch may write r0.x after capturing its input");
    passed &= CheckSynthetic(plan[5].valid && plan[5].accesses.size() == 1,
                             "captured index allows the full-fetch range plan");
  }
  {
    // A write after a full fetch does not alter that fetch's address.
    const auto words = MakeProgram({MakeVertexFetch(5, 1, false),
                                    MakeAluWrite(0, false, 1)},
                                   1);
    fear_nx::deko::VertexRangePlan plan{};
    AnalyzeSynthetic(words, index_unmodified, fetch_indices_unmodified, addresses_linear, &plan);
    passed &= CheckSynthetic(!index_unmodified && fetch_indices_unmodified && addresses_linear,
                             "later ALU r0.x write preserves earlier fetch address");
    passed &= CheckSynthetic(plan[5].valid,
                             "earlier full fetch remains range-plan eligible");
  }
  {
    // The first fetch changes r0.x. A subsequent full fetch would use that new
    // value, so all prefix clipping for this shader must be rejected.
    const auto words = MakeProgram({MakeVertexFetch(5, 0, false),
                                    MakeVertexFetch(6, 1, false)},
                                   1u | (1u << 2));
    fear_nx::deko::VertexRangePlan plan{};
    AnalyzeSynthetic(words, index_unmodified, fetch_indices_unmodified, addresses_linear, &plan);
    passed &= CheckSynthetic(!index_unmodified && !fetch_indices_unmodified && addresses_linear,
                             "later full fetch after r0.x write is unsafe");
    passed &= CheckSynthetic(std::none_of(plan.begin(), plan.end(),
                                          [](const auto& slot) { return slot.valid; }),
                             "unsafe later full fetch rejects all range plans");
  }
  {
    // Mini fetches reuse the address calculated by the earlier full fetch even
    // if that full fetch wrote r0.x as its result.
    const auto words = MakeProgram({MakeVertexFetch(5, 0, false),
                                    MakeVertexFetch(5, 1, true)},
                                   1u | (1u << 2));
    fear_nx::deko::VertexRangePlan plan{};
    AnalyzeSynthetic(words, index_unmodified, fetch_indices_unmodified, addresses_linear, &plan);
    passed &= CheckSynthetic(!index_unmodified && fetch_indices_unmodified && addresses_linear,
                             "mini fetch reuses captured address after r0.x write");
    passed &= CheckSynthetic(plan[5].valid && plan[5].accesses.size() == 2,
                             "captured full and mini fetches share a safe plan");
  }

  if (passed) std::cout << "synthetic shader proof checks=22 passed\n";
  return passed;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3 || std::string_view(argv[1]) == "--help") {
    std::cerr << "Usage: deko_vertex_range_shader_test <xesh-cache.xsh> "
                 "[expected-vertex-count]\n";
    return argc >= 2 && std::string_view(argv[1]) == "--help" ? 0 : 2;
  }

  if (!RunSyntheticCases()) return 1;

  size_t expected_vertex_count = 0;
  if (argc == 3) {
    try {
      expected_vertex_count = std::stoull(argv[2]);
    } catch (...) {
      std::cerr << "Invalid expected vertex shader count.\n";
      return 2;
    }
  }

  std::vector<Record> records;
  std::string error;
  if (!LoadXesh(argv[1], records, error)) {
    std::cerr << error << '\n';
    return 2;
  }

  size_t vertex_count = 0;
  size_t untouched_index_count = 0;
  size_t captured_index_proof_count = 0;
  size_t linear_fetch_count = 0;
  size_t planned_shader_count = 0;
  size_t plan_valid_slot_count = 0;
  size_t plan_access_count = 0;
  rex::string::StringBuffer disassembly;
  for (const Record& record : records) {
    if (record.type != rex::graphics::xenos::ShaderType::kVertex) continue;
    ++vertex_count;

    rex::graphics::SpirvShader shader(record.type, record.hash, record.words.data(),
                                      record.words.size(), std::endian::big);
    shader.AnalyzeUcode(disassembly);
    const bool index_unmodified = shader.vertex_index_register_unmodified();
    const bool fetch_indices_unmodified = shader.vertex_fetch_indices_unmodified();
    const bool addresses_linear = shader.vertex_fetch_addresses_linear();
    if (index_unmodified) ++untouched_index_count;
    if (fetch_indices_unmodified) ++captured_index_proof_count;
    if (addresses_linear) ++linear_fetch_count;

    const auto plan = fear_nx::deko::BuildVertexRangePlan(shader);
    bool shader_has_plan = false;
    for (const auto& slot : plan) {
      if (!slot.valid) continue;
      shader_has_plan = true;
      ++plan_valid_slot_count;
      plan_access_count += slot.accesses.size();
      if (!fetch_indices_unmodified || !addresses_linear || slot.accesses.empty()) {
        std::cerr << "Invalid slot plan escaped shader proof.\n";
        return 1;
      }
    }
    if (shader_has_plan) ++planned_shader_count;
  }

  if (vertex_count == 0 ||
      (expected_vertex_count != 0 && vertex_count != expected_vertex_count)) {
    std::cerr << "Expected " << expected_vertex_count << " vertex shaders, found "
              << vertex_count << ".\n";
    return 1;
  }

  std::cout << "XESH records=" << records.size() << " vertex_shaders=" << vertex_count
            << " r0x_unmodified=" << untouched_index_count
            << " r0x_written_or_ambiguous=" << (vertex_count - untouched_index_count)
            << " captured_index_proof=" << captured_index_proof_count
            << " linear_fetches=" << linear_fetch_count
            << " planned_shaders=" << planned_shader_count
            << " planned_slots=" << plan_valid_slot_count
            << " planned_fetch_accesses=" << plan_access_count << '\n';
  return 0;
}
