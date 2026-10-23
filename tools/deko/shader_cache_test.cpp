#include "deko/deko_shader_cache.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include <xxhash.h>

namespace cache = fear_nx::deko::shader_cache;
using fear_nx::deko::DescriptorLocation;
using fear_nx::deko::GlslShader;
using fear_nx::deko::ImageDimension;
using fear_nx::deko::ShaderStage;
using fear_nx::deko::StorageBufferBinding;
using fear_nx::deko::StorageImageBinding;
using fear_nx::deko::SystemConstantCopyRange;
using fear_nx::deko::TextureBinding;
using fear_nx::deko::UniformBufferBinding;

namespace {

bool Check(bool condition, const char* message) {
  if (!condition) std::cerr << "FAIL: " << message << '\n';
  return condition;
}

void WriteU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value);

GlslShader MakeInfo() {
  GlslShader info;
  info.stage = ShaderStage::kGeometry;
  info.source = "#version 450\nvoid main() {}\n";
  info.textures = {
      {{2, 4}, {3, 1}, 1, 2, ImageDimension::k2D, true, false, true},
      {{2, 7}, {DescriptorLocation::kUnused, DescriptorLocation::kUnused},
       4, 1, ImageDimension::kCube, false, true, false},
  };
  info.uniform_buffers = {{{1, 0}, 0, 1}, {{1, 2}, 3, 1}};
  info.storage_buffers = {{{0, 0}, 0, 1}};
  info.storage_images = {{{0, 2}, 0, 1, ImageDimension::k2D, false, false}};
  info.system_constant_size = 32;
  info.system_constant_copy_ranges = {{0, 0, 4}, {16, 16, 16}};
  info.warnings = {"compatibility warning", "second warning"};
  return info;
}

cache::Entry MakeEntry() {
  cache::Entry entry;
  entry.info = MakeInfo();
  entry.dksh.resize(256 + 256);
  WriteU32(entry.dksh, 0, 0x48534B44u);  // DKSH magic
  WriteU32(entry.dksh, 4, 24);           // DkshHeader size
  WriteU32(entry.dksh, 8, 256);          // control section
  WriteU32(entry.dksh, 12, 256);         // code section
  WriteU32(entry.dksh, 16, 24);          // one program header follows
  WriteU32(entry.dksh, 20, 1);           // exactly one program
  WriteU32(entry.dksh, 24, 2);           // geometry program
  WriteU32(entry.dksh, 28, 0);           // entry point within code section
  return entry;
}

bool SameLocation(DescriptorLocation a, DescriptorLocation b) {
  return a.set == b.set && a.binding == b.binding;
}

bool SameInfo(const GlslShader& a, const GlslShader& b) {
  if (a.stage != b.stage || a.source != b.source || a.system_constant_size != b.system_constant_size ||
      a.error != b.error || a.warnings != b.warnings || a.textures.size() != b.textures.size() ||
      a.uniform_buffers.size() != b.uniform_buffers.size() ||
      a.storage_buffers.size() != b.storage_buffers.size() ||
      a.storage_images.size() != b.storage_images.size() ||
      a.system_constant_copy_ranges.size() != b.system_constant_copy_ranges.size()) return false;
  for (size_t i = 0; i < a.textures.size(); ++i) {
    const auto& x = a.textures[i]; const auto& y = b.textures[i];
    if (!SameLocation(x.image, y.image) || !SameLocation(x.sampler, y.sampler) ||
        x.slot != y.slot || x.count != y.count || x.dimension != y.dimension ||
        x.arrayed != y.arrayed || x.multisampled != y.multisampled ||
        x.sampler_is_combined != y.sampler_is_combined) return false;
  }
  for (size_t i = 0; i < a.uniform_buffers.size(); ++i) {
    const auto& x = a.uniform_buffers[i]; const auto& y = b.uniform_buffers[i];
    if (!SameLocation(x.source, y.source) || x.slot != y.slot || x.count != y.count) return false;
  }
  for (size_t i = 0; i < a.storage_buffers.size(); ++i) {
    const auto& x = a.storage_buffers[i]; const auto& y = b.storage_buffers[i];
    if (!SameLocation(x.source, y.source) || x.slot != y.slot || x.count != y.count) return false;
  }
  for (size_t i = 0; i < a.storage_images.size(); ++i) {
    const auto& x = a.storage_images[i]; const auto& y = b.storage_images[i];
    if (!SameLocation(x.source, y.source) || x.slot != y.slot || x.count != y.count ||
        x.dimension != y.dimension || x.arrayed != y.arrayed ||
        x.multisampled != y.multisampled) return false;
  }
  for (size_t i = 0; i < a.system_constant_copy_ranges.size(); ++i) {
    const auto& x = a.system_constant_copy_ranges[i]; const auto& y = b.system_constant_copy_ranges[i];
    if (x.source_offset != y.source_offset || x.destination_offset != y.destination_offset ||
        x.size != y.size) return false;
  }
  return true;
}

bool ReadAll(const std::filesystem::path& path, std::vector<uint8_t>& data) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  data.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  return bool(input) || input.eof();
}

bool WriteAll(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return false;
  output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  output.close();
  return bool(output);
}

uint32_t ReadU32(const std::vector<uint8_t>& bytes, size_t offset) {
  uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) value |= uint32_t(bytes.at(offset + i)) << (8 * i);
  return value;
}

void WriteU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) bytes.at(offset + i) = uint8_t(value >> (8 * i));
}

size_t DkshOffsetForFixture(const std::vector<uint8_t>& bytes, size_t payload) {
  size_t offset = payload;
  const uint32_t source_size = ReadU32(bytes, offset);
  offset += 4 + source_size;
  const uint32_t textures = ReadU32(bytes, offset); offset += 4 + size_t(textures) * 28;
  const uint32_t uniforms = ReadU32(bytes, offset); offset += 4 + size_t(uniforms) * 16;
  const uint32_t storage = ReadU32(bytes, offset); offset += 4 + size_t(storage) * 16;
  const uint32_t images = ReadU32(bytes, offset); offset += 4 + size_t(images) * 19;
  offset += 4;  // system-constant byte size
  const uint32_t ranges = ReadU32(bytes, offset); offset += 4 + size_t(ranges) * 12;
  const uint32_t warnings = ReadU32(bytes, offset); offset += 4;
  for (uint32_t i = 0; i < warnings; ++i) {
    const uint32_t length = ReadU32(bytes, offset);
    offset += 4 + length;
  }
  return offset + 4;  // DKSH length prefix
}

void FixChecksum(std::vector<uint8_t>& bytes) {
  const uint64_t checksum = XXH3_64bits(bytes.data(), bytes.size() - 8);
  for (unsigned i = 0; i < 8; ++i) bytes[bytes.size() - 8 + i] = uint8_t(checksum >> (8 * i));
}

bool ExpectLoadFailure(const std::filesystem::path& path, uint64_t key,
                       ShaderStage stage, std::string_view compiler,
                       const char* message) {
  cache::Entry output;
  std::string error;
  return Check(!cache::Load(path, key, stage, compiler, output, error) && !error.empty(), message);
}

}  // namespace

int main() {
  const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root = std::filesystem::temp_directory_path() /
                    ("fear-deko-shader-cache-test-" + std::to_string(ticks));
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);
  if (!Check(!ec, "create temporary test directory")) return 1;
  const auto path = root / "sample.fshader";
  constexpr uint64_t key = 0x123456789abcdef0ULL;
  constexpr std::string_view compiler = "uam-test-0.5";
  std::string error;
  bool ok = true;

  const std::array<uint32_t, 4> words{0x07230203, 0x00010000, 0, 7};
  const uint64_t vertex_key = cache::SpirvKey(words, ShaderStage::kVertex, compiler);
  ok &= Check(vertex_key != 0 && vertex_key == cache::SpirvKey(words, ShaderStage::kVertex, compiler),
              "SPIR-V key is stable");
  ok &= Check(vertex_key != cache::SpirvKey(words, ShaderStage::kFragment, compiler),
              "SPIR-V key includes stage");
  ok &= Check(vertex_key != cache::SpirvKey(words, ShaderStage::kVertex, "uam-test-next"),
              "SPIR-V key includes compiler identity");

  cache::Entry expected = MakeEntry();
  const bool saved = cache::Save(path, key, expected.info.stage, compiler, expected, error);
  if (!saved) std::cerr << "Save error: " << error << '\n';
  ok &= Check(saved, "save full shader entry");
  cache::Entry loaded;
  ok &= Check(cache::Load(path, key, expected.info.stage, compiler, loaded, error),
              "load full shader entry");
  ok &= Check(SameInfo(expected.info, loaded.info) && expected.dksh == loaded.dksh,
              "round-trip preserves every metadata field and DKSH bytes");

  ok &= ExpectLoadFailure(path, key + 1, expected.info.stage, compiler, "reject wrong key");
  ok &= ExpectLoadFailure(path, key, ShaderStage::kFragment, compiler, "reject wrong stage");
  ok &= ExpectLoadFailure(path, key, expected.info.stage, "uam-other", "reject wrong compiler identity");

  std::vector<uint8_t> bytes;
  ok &= Check(ReadAll(path, bytes), "read serialized cache for corruption tests");
  if (bytes.size() > 1) {
    auto corrupt = bytes;
    corrupt[corrupt.size() / 2] ^= 0x40;
    const auto corrupt_path = root / "corrupt.fshader";
    ok &= Check(WriteAll(corrupt_path, corrupt), "write checksum corruption case");
    ok &= ExpectLoadFailure(corrupt_path, key, expected.info.stage, compiler, "reject checksum corruption");

    corrupt = bytes;
    corrupt.resize(corrupt.size() - 1);
    const auto truncated_path = root / "truncated.fshader";
    ok &= Check(WriteAll(truncated_path, corrupt), "write truncated cache case");
    ok &= ExpectLoadFailure(truncated_path, key, expected.info.stage, compiler, "reject truncated file");

    corrupt = bytes;
    WriteU32(corrupt, 12, cache::kConverterAbiRevision + 1);
    FixChecksum(corrupt);
    const auto old_abi_path = root / "old-abi.fshader";
    ok &= Check(WriteAll(old_abi_path, corrupt), "write old converter ABI case");
    ok &= ExpectLoadFailure(old_abi_path, key, expected.info.stage, compiler, "reject old converter ABI");

    corrupt = bytes;
    corrupt[16] = 0xff;
    FixChecksum(corrupt);
    const auto bad_stage_path = root / "bad-stage.fshader";
    ok &= Check(WriteAll(bad_stage_path, corrupt), "write invalid stage case");
    ok &= ExpectLoadFailure(bad_stage_path, key, expected.info.stage, compiler, "reject invalid stage enum");

    // Header: magic(8), format(4), ABI(4), stage+reserved(4), key(8),
    // compiler length(4), compiler bytes, payload length(8). Payload starts
    // with the GLSL string length+bytes followed by the texture count.
    const size_t payload = 40 + compiler.size();
    const uint32_t source_length = ReadU32(bytes, payload);
    corrupt = bytes;
    WriteU32(corrupt, payload + 4 + source_length, UINT32_MAX);
    FixChecksum(corrupt);
    const auto bad_count_path = root / "bad-count.fshader";
    ok &= Check(WriteAll(bad_count_path, corrupt), "write oversized metadata count case");
    ok &= ExpectLoadFailure(bad_count_path, key, expected.info.stage, compiler, "reject oversized metadata count");

    corrupt = bytes;
    const size_t dksh = DkshOffsetForFixture(bytes, payload);
    corrupt[dksh] ^= 0x01;
    FixChecksum(corrupt);
    const auto bad_dksh_path = root / "bad-dksh.fshader";
    ok &= Check(WriteAll(bad_dksh_path, corrupt), "write structurally invalid DKSH case");
    ok &= ExpectLoadFailure(bad_dksh_path, key, expected.info.stage, compiler,
                            "reject corrupt DKSH after outer checksum passes");

    corrupt = bytes;
    WriteU32(corrupt, dksh + 20, 2);  // runtime supports exactly one program
    FixChecksum(corrupt);
    const auto bad_program_count_path = root / "bad-program-count.fshader";
    ok &= Check(WriteAll(bad_program_count_path, corrupt), "write multiple-program DKSH case");
    ok &= ExpectLoadFailure(bad_program_count_path, key, expected.info.stage, compiler,
                            "reject DKSH with an unsupported program count");

    corrupt = bytes;
    WriteU32(corrupt, dksh + 8, 257);  // section sizes must be 256-byte aligned
    FixChecksum(corrupt);
    const auto bad_section_path = root / "bad-section-size.fshader";
    ok &= Check(WriteAll(bad_section_path, corrupt), "write misaligned DKSH control section case");
    ok &= ExpectLoadFailure(bad_section_path, key, expected.info.stage, compiler,
                            "reject invalid DKSH section size");

    corrupt = bytes;
    WriteU32(corrupt, 28, std::numeric_limits<uint32_t>::max());
    FixChecksum(corrupt);
    const auto bad_identity_length_path = root / "bad-identity-length.fshader";
    ok &= Check(WriteAll(bad_identity_length_path, corrupt), "write oversized identity length case");
    ok &= ExpectLoadFailure(bad_identity_length_path, key, expected.info.stage, compiler,
                            "reject oversized compiler identity length");
  }

  // A failed replacement must leave the previous valid file intact. A later
  // valid replacement must atomically publish the complete new entry.
  cache::Entry invalid = expected;
  invalid.info.textures[0].slot = 31;
  invalid.info.textures[0].count = 2;
  ok &= Check(!cache::Save(path, key, expected.info.stage, compiler, invalid, error),
              "reject out-of-bounds descriptor slot range");
  loaded = {};
  ok &= Check(cache::Load(path, key, expected.info.stage, compiler, loaded, error) &&
                  SameInfo(expected.info, loaded.info) && loaded.dksh == expected.dksh,
              "failed save preserves prior cache file");

  invalid = expected;
  invalid.dksh[0] ^= 0xff;
  ok &= Check(!cache::Save(root / "bad-dksh-save.fshader", key,
                           expected.info.stage, compiler, invalid, error),
              "reject invalid DKSH header before saving");

  cache::Entry replacement = expected;
  replacement.info.source += "// replacement\n";
  replacement.dksh.back() ^= 0x09;
  const bool replaced = cache::Save(path, key, expected.info.stage, compiler, replacement, error);
  if (!replaced) std::cerr << "Replacement error: " << error << '\n';
  ok &= Check(replaced, "replace an existing cache entry");
  loaded = {};
  ok &= Check(cache::Load(path, key, expected.info.stage, compiler, loaded, error) &&
                  SameInfo(replacement.info, loaded.info) && loaded.dksh == replacement.dksh,
              "atomic replacement publishes complete new entry");

  invalid = expected;
  invalid.info.system_constant_copy_ranges[1].destination_offset = 31;
  invalid.info.system_constant_copy_ranges[1].size = 4;
  ok &= Check(!cache::Save(root / "bad-range.fshader", key, expected.info.stage,
                           compiler, invalid, error),
              "reject system-constant destination range outside the UBO");

  const auto oversized_path = root / "oversized.fshader";
  {
    std::ofstream output(oversized_path, std::ios::binary | std::ios::trunc);
    output.seekp(16u * 1024u * 1024u);
    output.put('\0');
  }
  ok &= ExpectLoadFailure(oversized_path, key, expected.info.stage, compiler,
                          "reject oversized file before allocation");

  std::filesystem::remove_all(root, ec);
  if (ok) std::cout << "All shader cache tests passed\n";
  return ok ? 0 : 1;
}
