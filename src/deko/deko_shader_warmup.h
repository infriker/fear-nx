#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fear_nx::deko::shader_warmup {

// Xenos shader stage values used by ReXGlue's shader hash seed.
inline constexpr uint32_t kVertexShader = 0;
inline constexpr uint32_t kPixelShader = 1;

struct Entry {
  uint32_t shader_type = kVertexShader;
  uint64_t shader_hash = 0;
  uint64_t modification = 0;
  // Host-order Xenos microcode words (the same representation returned by
  // SpirvShader::ucode_data()). The file format encodes each word explicitly
  // as little-endian; shader_hash is verified over big-endian guest bytes.
  std::vector<uint32_t> ucode;
};

// Deterministic discovery name: vs-<16 hex hash>-<16 hex modification>.fwarm
// or ps-<16 hex hash>-<16 hex modification>.fwarm.
std::string FileName(const Entry& entry);

// Saves one observed guest shader variant. The destination is replaced only
// after a complete temporary file has been written and validated.
bool Save(const std::filesystem::path& path, const Entry& entry, std::string& error);

// Loads and verifies a .fwarm entry. `out` is left unchanged on failure.
bool Load(const std::filesystem::path& path, Entry& out, std::string& error);

}  // namespace fear_nx::deko::shader_warmup
