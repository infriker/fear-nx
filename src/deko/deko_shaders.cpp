// SPDX-License-Identifier: GPL-3.0-or-later

#include "deko_shaders.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include <spirv_cross/spirv_glsl.hpp>

namespace fear_nx::deko {
namespace {

constexpr uint32_t kMaxTextureSlotsPerStage = 32;
constexpr uint32_t kMaxStorageImageSlotsPerStage = 32;
constexpr uint32_t kConstantBufferCount = 5;
constexpr std::array<const char*, 5> kUamUnsupportedExtensions = {
    "GL_EXT_buffer_reference",
    "GL_ARB_gpu_shader_int64",
    "GL_EXT_shader_explicit_arithmetic_types",
    "GL_EXT_nonuniform_qualifier",
    "GL_EXT_samplerless_texture_functions",
};

using Resource = spirv_cross::Resource;

DescriptorLocation GetLocation(const spirv_cross::Compiler& compiler, uint32_t id) {
  DescriptorLocation location;
  if (compiler.has_decoration(id, spv::DecorationDescriptorSet)) {
    location.set = compiler.get_decoration(id, spv::DecorationDescriptorSet);
  }
  if (compiler.has_decoration(id, spv::DecorationBinding)) {
    location.binding = compiler.get_decoration(id, spv::DecorationBinding);
  }
  return location;
}

std::string LocationString(DescriptorLocation location) {
  if (!location.valid()) {
    return "without a complete set/binding";
  }
  return "set " + std::to_string(location.set) + ", binding " +
         std::to_string(location.binding);
}

ImageDimension ConvertDimension(spv::Dim dimension) {
  switch (dimension) {
    case spv::Dim1D:
      return ImageDimension::k1D;
    case spv::Dim2D:
      return ImageDimension::k2D;
    case spv::Dim3D:
      return ImageDimension::k3D;
    case spv::DimCube:
      return ImageDimension::kCube;
    case spv::DimRect:
      return ImageDimension::kRect;
    case spv::DimBuffer:
      return ImageDimension::kBuffer;
    case spv::DimSubpassData:
      return ImageDimension::kSubpass;
    default:
      return ImageDimension::kUnknown;
  }
}

const char* DimensionName(ImageDimension dimension) {
  switch (dimension) {
    case ImageDimension::k1D:
      return "1D";
    case ImageDimension::k2D:
      return "2D";
    case ImageDimension::k3D:
      return "3D";
    case ImageDimension::kCube:
      return "cube";
    case ImageDimension::kRect:
      return "rectangle";
    case ImageDimension::kBuffer:
      return "buffer";
    case ImageDimension::kSubpass:
      return "subpass";
    case ImageDimension::kUnknown:
      return "unknown";
  }
  return "unknown";
}

bool IsSupportedSampledDimension(ImageDimension dimension) {
  // The Xenos translator currently emits 2D-array, 3D and cube textures.
  // Other dimensions are retained in metadata for utility shaders but are
  // rejected by the game-shader ABI gate.
  return dimension == ImageDimension::k2D || dimension == ImageDimension::k3D ||
         dimension == ImageDimension::kCube;
}

bool HasVariableDescriptorArray(const spirv_cross::Compiler& compiler, uint32_t variable_id) {
  const auto& type = compiler.get_type_from_variable(variable_id);
  return !type.array.empty();
}

spv::ExecutionModel ExecutionModelFor(ShaderStage stage) {
  switch (stage) {
    case ShaderStage::kVertex:
      return spv::ExecutionModelVertex;
    case ShaderStage::kGeometry:
      return spv::ExecutionModelGeometry;
    case ShaderStage::kFragment:
      return spv::ExecutionModelFragment;
    case ShaderStage::kCompute:
      return spv::ExecutionModelGLCompute;
  }
  return spv::ExecutionModelMax;
}

bool IsGameStage(ShaderStage stage) { return stage != ShaderStage::kCompute; }

bool ValidateGameLocation(DescriptorLocation location, uint32_t expected_set,
                          const char* resource_name, std::string& error) {
  if (!location.valid()) {
    error = std::string(resource_name) + " has " + LocationString(location);
    return false;
  }
  if (location.set != expected_set) {
    error = std::string(resource_name) + " uses unexpected descriptor " +
            LocationString(location) + "; ReXGlue expects set " +
            std::to_string(expected_set);
    return false;
  }
  return true;
}

void RemapToFlatBinding(spirv_cross::Compiler& compiler, uint32_t id, uint32_t slot) {
  compiler.set_decoration(id, spv::DecorationBinding, slot);
  compiler.unset_decoration(id, spv::DecorationDescriptorSet);
}

bool CheckNoUnsupportedResources(const spirv_cross::ShaderResources& resources,
                                 std::string& error) {
  if (!resources.push_constant_buffers.empty()) {
    error = "push-constant blocks are not part of the ReXGlue descriptor ABI";
    return false;
  }
  if (!resources.atomic_counters.empty()) {
    error = "atomic-counter resources are not supported by the Deko shader ABI";
    return false;
  }
  if (!resources.subpass_inputs.empty()) {
    error = "subpass-input resources are not supported by the Deko shader ABI";
    return false;
  }
  if (!resources.acceleration_structures.empty()) {
    error = "ray-tracing acceleration structures are not supported by the Deko shader ABI";
    return false;
  }
  return true;
}

void CheckUamGlslCompatibility(const std::string& source, GlslShader& result) {
  for (const char* extension : kUamUnsupportedExtensions) {
    if (source.find(extension) != std::string::npos) {
      result.error = std::string("generated GLSL requires unsupported UAM extension ") + extension;
      result.source.clear();
      return;
    }
  }

  // UAM's Mesa frontend can read a dynamically selected vector component as
  // .x. Check all UBO spellings emitted by SPIRV-Cross, including native
  // `xe_uniform_*` blocks and the SDK's `g_Ubo[].v[][]` blocks.
  for (size_t at = source.find("]["); at != std::string::npos; at = source.find("][", at + 2)) {
    const size_t line_start = source.rfind('\n', at) == std::string::npos
                                  ? 0
                                  : source.rfind('\n', at) + 1;
    const size_t line_end = source.find('\n', at);
    const std::string_view line(source.data() + line_start,
                                (line_end == std::string::npos ? source.size() : line_end) -
                                    line_start);
    if (line.find("xe_uniform_") == std::string_view::npos &&
        line.find("g_Ubo") == std::string_view::npos) {
      continue;
    }

    size_t component_start = at + 2;
    int bracket_depth = 1;
    size_t component_end = component_start;
    for (; component_end < source.size() && bracket_depth; ++component_end) {
      bracket_depth += source[component_end] == '[' ? 1 : source[component_end] == ']' ? -1 : 0;
    }
    if (bracket_depth) continue;
    --component_end;
    const std::string_view component(source.data() + component_start,
                                     component_end - component_start);
    bool literal = !component.empty();
    for (char c : component) {
      literal &= std::isdigit(static_cast<unsigned char>(c)) || c == 'u';
    }
    if (!literal) {
      result.warnings.push_back("dynamic UBO vector component may be read as .x by UAM: " +
                                std::string(line));
    }
  }
}

bool IsSetAndBinding(const spirv_cross::Compiler& compiler, uint32_t id, uint32_t set,
                     uint32_t binding) {
  const DescriptorLocation location = GetLocation(compiler, id);
  return location.valid() && location.set == set && location.binding == binding;
}

struct Std140Layout {
  uint32_t alignment = 0;
  uint32_t size = 0;
};

uint32_t AlignUp(uint32_t value, uint32_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

bool GetStd140Layout(const spirv_cross::SPIRType& type, Std140Layout& layout,
                     std::string& error) {
  if (type.array.size() > 1) {
    error = "system constant has a multidimensional array that the Deko ABI cannot repack";
    return false;
  }

  const uint32_t scalar_size = type.width / 8;
  if (!scalar_size || type.width % 8) {
    error = "system constant uses a scalar width unsupported by the Deko ABI";
    return false;
  }

  uint32_t element_alignment = 0;
  uint32_t element_size = 0;
  if (type.columns > 1) {
    // The current ReXGlue system block contains no matrices. Keep the
    // calculator correct for column-major matrices so a future addition fails
    // only if its type is otherwise unsupported.
    const uint32_t vector_alignment = type.vecsize == 1 ? scalar_size
                                    : type.vecsize == 2 ? scalar_size * 2
                                                        : scalar_size * 4;
    const uint32_t vector_size = scalar_size * type.vecsize;
    const uint32_t matrix_stride = AlignUp(vector_size, std::max(16u, vector_alignment));
    element_alignment = std::max(16u, vector_alignment);
    element_size = matrix_stride * type.columns;
  } else {
    element_alignment = type.vecsize == 1 ? scalar_size
                       : type.vecsize == 2 ? scalar_size * 2
                                           : scalar_size * 4;
    element_size = scalar_size * type.vecsize;
  }

  if (!type.array.empty()) {
    if (!type.array_size_literal[0] || !type.array[0]) {
      error = "system constant uses a non-literal or empty array";
      return false;
    }
    layout.alignment = std::max(16u, element_alignment);
    const uint32_t stride = AlignUp(element_size, layout.alignment);
    if (type.array[0] > std::numeric_limits<uint32_t>::max() / stride) {
      error = "system-constant array is too large to repack";
      return false;
    }
    layout.size = stride * type.array[0];
  } else {
    layout.alignment = element_alignment;
    layout.size = element_size;
  }
  return true;
}

bool AppendSystemConstantCopyRanges(const spirv_cross::Compiler& compiler,
                                   const spirv_cross::SPIRType& parent_type,
                                   uint32_t member_index, const spirv_cross::SPIRType& member_type,
                                   uint32_t source_offset, uint32_t destination_offset,
                                   std::vector<SystemConstantCopyRange>& ranges,
                                   std::string& error) {
  if (member_type.basetype == spirv_cross::SPIRType::Struct) {
    error = "nested system-constant structs are not supported by the Deko ABI";
    return false;
  }

  if (!member_type.array.empty()) {
    if (member_type.array.size() != 1 || !member_type.array_size_literal[0] ||
        !member_type.array[0]) {
      error = "system constant uses an unsupported array shape";
      return false;
    }
    spirv_cross::SPIRType element_type = member_type;
    element_type.array.clear();
    element_type.array_size_literal.clear();
    Std140Layout element_layout;
    if (!GetStd140Layout(element_type, element_layout, error)) return false;

    uint32_t source_stride = compiler.type_struct_member_array_stride(parent_type, member_index);
    if (!source_stride) {
      const size_t declared_size = compiler.get_declared_struct_member_size(parent_type, member_index);
      if (!declared_size || declared_size % member_type.array[0]) {
        error = "system-constant array has no usable source stride";
        return false;
      }
      source_stride = uint32_t(declared_size / member_type.array[0]);
    }
    const uint32_t destination_stride =
        AlignUp(element_layout.size, std::max(16u, element_layout.alignment));
    for (uint32_t index = 0; index < member_type.array[0]; ++index) {
      if (!AppendSystemConstantCopyRanges(compiler, parent_type, member_index, element_type,
                                          source_offset + source_stride * index,
                                          destination_offset + destination_stride * index,
                                          ranges, error)) {
        return false;
      }
    }
    return true;
  }

  Std140Layout layout;
  if (!GetStd140Layout(member_type, layout, error)) return false;
  ranges.push_back({source_offset, destination_offset, layout.size});
  return true;
}

bool RepackSystemConstantBlock(spirv_cross::Compiler& compiler, const Resource& resource,
                               GlslShader& result) {
  const spirv_cross::SPIRType& struct_type = compiler.get_type(resource.base_type_id);
  if (struct_type.basetype != spirv_cross::SPIRType::Struct) {
    result.error = "ReXGlue system uniform buffer is not a structure";
    return false;
  }

  struct Member {
    uint32_t index;
    uint32_t source_offset;
    uint32_t destination_offset;
    Std140Layout layout;
  };
  std::vector<Member> members;
  members.reserve(struct_type.member_types.size());
  for (uint32_t index = 0; index < struct_type.member_types.size(); ++index) {
    if (!compiler.has_member_decoration(resource.base_type_id, index, spv::DecorationOffset)) {
      result.error = "ReXGlue system constant is missing its source byte offset";
      return false;
    }
    Std140Layout layout;
    const auto& member_type = compiler.get_type(struct_type.member_types[index]);
    if (!GetStd140Layout(member_type, layout, result.error)) return false;
    members.push_back({index, compiler.type_struct_member_offset(struct_type, index), 0, layout});
  }

  // Keep the SPIR-V member declaration order. GLSL explicit block offsets must
  // be monotonic in declaration order, and the CPU copy metadata makes the
  // source-to-destination remap independent of that order.
  uint32_t destination_cursor = 0;
  for (Member& member : members) {
    destination_cursor = AlignUp(destination_cursor, member.layout.alignment);
    member.destination_offset = destination_cursor;
    if (member.layout.size > std::numeric_limits<uint32_t>::max() - destination_cursor) {
      result.error = "ReXGlue system-constant block is too large to repack";
      return false;
    }
    destination_cursor += member.layout.size;
  }
  result.system_constant_size = AlignUp(destination_cursor, 16);

  for (const Member& member : members) {
    compiler.set_member_decoration(resource.base_type_id, member.index, spv::DecorationOffset,
                                   member.destination_offset);
    const auto& member_type = compiler.get_type(struct_type.member_types[member.index]);
    if (!AppendSystemConstantCopyRanges(compiler, struct_type, member.index, member_type,
                                        member.source_offset, member.destination_offset,
                                        result.system_constant_copy_ranges, result.error)) {
      return false;
    }
  }
  std::sort(result.system_constant_copy_ranges.begin(),
            result.system_constant_copy_ranges.end(),
            [](const SystemConstantCopyRange& a, const SystemConstantCopyRange& b) {
              if (a.source_offset != b.source_offset) return a.source_offset < b.source_offset;
              return a.destination_offset < b.destination_offset;
            });
  return true;
}

bool ValidateAndRemapUniformBuffers(spirv_cross::Compiler& compiler,
                                   const spirv_cross::ShaderResources& resources,
                                   ShaderStage stage, GlslShader& result) {
  std::array<bool, kConstantBufferCount> used{};
  std::vector<Resource> sorted(resources.uniform_buffers.begin(), resources.uniform_buffers.end());
  std::sort(sorted.begin(), sorted.end(), [&](const Resource& a, const Resource& b) {
    const auto left = GetLocation(compiler, a.id);
    const auto right = GetLocation(compiler, b.id);
    return std::pair(left.set, left.binding) < std::pair(right.set, right.binding);
  });

  for (const Resource& resource : sorted) {
    const DescriptorLocation location = GetLocation(compiler, resource.id);
    if (IsGameStage(stage)) {
      if (!ValidateGameLocation(location, 1, "uniform buffer", result.error)) return false;
      if (location.binding >= kConstantBufferCount) {
        result.error = "ReXGlue constant-buffer binding " +
                       std::to_string(location.binding) + " exceeds its five-buffer ABI";
        return false;
      }
      const uint32_t slot = location.binding;
      if (used[slot]) {
        result.error = "more than one uniform buffer uses ReXGlue set 1 binding " +
                       std::to_string(slot);
        return false;
      }
      used[slot] = true;
      if (slot == 0 && !RepackSystemConstantBlock(compiler, resource, result)) return false;
      RemapToFlatBinding(compiler, resource.id, slot);
      result.uniform_buffers.push_back({location, slot, 1});
    } else {
      if (!location.valid()) {
        result.error = "compute uniform buffer has " + LocationString(location);
        return false;
      }
      if (location.binding >= kConstantBufferCount || used[location.binding]) {
        result.error = "compute uniform-buffer bindings must be unique and in the range 0..4";
        return false;
      }
      used[location.binding] = true;
      RemapToFlatBinding(compiler, resource.id, location.binding);
      result.uniform_buffers.push_back({location, location.binding, 1});
    }
    if (HasVariableDescriptorArray(compiler, resource.id)) {
      result.error = "uniform-buffer descriptor arrays are not supported";
      return false;
    }
  }
  return true;
}

bool ValidateAndRemapStorageBuffers(spirv_cross::Compiler& compiler,
                                   const spirv_cross::ShaderResources& resources,
                                   ShaderStage stage, GlslShader& result) {
  std::vector<Resource> sorted(resources.storage_buffers.begin(), resources.storage_buffers.end());
  std::sort(sorted.begin(), sorted.end(), [&](const Resource& a, const Resource& b) {
    const auto left = GetLocation(compiler, a.id);
    const auto right = GetLocation(compiler, b.id);
    return std::pair(left.set, left.binding) < std::pair(right.set, right.binding);
  });

  for (const Resource& resource : sorted) {
    const DescriptorLocation location = GetLocation(compiler, resource.id);
    uint32_t slot = 0;
    if (IsGameStage(stage)) {
      if (!ValidateGameLocation(location, 0, "storage buffer", result.error)) return false;
      // ReXGlue reserves set 0 binding 0 for the 512 MiB guest shared-memory
      // buffer. Binding 1 is the optional EDRAM buffer used by its interlock
      // path, which this renderer does not enable and cannot safely bind.
      if (location.binding != 0) {
        result.error = "ReXGlue storage buffer " + LocationString(location) +
                       " is not the shared-memory binding supported by Deko";
        return false;
      }
    } else {
      if (!location.valid()) {
        result.error = "compute storage buffer has " + LocationString(location);
        return false;
      }
      if (location.set != 0 || location.binding != 0) {
        result.error = "compute storage buffers currently require set 0 binding 0";
        return false;
      }
    }
    if (HasVariableDescriptorArray(compiler, resource.id)) {
      result.error = "storage-buffer descriptor arrays are unsupported; configure the "
                     "ReXGlue translator for one shared-memory binding";
      return false;
    }
    RemapToFlatBinding(compiler, resource.id, slot);
    result.storage_buffers.push_back({location, slot, 1});
  }
  return true;
}

bool ValidateAndRemapCombinedTextures(spirv_cross::Compiler& compiler,
                                     const spirv_cross::ShaderResources& original_resources,
                                     ShaderStage stage, GlslShader& result) {
  const bool game_stage = IsGameStage(stage);
  const uint32_t expected_set = stage == ShaderStage::kVertex ? 2u : 3u;

  // Record pre-combined images that already carry their sampler in SPIR-V.
  // ReXGlue's translator emits separate images and samplers, but utility
  // shaders are allowed to use OpTypeSampledImage directly.
  std::vector<Resource> original_combined(original_resources.sampled_images.begin(),
                                          original_resources.sampled_images.end());
  std::sort(original_combined.begin(), original_combined.end(), [&](const Resource& a,
                                                                    const Resource& b) {
    const auto left = GetLocation(compiler, a.id);
    const auto right = GetLocation(compiler, b.id);
    return std::pair(left.set, left.binding) < std::pair(right.set, right.binding);
  });

  const auto combined = compiler.get_combined_image_samplers();
  struct PairInfo {
    uint32_t combined_id;
    DescriptorLocation image;
    DescriptorLocation sampler;
    ImageDimension dimension;
    bool arrayed;
    bool multisampled;
    bool original_combined;
  };
  std::vector<PairInfo> pairs;
  pairs.reserve(combined.size() + original_combined.size());

  for (const auto& pair : combined) {
    const DescriptorLocation image = GetLocation(compiler, pair.image_id);
    const DescriptorLocation sampler = GetLocation(compiler, pair.sampler_id);
    const auto& image_type = compiler.get_type_from_variable(pair.image_id);
    const DescriptorLocation combined_location = GetLocation(compiler, pair.combined_id);
    const uint32_t set = image.valid() ? image.set : combined_location.set;
    if (game_stage && set != expected_set) {
      result.error = "sampled image uses unexpected descriptor set " + std::to_string(set) +
                     "; ReXGlue expects set " + std::to_string(expected_set);
      return false;
    }
    if (!image.valid() && !combined_location.valid()) {
      result.error = "combined sampled image has no source descriptor binding";
      return false;
    }
    if (game_stage && sampler.valid() && sampler.set != expected_set) {
      result.error = "sampler uses unexpected descriptor " + LocationString(sampler) +
                     "; ReXGlue expects set " + std::to_string(expected_set);
      return false;
    }
    if (HasVariableDescriptorArray(compiler, pair.combined_id) ||
        HasVariableDescriptorArray(compiler, pair.image_id) ||
        (sampler.valid() && HasVariableDescriptorArray(compiler, pair.sampler_id))) {
      result.error = "sampled-image descriptor arrays are not supported";
      return false;
    }
    const ImageDimension dimension = ConvertDimension(image_type.image.dim);
    if (game_stage && !IsSupportedSampledDimension(dimension)) {
      result.error = "ReXGlue sampled image dimension " + std::string(DimensionName(dimension)) +
                     " is not supported by the Deko guest-shader ABI";
      return false;
    }
    pairs.push_back({pair.combined_id, image.valid() ? image : combined_location, sampler,
                     dimension, image_type.image.arrayed, image_type.image.ms, false});
  }

  // SPIRV-Cross may not report a combined variable in get_combined_image_samplers
  // when it was already represented as a sampled image in the input module.
  for (const Resource& resource : original_combined) {
    const bool already_added = std::any_of(pairs.begin(), pairs.end(), [&](const PairInfo& pair) {
      return pair.combined_id == resource.id;
    });
    if (already_added) continue;
    const DescriptorLocation location = GetLocation(compiler, resource.id);
    if (game_stage && !ValidateGameLocation(location, expected_set, "sampled image", result.error)) {
      return false;
    }
    const auto& type = compiler.get_type(resource.type_id);
    const ImageDimension dimension = ConvertDimension(type.image.dim);
    if (game_stage && !IsSupportedSampledDimension(dimension)) {
      result.error = "ReXGlue sampled image dimension is not supported by the Deko guest-shader ABI";
      return false;
    }
    pairs.push_back({resource.id, location, {}, dimension, type.image.arrayed, type.image.ms, true});
  }

  std::sort(pairs.begin(), pairs.end(), [](const PairInfo& a, const PairInfo& b) {
    return std::pair(a.image.set, a.image.binding) < std::pair(b.image.set, b.image.binding);
  });
  if (pairs.size() > kMaxTextureSlotsPerStage) {
    result.error = "shader needs " + std::to_string(pairs.size()) +
                   " combined texture slots; Deko supports at most 32 per stage";
    return false;
  }

  for (uint32_t slot = 0; slot < pairs.size(); ++slot) {
    const PairInfo& pair = pairs[slot];
    RemapToFlatBinding(compiler, pair.combined_id, slot);
    result.textures.push_back({pair.image, pair.sampler, slot, 1, pair.dimension, pair.arrayed,
                               pair.multisampled, true});
  }
  return true;
}

bool ValidateAndRemapStorageImages(spirv_cross::Compiler& compiler,
                                  const spirv_cross::ShaderResources& resources,
                                  ShaderStage stage, GlslShader& result) {
  std::vector<Resource> sorted(resources.storage_images.begin(), resources.storage_images.end());
  std::sort(sorted.begin(), sorted.end(), [&](const Resource& a, const Resource& b) {
    const auto left = GetLocation(compiler, a.id);
    const auto right = GetLocation(compiler, b.id);
    return std::pair(left.set, left.binding) < std::pair(right.set, right.binding);
  });
  if (sorted.size() > kMaxStorageImageSlotsPerStage) {
    result.error = "shader needs more than 32 storage-image slots";
    return false;
  }

  for (uint32_t slot = 0; slot < sorted.size(); ++slot) {
    const Resource& resource = sorted[slot];
    const DescriptorLocation location = GetLocation(compiler, resource.id);
    if (!location.valid()) {
      result.error = "storage image has " + LocationString(location);
      return false;
    }
    if (IsGameStage(stage)) {
      result.error = "storage images are not part of ReXGlue guest vertex/geometry/fragment ABI";
      return false;
    }
    if (HasVariableDescriptorArray(compiler, resource.id)) {
      result.error = "storage-image descriptor arrays are not supported";
      return false;
    }
    const auto& image_type = compiler.get_type(resource.type_id);
    RemapToFlatBinding(compiler, resource.id, slot);
    result.storage_images.push_back({location, slot, 1, ConvertDimension(image_type.image.dim),
                                     image_type.image.arrayed, image_type.image.ms});
  }
  return true;
}

bool ValidateStage(const spirv_cross::Compiler& compiler, ShaderStage expected,
                   std::string& error) {
  const spv::ExecutionModel model = compiler.get_execution_model();
  if (model != ExecutionModelFor(expected)) {
    error = "SPIR-V execution model does not match the requested shader stage";
    return false;
  }
  return true;
}

}  // namespace

GlslShader ConvertSpirv(std::span<const uint32_t> spirv, ShaderStage expected_stage) {
  GlslShader result;
  result.stage = expected_stage;
  if (spirv.size() < 5 || spirv.front() != spv::MagicNumber) {
    result.error = "input is not a complete SPIR-V module";
    return result;
  }

  try {
    std::vector<uint32_t> words(spirv.begin(), spirv.end());
    spirv_cross::CompilerGLSL compiler(std::move(words));
    if (!ValidateStage(compiler, expected_stage, result.error)) return result;

    const auto original_resources = compiler.get_shader_resources();
    if (!CheckNoUnsupportedResources(original_resources, result.error)) return result;

    if (!ValidateAndRemapUniformBuffers(compiler, original_resources, expected_stage, result)) {
      return result;
    }
    if (!ValidateAndRemapStorageBuffers(compiler, original_resources, expected_stage, result)) {
      return result;
    }
    if (!ValidateAndRemapStorageImages(compiler, original_resources, expected_stage, result)) {
      return result;
    }

    // Combine the translator's separate image and sampler variables only after
    // collecting the source bindings, so the renderer can still map each
    // generated sampler back to its original texture/sampler descriptor pair.
    compiler.build_dummy_sampler_for_combined_images();
    compiler.build_combined_image_samplers();
    if (!ValidateAndRemapCombinedTextures(compiler, original_resources, expected_stage, result)) {
      return result;
    }

    spirv_cross::CompilerGLSL::Options options;
    options.version = 460;
    options.es = false;
    // UAM rejects GL_EXT_scalar_block_layout. Repack the translator's system
    // constants to std140 above, then emit ordinary GLSL blocks.
    options.vulkan_semantics = false;
    options.enable_420pack_extension = false;
    compiler.set_common_options(options);

    result.source = compiler.compile();
    if (result.source.empty()) {
      result.error = "SPIRV-Cross produced empty GLSL output";
    } else {
      CheckUamGlslCompatibility(result.source, result);
    }
  } catch (const spirv_cross::CompilerError& exception) {
    result.error = std::string("SPIRV-Cross rejected the shader: ") + exception.what();
  } catch (const std::exception& exception) {
    result.error = std::string("shader conversion failed: ") + exception.what();
  }
  return result;
}

}  // namespace fear_nx::deko
