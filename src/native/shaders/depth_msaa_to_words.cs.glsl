#version 450
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Resolves the selected MSAA depth sample into the float image buffer and the
// packed (depth24 << 8) | stencil guest words used by non-depth texture reads.
// Depth selects one sample; it is never averaged.
//
// Rebuild depth_msaa_to_words_cs.h after editing:
//   glslangValidator -S comp -V --target-env vulkan1.1
//     --vn kDepthMsaaToWordsCs -o depth_msaa_to_words_cs.h depth_msaa_to_words.cs.glsl
#extension GL_EXT_samplerless_texture_functions : require
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform texture2DMS depth_buffer;
layout(set = 0, binding = 1) uniform utexture2DMS stencil_buffer;
layout(set = 0, binding = 2, std430) buffer Data { uint words[]; } data;
layout(push_constant) uniform Params {
  ivec2 source_origin;
  uvec2 size;
  uint sample_index;
  uint packed_word_offset;
};
void main() {
  uvec2 p = gl_GlobalInvocationID.xy;
  if (p.x >= size.x || p.y >= size.y) {
    return;
  }
  ivec2 source_pixel = source_origin + ivec2(p);
  uint index = p.y * size.x + p.x;
  float depth = texelFetch(depth_buffer, source_pixel, int(sample_index)).r;
  uint stencil = texelFetch(stencil_buffer, source_pixel, int(sample_index)).r & 0xFFu;
  // Adding 0.5 can round 0xFFFFFF + 0.5 to 2^24 in float32 at depth 1.0.
  uint depth24 = uint(roundEven(clamp(depth, 0.0, 1.0) * 16777215.0));
  data.words[index] = floatBitsToUint(depth);
  data.words[packed_word_offset + index] = (depth24 << 8u) | stencil;
}
