#version 450
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copies the selected depth sample into the R32_SFLOAT image used by guest
// depth-resolve fetches. Depth is selected, never averaged.
//
// Rebuild depth_msaa_to_float_cs.h after editing:
//   glslangValidator -S comp -V --target-env vulkan1.1
//     --vn kDepthMsaaToFloatCs -o depth_msaa_to_float_cs.h depth_msaa_to_float.cs.glsl
#extension GL_EXT_samplerless_texture_functions : require
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform texture2DMS depth_buffer;
layout(set = 0, binding = 1, r32f) uniform writeonly image2D dest;
layout(push_constant) uniform Params {
  ivec2 source_origin;
  ivec2 dest_origin;
  uvec2 size;
  uint sample_index;
};
void main() {
  uvec2 p = gl_GlobalInvocationID.xy;
  if (p.x >= size.x || p.y >= size.y) {
    return;
  }
  ivec2 source_pixel = source_origin + ivec2(p);
  ivec2 dest_pixel = dest_origin + ivec2(p);
  float depth = texelFetch(depth_buffer, source_pixel, int(sample_index)).r;
  imageStore(dest, dest_pixel, vec4(depth));
}