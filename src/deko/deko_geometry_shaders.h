// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <vector>

#include "native/native_geometry_shaders.h"

namespace fear_nx::deko {

// Builds the ReXGlue primitive-expansion geometry shader as SPIR-V. The caller
// can pass the words through SPIRV-Cross and UAM without creating Vulkan state.
// An empty vector means the geometry key could not be generated.
std::vector<uint32_t> CreateGeometryShaderSpirv(native::GeometryShaderKey key);

}  // namespace fear_nx::deko
