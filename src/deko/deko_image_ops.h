// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <deko3d.h>
#include <array>
#include <cstdint>
#include <string>
#include "deko_image_barriers.h"

namespace fear_nx::deko {

// Stage one deliberately excludes SNORM (two representations of -1), float
// NaNs and depth/stencil packing. UNORM round-trips through texelFetch exactly.
inline bool Copy3DColorFormat(DkImageFormat format) {
  return format == DkImageFormat_RGBA8_Unorm || format == DkImageFormat_RGB10A2_Unorm;
}
inline uint32_t ColorImageFlags(DkImageFormat format, bool compression) {
  return DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine |
         (compression && Copy3DColorFormat(format) ? DkImageFlags_HwCompression : 0);
}
inline bool CoversImage(uint32_t width, uint32_t height, int32_t x, int32_t y,
                        uint32_t w, uint32_t h) {
  return width && height && x == 0 && y == 0 && w == width && h == height;
}
inline DkMultisampleState SceneSampleState(uint32_t samples) {
  DkMultisampleState state{};
  dkMultisampleStateDefaults(&state);
  if (samples == 1) return state;
  state.mode = state.rasterizerMode = samples == 4 ? DkMsMode_4x : DkMsMode_2x;
  static constexpr DkSampleLocation two[] = {{0.25f,0.25f},{0.75f,0.75f}};
  static constexpr DkSampleLocation four[] = {{0.375f,0.125f},{0.875f,0.375f},{0.125f,0.625f},{0.625f,0.875f}};
  dkMultisampleStateSetLocations(&state, samples == 4 ? four : two, samples);
  return state;
}
inline bool FitsImage(uint32_t width, uint32_t height, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h) {
  return w && h && x < width && y < height && w <= width-x && h <= height-y;
}
inline bool CanDeferClear(uint32_t width, uint32_t height, int32_t x, int32_t y,
                          uint32_t w, uint32_t h, bool depth, bool clear_z, uint32_t mask) {
  return CoversImage(width,height,x,y,w,h) && (!depth || (clear_z && mask == 0xFF));
}
inline constexpr char kImageCopyVertex[] = R"glsl(#version 460
void main() {
  vec2 p=vec2(float((gl_VertexID&1)<<2)-1.0,float((gl_VertexID&2)<<1)-1.0);
  gl_Position=vec4(p,0.0,1.0);
}
)glsl";
inline constexpr char kImageCopyFragment[] = R"glsl(#version 460
layout(binding=0) uniform sampler2D source_image;
layout(std140,binding=0) uniform CopyParams { ivec4 offset; } params;
layout(location=0) out vec4 color;
void main() { color=texelFetch(source_image,ivec2(gl_FragCoord.xy)+params.offset.xy,0); }
)glsl";
inline constexpr char kSceneResolveFragment[] = R"glsl(#version 460
layout(binding=0) uniform sampler2DMS source_image;
layout(std140,binding=0) uniform CopyParams { ivec4 offset; } params;
layout(location=0) out vec4 color;
void main() {
  ivec2 p=ivec2(gl_FragCoord.xy)+params.offset.xy;
  int first=params.offset.z, count=params.offset.w;
  vec4 sum=texelFetch(source_image,p,first);
  if(count>=2) sum+=texelFetch(source_image,p,first+1);
  if(count==4) sum+=texelFetch(source_image,p,2)+texelFetch(source_image,p,3);
  color=sum/float(count);
}
)glsl";
inline constexpr char kDepthStencilCopyFragment[] = R"glsl(#version 460
#extension GL_ARB_shader_stencil_export : require
#ifdef FEAR_MULTISAMPLED
layout(binding=0) uniform sampler2DMS depth_image;
layout(binding=1) uniform usampler2DMS stencil_image;
#define SAMPLE_INDEX gl_SampleID
#else
layout(binding=0) uniform sampler2D depth_image;
layout(binding=1) uniform usampler2D stencil_image;
#define SAMPLE_INDEX 0
#endif
layout(std140,binding=0) uniform CopyParams { ivec4 offset; } params;
void main() {
  ivec2 p=ivec2(gl_FragCoord.xy)+params.offset.xy;
  gl_FragDepth=texelFetch(depth_image,p,SAMPLE_INDEX).r;
  gl_FragStencilRefARB=int(texelFetch(stencil_image,p,SAMPLE_INDEX).r);
}
)glsl";
inline constexpr char kMultisampleCopyFragment[] = R"glsl(#version 460
layout(binding=0) uniform sampler2DMS source_image;
layout(std140,binding=0) uniform CopyParams { ivec4 offset; } params;
layout(location=0) out vec4 color;
void main() {
  color=texelFetch(source_image,ivec2(gl_FragCoord.xy)+params.offset.xy,gl_SampleID);
}
)glsl";
inline std::string MultisampledShader(std::string source) {
  source.insert(source.find('\n')+1, "#define FEAR_MULTISAMPLED\n");
  return source;
}
} // namespace fear_nx::deko
