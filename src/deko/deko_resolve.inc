// SPDX-License-Identifier: GPL-3.0-or-later
// Resolve and exact, register-proven EDRAM clear aliases for the Deko backend.

namespace {

bool DekoHasStencil(VkFormat format) {
  return format == VK_FORMAT_D24_UNORM_S8_UINT ||
         format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

constexpr char kDepthResolveGlsl[] = R"glsl(#version 460
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0) uniform sampler2D depth_buffer;
layout(binding = 1) uniform usampler2D stencil_buffer;
layout(binding = 0, r32f) writeonly uniform image2D depth_output;
layout(binding = 1, rgba8) writeonly uniform image2D words_output;
layout(binding = 0) uniform Params {
  ivec4 source_destination_origin;
  uvec4 extent_flags;
} params;
void main() {
  uvec2 p = gl_GlobalInvocationID.xy;
  if (p.x >= params.extent_flags.x || p.y >= params.extent_flags.y) return;
  ivec2 source_pixel = params.source_destination_origin.xy + ivec2(p);
  ivec2 destination_pixel = params.source_destination_origin.zw + ivec2(p);
  float depth = clamp(texelFetch(depth_buffer, source_pixel, 0).r, 0.0, 1.0);
  imageStore(depth_output, destination_pixel, vec4(depth, 0.0, 0.0, 1.0));
  uint depth24 = uint(depth * 16777215.0 + 0.5);
  uint stencil = texelFetch(stencil_buffer, source_pixel, 0).r & 0xFFu;
  uvec4 bytes = uvec4(stencil, depth24 & 0xFFu, (depth24 >> 8u) & 0xFFu,
                      (depth24 >> 16u) & 0xFFu);
  imageStore(words_output, destination_pixel, vec4(bytes) * (1.0 / 255.0));
}
)glsl";

constexpr char kDepthFloatResolveGlsl[] = R"glsl(#version 460
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0) uniform sampler2D depth_buffer;
layout(binding = 0, r32f) writeonly uniform image2D depth_output;
layout(binding = 0) uniform Params {
  ivec4 source_destination_origin;
  uvec4 extent_flags;
} params;
void main() {
  uvec2 p = gl_GlobalInvocationID.xy;
  if (p.x >= params.extent_flags.x || p.y >= params.extent_flags.y) return;
  ivec2 source_pixel = params.source_destination_origin.xy + ivec2(p);
  ivec2 destination_pixel = params.source_destination_origin.zw + ivec2(p);
  float depth = clamp(texelFetch(depth_buffer, source_pixel, 0).r, 0.0, 1.0);
  imageStore(depth_output, destination_pixel, vec4(depth, 0.0, 0.0, 1.0));
}
)glsl";

StencilClearLayout MakeStencilLayout(const RenderTarget& target) {
  return {target.key.base_tiles, target.key.pitch,
          target.key.is_depth ? target.key.guest_msaa : target.last_draw_guest_msaa,
          target.key.is_depth ? target.key.format : target.guest_format,
          target.width, target.height};
}

}  // namespace

bool Renderer::Impl::DispatchDepthResolve(RenderTarget& source, ResolveImage& depth,
                                          ResolveImage* words, uint32_t source_x,
                                          uint32_t source_y, uint32_t destination_x,
                                          uint32_t destination_y, uint32_t width,
                                          uint32_t height, bool unorm_guest) {
  if (!width || !height || !DekoHasStencil(source.format)) {
    return false;
  }

  CompiledShader* shader = CompileText(
      unorm_guest ? kDepthResolveGlsl : kDepthFloatResolveGlsl, ShaderStage::kCompute);
  if (!shader) return false;

  const ClearRect read_area{source_x,source_y,width,height},write_area{destination_x,destination_y,width,height};
  MaterializeClear(source.image,&read_area);
  MaterializeClear(depth.image,&write_area);
  if(words)MaterializeClear(words->image,&write_area);
  std::optional<uint32_t> depth_view;
  std::optional<uint32_t> depth_output_view;
  std::optional<uint32_t> stencil_view;
  std::optional<uint32_t> words_view;
  if (unorm_guest && !words) return false;
  for (uint32_t attempt = 0; attempt < 2; ++attempt) {
    depth_view = textures->GetImageView(source.image, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
                                        DkImageType_2D, DkDsSource_Depth, false);
    depth_output_view = textures->GetImageView(depth.image, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
                                               DkImageType_2D, DkDsSource_Depth, true);
    if (unorm_guest) {
      stencil_view = textures->GetImageView(source.image, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
                                            DkImageType_2D, DkDsSource_Stencil, false);
      words_view = textures->GetImageView(words->image, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
                                          DkImageType_2D, DkDsSource_Depth, true);
    }
    if (depth_view && depth_output_view &&
        (!unorm_guest || (stencil_view && words_view))) break;
    if (!attempt && textures->NeedsFlushRetry()) {
      Flush();
      Begin();
      textures->ClearFlushRetry();
      continue;
    }
    retry = textures->NeedsFlushRetry();
    return false;
  }

  struct Params {
    int32_t source_destination_origin[4];
    uint32_t extent_flags[4];
  } params{{int32_t(source_x), int32_t(source_y), int32_t(destination_x),
            int32_t(destination_y)},
           {width, height, uint32_t(unorm_guest), 0}};
  auto upload = core.Reservar(sizeof(params), 256);
  if (!upload) {
    Flush();
    Begin();
    upload = core.Reservar(sizeof(params), 256);
  }
  if (!upload) {
    retry = true;
    return false;
  }
  std::memcpy(upload.cpu, &params, sizeof(params));

  core.InvalidarCodigoSiHaceFalta();
  Barrier();
  command_state.Invalidate();
  DkCmdBuf cmd = core.comandos();
  const DkShader* shaders[] = {&shader->shader};
  dkCmdBufBindShaders(cmd, DkStageFlag_Compute, shaders, 1);
  dkCmdBufBindUniformBuffer(cmd, DkStage_Compute, 0, upload.gpu, sizeof(params));

  const DkResHandle sampled[2] = {
      dkMakeTextureHandle(*depth_view, 0),
      unorm_guest ? dkMakeTextureHandle(*stencil_view, 0) : dkMakeTextureHandle(0, 0)};
  dkCmdBufBindTextures(cmd, DkStage_Compute, 0, sampled, unorm_guest ? 2 : 1);
  const DkResHandle storage[2] = {
      dkMakeImageHandle(*depth_output_view),
      unorm_guest ? dkMakeImageHandle(*words_view) : dkMakeImageHandle(0)};
  dkCmdBufBindImages(cmd, DkStage_Compute, 0, storage, unorm_guest ? 2 : 1);
  dkCmdBufDispatchCompute(cmd, (width + 7) / 8, (height + 7) / 8, 1);
  depth.image.pending_clear.Written(write_area);
  if(words)words->image.pending_clear.Written(write_area);
  Barrier();
  textures->Touch(source.image, work);
  textures->Touch(depth.image, work);textures->Changed(depth.image);
  if (words) {textures->Touch(words->image, work);textures->Changed(words->image);}
  return true;
}

void Renderer::Impl::MirrorClears() {
  const DrawSetup& st = setup;
  const RegisterFile& r = *regs;
  RenderTarget* source = st.targets.depth;
  if (!source || !DekoHasStencil(source->format)) return;

  const auto initiator = r.Get<reg::VGT_DRAW_INITIATOR>();
  const auto& depth = st.depth_control;
  const auto ref = r.Get<reg::RB_STENCILREFMASK>();
  const bool shared_rectangle = !st.ps && !st.primitive_polygonal && Capabilities{}.depthClamp;
  // This backend's rectangle-list expansion is non-polygonal. Mirror only the
  // exact unconditional stencil replacement used by the native path.
  if (shared_rectangle && !depth.z_enable && depth.stencil_enable &&
      depth.stencilfunc == xenos::CompareFunction::kAlways &&
      depth.stencilzpass == xenos::StencilOp::kReplace &&
      initiator.prim_type == xenos::PrimitiveType::kRectangleList &&
      initiator.source_select == xenos::SourceSelect::kAutoIndex &&
      initiator.num_indices == 3 && r.Get<reg::PA_CL_CLIP_CNTL>().clip_disable && st.vs &&
      !(st.vs->writes_point_size_edge_flag_kill_vertex() & 0b100) &&
      rex::graphics::ShaderInterpreter::CanInterpretShader(*st.vs) &&
      ref.stencilwritemask == 0xFF && ref.stencilref == 0 &&
      source->key.guest_msaa <= 2 &&
      (r[rex::graphics::XE_GPU_REG_PA_SC_AA_MASK] & 0xFFFFu) == 0xFFFFu) {
    const StencilClearLayout source_layout = MakeStencilLayout(*source);
    bool has_alias = false;
    for (const auto& entry : render_targets) {
      const RenderTarget& target = *entry.second;
      StencilClearRect mapped{};
      if (&target != source && target.key.is_depth && DekoHasStencil(target.format) &&
          MapStencilClearRectangle(source_layout, MakeStencilLayout(target),
                                   {0, 0, int32_t(source->width), int32_t(source->height)},
                                   mapped)) {
        has_alias = true;
        break;
      }
    }
    StencilClearRect rect{};
    if (has_alias && GetFastClearRectangle(rect, false)) {
      for (auto& entry : render_targets) {
        RenderTarget& target = *entry.second;
        StencilClearRect mapped{};
        if (&target == source || !target.key.is_depth || !DekoHasStencil(target.format) ||
            !MapStencilClearRectangle(source_layout, MakeStencilLayout(target), rect, mapped)) {
          continue;
        }
        VkRect2D clear_rect{{mapped.x0, mapped.y0},
                            {uint32_t(mapped.x1 - mapped.x0),
                             uint32_t(mapped.y1 - mapped.y0)}};
        Clear(target.image, true, 0.0f, uint8_t(ref.stencilref), 0xFF, nullptr,
              clear_rect, false);
      }
    }
  }

  // D3D9 clears RGBA8 EDRAM through D24S8 by drawing depth and stencil zero.
  // Only identical guest sample layouts and whole physical EDRAM tiles alias.
  if (source->key.format != uint32_t(xenos::DepthRenderTargetFormat::kD24S8) ||
      !shared_rectangle || !depth.z_enable || !depth.z_write_enable ||
      depth.zfunc != xenos::CompareFunction::kAlways || !depth.stencil_enable ||
      depth.stencilfunc != xenos::CompareFunction::kAlways ||
      depth.stencilzpass != xenos::StencilOp::kReplace ||
      initiator.prim_type != xenos::PrimitiveType::kRectangleList ||
      initiator.source_select != xenos::SourceSelect::kAutoIndex || initiator.num_indices != 3 ||
      !r.Get<reg::PA_CL_CLIP_CNTL>().clip_disable || !st.vs ||
      (st.vs->writes_point_size_edge_flag_kill_vertex() & 0b100) ||
      !rex::graphics::ShaderInterpreter::CanInterpretShader(*st.vs) || st.bias_constant != 0.0f ||
      st.bias_slope != 0.0f || st.vp.minDepth != 0.0f || st.vp.maxDepth != 1.0f ||
      ref.stencilwritemask != 0xFF || ref.stencilref != 0 || source->key.guest_msaa > 2 ||
      (r[rex::graphics::XE_GPU_REG_PA_SC_AA_MASK] & 0xFFFFu) != 0xFFFFu) {
    return;
  }
  for (const RenderTarget* color : st.targets.color) {
    if (color) return;
  }

  const StencilClearLayout source_layout = MakeStencilLayout(*source);
  bool has_alias = false;
  for (const auto& entry : render_targets) {
    const RenderTarget& target = *entry.second;
    if (target.key.is_depth || target.guest_format >
                                   uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) ||
        target.format != VK_FORMAT_R8G8B8A8_UNORM ||
        target.last_draw_guest_msaa != source->key.guest_msaa ||
        target.key.base_tiles != source->key.base_tiles || target.key.pitch != source->key.pitch) {
      continue;
    }
    StencilClearRect mapped{};
    if (MapZeroDepthColorClearRectangle(source_layout, MakeStencilLayout(target),
                                        {0, 0, int32_t(source->width),
                                         int32_t(source->height)},
                                        mapped)) {
      has_alias = true;
      break;
    }
  }
  StencilClearRect rect{};
  if (!has_alias || !GetFastClearRectangle(rect, true)) return;
  const float zero[4] = {};
  for (auto& entry : render_targets) {
    RenderTarget& target = *entry.second;
    StencilClearRect mapped{};
    if (target.key.is_depth || target.guest_format >
                                   uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) ||
        target.format != VK_FORMAT_R8G8B8A8_UNORM ||
        target.last_draw_guest_msaa != source->key.guest_msaa ||
        !MapZeroDepthColorClearRectangle(source_layout, MakeStencilLayout(target), rect,
                                         mapped)) {
      continue;
    }
    VkRect2D clear_rect{{mapped.x0, mapped.y0},
                        {uint32_t(mapped.x1 - mapped.x0),
                         uint32_t(mapped.y1 - mapped.y0)}};
    Clear(target.image, false, 0.0f, 0, 0xFF, zero, clear_rect);
  }
}

void Renderer::Impl::Resolve() {
  const RegisterFile& r = *regs;
  const auto copy_control = r.Get<reg::RB_COPY_CONTROL>();
  const auto surface_info = r.Get<reg::RB_SURFACE_INFO>();
  const xenos::xe_gpu_vertex_fetch_t fetch = r.GetVertexFetch(0);
  if (fetch.type != xenos::FetchConstantType::kVertex || fetch.size != 6) return;

  const float* vertices = memory->TranslatePhysical<const float*>(fetch.address << 2);
  if (!vertices) return;
  const float half_pixel = r.Get<reg::PA_SU_VTX_CNTL>().pix_center ==
                                   xenos::PixelCenter::kD3DZero
                               ? 0.5f
                               : 0.0f;
  int32_t fixed[6];
  for (uint32_t i = 0; i < 6; ++i) {
    const float value = xenos::GpuSwap(vertices[i], fetch.endian) + half_pixel;
    if (!std::isfinite(value) || std::abs(value) > 32768.0f) return;
    fixed[i] = int32_t(value * 256.0f);
  }
  int32_t x0 = (std::min({fixed[0], fixed[2], fixed[4]}) + 127) >> 8;
  int32_t y0 = (std::min({fixed[1], fixed[3], fixed[5]}) + 127) >> 8;
  int32_t x1 = (std::max({fixed[0], fixed[2], fixed[4]}) + 127) >> 8;
  int32_t y1 = (std::max({fixed[1], fixed[3], fixed[5]}) + 127) >> 8;
  if (r.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
    const auto offset = r.Get<reg::PA_SC_WINDOW_OFFSET>();
    x0 += offset.window_x_offset;
    x1 += offset.window_x_offset;
    y0 += offset.window_y_offset;
    y1 += offset.window_y_offset;
  }
  draw_util::Scissor scissor{};
  draw_util::GetScissor(r, scissor, false);
  x0 = std::clamp(x0, int32_t(scissor.offset[0]),
                  int32_t(scissor.offset[0] + scissor.extent[0]));
  x1 = std::clamp(x1, int32_t(scissor.offset[0]),
                  int32_t(scissor.offset[0] + scissor.extent[0]));
  y0 = std::clamp(y0, int32_t(scissor.offset[1]),
                  int32_t(scissor.offset[1] + scissor.extent[1]));
  y1 = std::clamp(y1, int32_t(scissor.offset[1]),
                  int32_t(scissor.offset[1] + scissor.extent[1]));
  x0 &= ~7;
  y0 &= ~7;
  x1 = rex::align(x1, 8);
  y1 = rex::align(y1, 8);
  x1 = std::min(x1, int32_t(surface_info.surface_pitch));
  if (x0 >= x1 || y0 >= y1 || x1 <= 0 || y1 <= 0) return;

  const bool is_depth = copy_control.copy_src_select >= 4;
  const uint32_t host_msaa = HostMsaa(surface_info.msaa_samples);
  RenderTargetKey source_key{};
  source_key.pitch = surface_info.surface_pitch;
  source_key.msaa = host_msaa;
  source_key.guest_msaa = is_depth ? uint32_t(surface_info.msaa_samples) : 0;
  if (is_depth) {
    const auto depth_info = r.Get<reg::RB_DEPTH_INFO>();
    source_key.base_tiles = depth_info.depth_base;
    source_key.format = uint32_t(depth_info.depth_format);
    source_key.is_depth = 1;
  } else {
    const auto color_info = r.Get<reg::RB_COLOR_INFO>(
        reg::RB_COLOR_INFO::rt_register_indices[copy_control.copy_src_select]);
    source_key.base_tiles = color_info.color_base;
    source_key.format = uint32_t(color_info.color_format);
  }
  RenderTarget* source = GetRenderTarget(source_key, uint32_t(y1));
  if (!source) return;
  x0 = std::max(x0, 0);
  y0 = std::max(y0, 0);
  x1 = std::min(x1, int32_t(source->width));
  y1 = std::min(y1, int32_t(source->height));
  if (x0 >= x1 || y0 >= y1) return;
  const uint32_t width = uint32_t(x1 - x0);
  const uint32_t height = uint32_t(y1 - y0);
  ++stats().resolves;

  const uint32_t dest_base = r[rex::graphics::XE_GPU_REG_RB_COPY_DEST_BASE];
  const auto dest_info = r.Get<reg::RB_COPY_DEST_INFO>();
  const bool copy = dest_base != 0 &&
                    (copy_control.copy_command == xenos::CopyCommand::kRaw ||
                     copy_control.copy_command == xenos::CopyCommand::kConvert);
  if (copy) {
    const uint32_t resolve_address = dest_base & 0x1FFFFFFFu;
    uint32_t dest_width = std::min(uint32_t(x1), 8192u);
    uint32_t dest_height = std::min(uint32_t(y0) + height, 8192u);
    uint32_t dest_x = uint32_t(x0);
    uint32_t dest_y = uint32_t(y0);
    uint32_t root_address = resolve_address;
    resolve_layout::Point origin{};
    std::optional<resolve_layout::SharedWrite> shared_write;

    const auto copy_pitch = r.Get<reg::RB_COPY_DEST_PITCH>();
    const auto window_scissor = r.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
    const auto window_offset = r.Get<reg::PA_SC_WINDOW_OFFSET>();
    const auto sc_mode = r.Get<reg::PA_SU_SC_MODE_CNTL>();
    const resolve_layout::Point tile_origin{uint32_t(window_scissor.tl_x),
                                            uint32_t(window_scissor.tl_y)};
    const uint32_t full_width = uint32_t(copy_pitch.copy_dest_pitch);
    const uint32_t full_height = uint32_t(copy_pitch.copy_dest_height);
    bool color_format_supported = !is_depth &&
        dest_info.copy_dest_format == xenos::ColorFormat::k_8_8_8_8 &&
        source->format == VK_FORMAT_R8G8B8A8_UNORM && !dest_info.copy_dest_array &&
        !dest_info.copy_dest_slice;
    bool window_mapping_valid = tile_origin.x == 0 && tile_origin.y == 0;
    if (!is_depth && color_format_supported && (tile_origin.x || tile_origin.y) &&
        !window_scissor.window_offset_disable && sc_mode.vtx_window_offset_enable &&
        window_offset.window_x_offset == -int32_t(tile_origin.x) &&
        window_offset.window_y_offset == -int32_t(tile_origin.y)) {
      window_mapping_valid = true;
    }
    if (is_depth) {
      window_mapping_valid = !window_scissor.window_offset_disable &&
          sc_mode.vtx_window_offset_enable &&
          window_offset.window_x_offset == -int32_t(tile_origin.x) &&
          window_offset.window_y_offset == -int32_t(tile_origin.y);
    }
    const bool supported = !dest_info.copy_dest_array && !dest_info.copy_dest_slice &&
                           ((is_depth && host_msaa == 0) || color_format_supported);
    if (REXCVAR_GET(nx_native_shared_resolves) && supported) {
      shared_write = resolve_layout::PlanSharedWrite(
          resolve_address, full_width, full_height, tile_origin, window_mapping_valid,
          uint32_t(x0), uint32_t(y0), width, height, source->width, source->height);
    }
    if (shared_write) {
      root_address = shared_write->root_address;
      origin = shared_write->origin;
      dest_width = shared_write->width;
      dest_height = shared_write->height;
      dest_x = shared_write->region.destination_x;
      dest_y = shared_write->region.destination_y;
    } else {
      // A write outside the verified shared layout remains address-local.
      root_address = resolve_address;
      origin = {};
    }

    const DkImageFormat color_format = NativeFormat(source->format);
    const uint32_t format_key = is_depth ? source->key.format : source->guest_format;
    const uint32_t float_root = is_depth ? (root_address & ~1u) : root_address;
    auto ensure_resolve = [&](uint32_t address, DkImageFormat format, uint32_t guest_format,
                              bool depth_image, bool words_image) -> ResolveImage* {
      address &= 0x1FFFFFFFu;
      auto found = resolves.find(address);
      auto matches = [&](const ResolveImage& image) {
        return image.image.format == format && image.image.width >= dest_width &&
               image.image.height >= dest_height && image.guest_format == guest_format &&
               image.is_depth == depth_image && image.depth_words == words_image;
      };
      if (found != resolves.end() && matches(*found->second)) {
        found->second->last_frame = frame;
        return found->second.get();
      }

      std::optional<Image> allocation;
      for (uint32_t attempt = 0; attempt < 2 && !allocation; ++attempt) {
        allocation = textures->CreateImage(
            format, dest_width, dest_height, DkImageType_2DArray, 1, 1,
            depth_image ? (DkImageFlags_UsageRender | DkImageFlags_UsageLoadStore | DkImageFlags_Usage2DEngine) :
                ColorImageFlags(format,color_compression_enabled));
        if (!allocation && attempt == 0 && textures->NeedsFlushRetry()) {
          Flush();
          Begin();
        }
      }
      if (!allocation) return nullptr;
      VkRect2D whole{{0, 0}, {dest_width, dest_height}};
      const float zero[4] = {};
      Clear(*allocation, false, 0.0f, 0, 0xFF, zero, whole);

      if (found == resolves.end()) {
        auto image = std::make_unique<ResolveImage>();
        image->image = std::move(*allocation);
        image->address = address;
        image->guest_format = guest_format;
        image->is_depth = depth_image;
        image->depth_words = words_image;
        image->origin_x = 0;
        image->origin_y = 0;
        image->version = ++resolve_version_clock;
        image->last_frame = frame;
        return resolves.emplace(address, std::move(image)).first->second.get();
      }

      ResolveImage& old = *found->second;
      BeforeImageWrite(old.image);
      const uint32_t retained_width = std::min(old.image.width, allocation->width);
      const uint32_t retained_height = std::min(old.image.height, allocation->height);
      const bool same_storage = old.image.format == format && old.is_depth == depth_image &&
                                old.depth_words == words_image;
      if (same_storage && retained_width && retained_height) {
        Copy(old.image, *allocation, 0, 0, 0, 0, retained_width, retained_height);
      }
      textures->FreeImage(old.image);
      old.image = std::move(*allocation);
      old.address = address;
      old.guest_format = guest_format;
      old.is_depth = depth_image;
      old.depth_words = words_image;
      old.origin_x = 0;
      old.origin_y = 0;
      old.version = ++resolve_version_clock;
      old.last_frame = frame;
      return &old;
    };

    ResolveImage* destination = nullptr;
    ResolveImage* words_destination = nullptr;
    if (is_depth) {
      destination = ensure_resolve(float_root, DkImageFormat_R32_Float, format_key, true, false);
      if (format_key == uint32_t(xenos::DepthRenderTargetFormat::kD24S8)) {
        words_destination = ensure_resolve(float_root | 1u, DkImageFormat_RGBA8_Unorm,
                                            format_key, true, true);
      }
    } else {
      destination = ensure_resolve(float_root, color_format, format_key, false, false);
    }
    if (!destination ||
        (is_depth && format_key == uint32_t(xenos::DepthRenderTargetFormat::kD24S8) &&
         !words_destination)) {
      REXLOG_ERROR("[deko] resolve destination allocation failed at {:08X}", float_root);
    } else {
      auto set_alias = [&](uint32_t alias_address, uint32_t root, resolve_layout::Point p) {
        resolve_aliases[alias_address & 0x1FFFFFFFu] = {root & 0x1FFFFFFFu, p.x, p.y};
      };

      if (is_depth) {
        destination->image.host_format_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR;
        if (words_destination) {
          words_destination->image.host_format_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
        }
        if (DispatchDepthResolve(*source, *destination, words_destination,
                                 uint32_t(x0), uint32_t(y0), dest_x, dest_y,
                                 width, height,
                                 format_key == uint32_t(xenos::DepthRenderTargetFormat::kD24S8))) {
          destination->version = ++resolve_version_clock;
          destination->last_frame = frame;
          if (words_destination) {
            words_destination->version = ++resolve_version_clock;
            words_destination->last_frame = frame;
          }
          set_alias(resolve_address, float_root, origin);
          if (words_destination) set_alias(resolve_address | 1u, float_root | 1u, origin);
        }
      } else {
        destination->image.host_format_swizzle = dest_info.copy_dest_swap
            ? XE_GPU_MAKE_TEXTURE_SWIZZLE(B, G, R, A)
            : xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
        if(!DeferFrontbuffer(*source,*destination,float_root,uint32_t(x0),uint32_t(y0),dest_x,dest_y,
                            width,height,root_address!=resolve_address||origin.x||origin.y,dest_info.copy_dest_swap))
          Copy(source->image, destination->image, uint32_t(x0), uint32_t(y0), dest_x, dest_y,
               width, height);
        destination->version = ++resolve_version_clock;
        destination->last_frame = frame;
        set_alias(resolve_address, float_root, origin);
      }
    }
  }

  const bool clear_depth = copy_control.depth_clear_enable != 0;
  const bool clear_color = !is_depth && copy_control.color_clear_enable != 0;
  if (!clear_depth && !clear_color) return;

  RenderTarget* depth_target = nullptr;
  RenderTarget* color_target = clear_color ? source : nullptr;
  if (clear_depth) {
    const auto depth_info = r.Get<reg::RB_DEPTH_INFO>();
    RenderTargetKey key{};
    key.base_tiles = depth_info.depth_base;
    key.pitch = surface_info.surface_pitch;
    key.format = uint32_t(depth_info.depth_format);
    key.msaa = host_msaa;
    key.guest_msaa = uint32_t(surface_info.msaa_samples);
    key.is_depth = 1;
    depth_target = GetRenderTarget(key, uint32_t(y1));
  }

  if (!depth_target && !color_target) return;

  const uint32_t clear_width = depth_target
      ? (color_target ? std::min(depth_target->width, color_target->width) : depth_target->width)
      : color_target->width;
  const uint32_t clear_height = depth_target
      ? (color_target ? std::min(depth_target->height, color_target->height) : depth_target->height)
      : color_target->height;
  const uint32_t clear_w = std::min(width, clear_width - std::min(uint32_t(x0), clear_width));
  const uint32_t clear_h = std::min(height, clear_height - std::min(uint32_t(y0), clear_height));
  if (!clear_w || !clear_h) return;
  const VkRect2D clear_rect{{x0, y0}, {clear_w, clear_h}};
  if (depth_target) {
    const uint32_t clear = r[rex::graphics::XE_GPU_REG_RB_DEPTH_CLEAR];
    const uint32_t depth24 = clear >> 8;
    const float depth_value =
        xenos::DepthRenderTargetFormat(depth_target->key.format) ==
                xenos::DepthRenderTargetFormat::kD24FS8
            ? xenos::Float20e4To32(depth24) * 0.5f
            : xenos::UNorm24To32(depth24);
    Clear(depth_target->image, true, depth_value, uint8_t(clear & 0xFFu), 0xFF, nullptr,
          clear_rect);
  }
  if (color_target) {
    const uint64_t value = r[rex::graphics::XE_GPU_REG_RB_COLOR_CLEAR] |
                           (uint64_t(r[rex::graphics::XE_GPU_REG_RB_COLOR_CLEAR_LO]) << 32);
    float color[4]{};
    using F = xenos::ColorRenderTargetFormat;
    switch (F(color_target->guest_format)) {
      case F::k_8_8_8_8:
      case F::k_8_8_8_8_GAMMA:
        for (uint32_t i = 0; i < 4; ++i) color[i] = float((value >> (i * 8)) & 0xFFu) / 255.0f;
        break;
      case F::k_2_10_10_10:
      case F::k_2_10_10_10_AS_10_10_10_10:
        for (uint32_t i = 0; i < 3; ++i) color[i] = float((value >> (i * 10)) & 0x3FFu) / 1023.0f;
        color[3] = float((value >> 30) & 0x3u) / 3.0f;
        break;
      case F::k_2_10_10_10_FLOAT:
      case F::k_2_10_10_10_FLOAT_AS_16_16_16_16:
        for (uint32_t i = 0; i < 3; ++i) color[i] = xenos::Float7e3To32((value >> (i * 10)) & 0x3FFu);
        color[3] = float((value >> 30) & 0x3u) / 3.0f;
        break;
      case F::k_16_16_FLOAT:
      case F::k_16_16_16_16_FLOAT:
        for (uint32_t i = 0; i < 4; ++i) color[i] = rex::xenos_half_to_float(uint16_t(value >> (i * 16)));
        break;
      case F::k_16_16:
      case F::k_16_16_16_16:
        for (uint32_t i = 0; i < 4; ++i) {
          color[i] = std::max(float(int16_t(value >> (i * 16))) * (1.0f / 32767.0f), -1.0f);
        }
        break;
      case F::k_32_FLOAT: {
        const uint32_t bits = uint32_t(value);
        std::memcpy(&color[0], &bits, sizeof(bits));
      } break;
      case F::k_32_32_FLOAT: {
        const uint32_t bits[2] = {uint32_t(value), uint32_t(value >> 32)};
        std::memcpy(color, bits, sizeof(bits));
      } break;
      default:
        break;
    }
    Clear(color_target->image, false, 0.0f, 0, 0xFF, color, clear_rect);
  }
}
