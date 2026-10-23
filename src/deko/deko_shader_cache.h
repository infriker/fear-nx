#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "deko_shaders.h"

namespace fear_nx::deko::shader_cache {

// Bump when SPIR-V-to-GLSL conversion or the cached resource ABI changes.
inline constexpr uint32_t kConverterAbiRevision = 1;

struct Entry {
  GlslShader info;
  std::vector<uint8_t> dksh;
};

// Stable across host/Switch endianness: hashes the LE SPIR-V words together
// with the converter ABI revision, shader stage and exact compiler identity.
uint64_t SpirvKey(std::span<const uint32_t> spirv, ShaderStage stage,
                  std::string_view compiler_id);

bool Load(const std::filesystem::path& path, uint64_t expected_key,
          ShaderStage stage, std::string_view compiler_id, Entry& out,
          std::string& error);

bool Save(const std::filesystem::path& path, uint64_t key, ShaderStage stage,
          std::string_view compiler_id, const Entry& entry,
          std::string& error);

}  // namespace fear_nx::deko::shader_cache
