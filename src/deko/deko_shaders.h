#pragma once

#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace fear_nx::deko {

enum class ShaderStage : uint8_t {
  kVertex,
  kGeometry,
  kFragment,
  kCompute,
};

struct DescriptorLocation {
  static constexpr uint32_t kUnused = std::numeric_limits<uint32_t>::max();

  uint32_t set = kUnused;
  uint32_t binding = kUnused;

  bool valid() const { return set != kUnused && binding != kUnused; }
};

enum class ImageDimension : uint8_t {
  kUnknown,
  k1D,
  k2D,
  k3D,
  kCube,
  kRect,
  kBuffer,
  kSubpass,
};

struct TextureBinding {
  // The original Vulkan locations let the renderer connect the dense Deko
  // slot back to the translator's texture and sampler binding tables.
  DescriptorLocation image;
  DescriptorLocation sampler;
  uint32_t slot = 0;
  uint32_t count = 1;
  ImageDimension dimension = ImageDimension::kUnknown;
  bool arrayed = false;
  bool multisampled = false;
  bool sampler_is_combined = false;
};

struct UniformBufferBinding {
  DescriptorLocation source;
  uint32_t slot = 0;
  uint32_t count = 1;
};

struct StorageBufferBinding {
  DescriptorLocation source;
  uint32_t slot = 0;
  uint32_t count = 1;
};

struct StorageImageBinding {
  DescriptorLocation source;
  uint32_t slot = 0;
  uint32_t count = 1;
  ImageDimension dimension = ImageDimension::kUnknown;
  bool arrayed = false;
  bool multisampled = false;
};

struct SystemConstantCopyRange {
  uint32_t source_offset = 0;
  uint32_t destination_offset = 0;
  uint32_t size = 0;
};

struct GlslShader {
  ShaderStage stage = ShaderStage::kVertex;
  std::string source;
  std::vector<TextureBinding> textures;
  std::vector<UniformBufferBinding> uniform_buffers;
  std::vector<StorageBufferBinding> storage_buffers;
  std::vector<StorageImageBinding> storage_images;
  // ReXGlue's CPU SystemConstants uses scalar-packed offsets, which cannot be
  // represented by UAM's GLSL compiler. Guest shaders therefore receive a
  // std140-compatible UBO0 layout and these ranges describe the CPU-side copy.
  uint32_t system_constant_size = 0;
  std::vector<SystemConstantCopyRange> system_constant_copy_ranges;
  std::vector<std::string> warnings;
  std::string error;

  explicit operator bool() const { return error.empty() && !source.empty(); }
};

// Converts ReXGlue's Vulkan-translator SPIR-V to flat-binding GLSL that uam
// can compile. This preserves the translator's descriptor ABI: UBO set 1 and
// bindings 0..4, shared-memory storage set 0, vertex textures set 2, and
// fragment textures set 3. The returned metadata maps the remapped resources
// back to their original Vulkan set/binding pairs.
GlslShader ConvertSpirv(std::span<const uint32_t> spirv, ShaderStage expected_stage);

}  // namespace fear_nx::deko
