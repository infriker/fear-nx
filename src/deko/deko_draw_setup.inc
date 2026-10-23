bool Renderer::Impl::BuildDrawSetup(uint32_t initiator_value) {
  DrawSetup& st = setup;
  st = DrawSetup();
  st.valid = true;
  st.state_generation = owner->state_generation_;
  st.fetch_generation = owner->fetch_generation_;
  st.rt_generation = rt_generation;
  st.active_vs = active_vs;
  st.active_ps = active_ps;
  st.initiator_state = initiator_value & 0xFFFF;
  st.skip_reason = UINT32_MAX;

  const RegisterFile& r = *regs;
  const Capabilities properties{};
  auto fail = [&](uint32_t reason) {
    st.skip_reason = reason;
    return false;
  };

  xenos::EdramMode edram_mode = r.Get<reg::RB_MODECONTROL>().edram_mode;
  auto surface_info = r.Get<reg::RB_SURFACE_INFO>();
  if (!surface_info.surface_pitch) return fail(0);
  SpirvShader* vs = active_vs;
  if (!vs) return fail(1);
  if (!vs->is_ucode_analyzed()) vs->AnalyzeUcode(disasm);
  if (vs->memexport_eM_written()) return fail(2);
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(r);
  if (!draw_util::IsRasterizationPotentiallyDone(r, primitive_polygonal)) return fail(3);
  SpirvShader* ps = nullptr;
  if (edram_mode == xenos::EdramMode::kColorDepth && active_ps) {
    ps = active_ps;
    if (!ps->is_ucode_analyzed()) ps->AnalyzeUcode(disasm);
    if (ps->memexport_eM_written()) return fail(2);
    if (!draw_util::IsPixelShaderNeededWithRasterization(*ps, r)) ps = nullptr;
  }
  // Xenos discards these probe pixels after hi-Z, before ordinary target
  // writes. VIZ/sample/extent answers are still faked by native_system, so
  // no real GPU query consumes this geometry. Do not prepare or rasterize
  // it as a normal draw. Keep the existing memexport exclusions above.
  // Revisit this shortcut when real VIZ or screen-extent queries are added.
  if (r.Get<reg::PA_SC_VIZ_QUERY>().kill_pix_post_hi_z) {
    return fail(kDiscardedVisibilityProbeSkipReason);
  }
  reg::RB_DEPTHCONTROL depth_control = draw_util::GetNormalizedDepthControl(r);
  uint32_t color_mask = draw_util::GetNormalizedColorMask(r, ps ? ps->writes_color_targets() : 0);

  reg::VGT_DRAW_INITIATOR initiator;
  initiator.value = initiator_value;
  xenos::PrimitiveType guest_type = initiator.prim_type;
  VkPrimitiveTopology topology;
  PipelineGeometryShader geometry = PipelineGeometryShader::kNone;
  switch (guest_type) {
    case xenos::PrimitiveType::kPointList:
      topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
      geometry = PipelineGeometryShader::kPointList;
      break;
    case xenos::PrimitiveType::kLineList:
      topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      break;
    case xenos::PrimitiveType::kLineStrip:
      topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      st.strip = true;
      break;
    case xenos::PrimitiveType::kLineLoop:
      topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      st.convert_loop = true;
      break;
    case xenos::PrimitiveType::kTriangleList:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      break;
    case xenos::PrimitiveType::kTriangleStrip:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      st.strip = true;
      break;
    case xenos::PrimitiveType::kTriangleFan:
    case xenos::PrimitiveType::kPolygon:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      st.convert_fan = true;
      break;
    case xenos::PrimitiveType::kRectangleList:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      geometry = PipelineGeometryShader::kRectangleList;
      break;
    case xenos::PrimitiveType::kQuadList:
      topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
      geometry = PipelineGeometryShader::kQuadList;
      break;
    default:
      return fail(4);
  }
  if (geometry != PipelineGeometryShader::kNone && !properties.geometryShader) return fail(5);
  bool indexed = initiator.source_select != xenos::SourceSelect::kAutoIndex;
  st.reset_enabled =
      indexed && st.strip && r.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena;

  // Shader modifications (VulkanPipelineCache::GetCurrent*ShaderModification).
  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask =
      ps ? (vs->writes_interpolators() &
            ps->GetInterpolatorInputMask(r.Get<reg::SQ_PROGRAM_CNTL>(),
                                         r.Get<reg::SQ_CONTEXT_MISC>(), ps_param_gen_pos))
         : 0;
  SpirvShaderTranslator::Modification vs_mod(translator->GetDefaultVertexShaderModification(
      vs->GetDynamicAddressableRegisterCount(r.Get<reg::SQ_PROGRAM_CNTL>().vs_num_reg),
      Shader::HostVertexShaderType::kVertex));
  vs_mod.vertex.interpolator_mask = interpolator_mask;
  auto pa_cl_clip_cntl = r.Get<reg::PA_CL_CLIP_CNTL>();
  uint32_t user_clip_planes = pa_cl_clip_cntl.clip_disable ? 0 : pa_cl_clip_cntl.ucp_ena;
  if (user_clip_planes) {
    if (pa_cl_clip_cntl.ucp_cull_only_ena && properties.shaderCullDistance) {
      vs_mod.vertex.user_clip_plane_cull = 1;
    } else if (!properties.shaderClipDistance && properties.shaderCullDistance) {
      vs_mod.vertex.user_clip_plane_cull = 1;
    } else if (!properties.shaderClipDistance) {
      user_clip_planes = 0;
    }
  }
  vs_mod.vertex.user_clip_plane_count = rex::bit_count(user_clip_planes);
  vs_mod.vertex.point_ps_ucp_mode = pa_cl_clip_cntl.ps_ucp_mode;
  vs_mod.vertex.output_point_parameters =
      uint32_t((vs->writes_point_size_edge_flag_kill_vertex() & 0b001) &&
               guest_type == xenos::PrimitiveType::kPointList);
  vs_mod.vertex.vertex_kill_and =
      uint32_t(properties.shaderCullDistance &&
               (vs->writes_point_size_edge_flag_kill_vertex() & 0b100) &&
               !pa_cl_clip_cntl.vtx_kill_or);
  SpirvShaderTranslator::Modification ps_mod(0);
  if (ps) {
    ps_mod = SpirvShaderTranslator::Modification(translator->GetDefaultPixelShaderModification(
        ps->GetDynamicAddressableRegisterCount(r.Get<reg::SQ_PROGRAM_CNTL>().ps_num_reg)));
    ps_mod.pixel.interpolator_mask = interpolator_mask;
    ps_mod.pixel.interpolators_centroid =
        interpolator_mask &
        ~xenos::GetInterpolatorSamplingPattern(surface_info.msaa_samples,
                                               r.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
                                               r.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);
    if (ps_param_gen_pos < xenos::kMaxInterpolators) {
      ps_mod.pixel.param_gen_enable = 1;
      ps_mod.pixel.param_gen_interpolator = ps_param_gen_pos;
      ps_mod.pixel.param_gen_point = uint32_t(guest_type == xenos::PrimitiveType::kPointList);
    }
    using DepthStencilMode = SpirvShaderTranslator::Modification::DepthStencilMode;
    if (ps->implicit_early_z_write_allowed() &&
        (!ps->writes_color_target(0) ||
         !draw_util::DoesCoverageDependOnAlpha(r.Get<reg::RB_COLORCONTROL>()))) {
      ps_mod.pixel.depth_stencil_mode = DepthStencilMode::kEarlyHint;
    } else {
      ps_mod.pixel.depth_stencil_mode = DepthStencilMode::kNoModifiers;
    }
  }
  auto* vs_translation =
      static_cast<SpirvShader::SpirvTranslation*>(vs->GetOrCreateTranslation(vs_mod.value));
  auto* ps_translation =
      ps ? static_cast<SpirvShader::SpirvTranslation*>(ps->GetOrCreateTranslation(ps_mod.value))
         : nullptr;
  {
    bool vs_pending = false, ps_pending = false;
    bool vs_ok = EnsureTranslated(vs_translation, &vs_pending);
    bool ps_ok = !ps_translation || EnsureTranslated(ps_translation, &ps_pending);
    if (!vs_ok || !ps_ok) {
      // Still being translated: look again on the next draw.
      if (vs_pending || ps_pending) st.valid = false;
      return fail(6);
    }
  }

  // Render targets: the color targets the draw writes and the depth buffer
  // if it is tested or written.
  bool depth_used = depth_control.z_enable || depth_control.stencil_enable;
  // The guest's sample count sizes the EDRAM below; the host's keys the targets.
  uint32_t guest_msaa = uint32_t(surface_info.msaa_samples);
  uint32_t msaa = HostMsaa(surface_info.msaa_samples);
  draw_util::GetScissor(r, st.scissor);
  draw_util::GetHostViewportInfo(r, 1, 1, false, properties.maxViewportDimensions[0],
                                 properties.maxViewportDimensions[1], true, depth_control, false,
                                 true, ps && ps->writes_depth(), st.viewport);
  // Rows the draw can touch: the viewport, cut by the scissor. The scissor
  // alone is often the 8192-pixel default and made every target 2048 tall.
  uint32_t needed_height = st.scissor.offset[1] + st.scissor.extent[1];
  if (st.viewport.xy_extent[1]) {
    needed_height =
        std::min(needed_height, st.viewport.xy_offset[1] + st.viewport.xy_extent[1]);
  }
  PassTargets& targets = st.targets;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!((color_mask >> (4 * i)) & 0xF)) continue;
    auto color_info = r.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    RenderTargetKey key = {};
    key.base_tiles = color_info.color_base;
    key.pitch = surface_info.surface_pitch;
    key.format = uint32_t(color_info.color_format);
    key.msaa = msaa;
    key.guest_msaa = 0;
    // The EDRAM below the base limits how tall the target can be.
    uint32_t pitch_tiles = (surface_info.surface_pitch * (guest_msaa >= 2 ? 2 : 1) + 79) / 80 *
                           (IsColorFormat64bpp(color_info.color_format) ? 2 : 1);
    uint32_t rows = pitch_tiles ? (2048 - std::min(key.base_tiles, 2047u)) / pitch_tiles *
                                      (guest_msaa >= 1 ? 8 : 16)
                                : 2048;
    targets.color[i] = GetRenderTarget(key, std::min(needed_height, rows));
    if (!targets.color[i]) return fail(7);
  }
  if (depth_used) {
    auto depth_info = r.Get<reg::RB_DEPTH_INFO>();
    RenderTargetKey key = {};
    key.base_tiles = depth_info.depth_base;
    key.pitch = surface_info.surface_pitch;
    key.format = uint32_t(depth_info.depth_format);
    key.msaa = msaa;
    key.guest_msaa = uint32_t(surface_info.msaa_samples);
    key.is_depth = 1;
    uint32_t pitch_tiles = (surface_info.surface_pitch * (guest_msaa >= 2 ? 2 : 1) + 79) / 80;
    uint32_t rows = pitch_tiles ? (2048 - std::min(key.base_tiles, 2047u)) / pitch_tiles *
                                      (guest_msaa >= 1 ? 8 : 16)
                                : 2048;
    targets.depth = GetRenderTarget(key, std::min(needed_height, rows));
    if (!targets.depth) return fail(7);
  }
  // Creating a render target above bumped rt_generation; this setup is built
  // against the new ones.
  st.rt_generation = rt_generation;
  if (targets.Empty()) return fail(7);

  // Layout.
  st.vs = vs;
  st.ps = ps;
  const SpirvShader* stage_shaders[2] = {vs, ps};
  for (uint32_t stage = 0; stage < 2; ++stage) {
    const SpirvShader* shader = stage_shaders[stage];
    if (!shader) continue;
    st.counts[stage * 2] = uint32_t(shader->GetTextureBindingsAfterTranslation().size());
    st.counts[stage * 2 + 1] = uint32_t(shader->GetSamplerBindingsAfterTranslation().size());
  }
  st.layout = GetPipelineLayout(st.counts[0], st.counts[1], st.counts[2], st.counts[3]);
  if (!st.layout) return fail(10);

  // Pipeline.
  PipelineKey key = {};
  key.vs_hash = vs->ucode_data_hash();
  key.vs_modification = vs_mod.value;
  key.ps_hash = ps ? ps->ucode_data_hash() : 0;
  key.ps_modification = ps ? ps_mod.value : 0;
  for (uint32_t i = 0; i < 4; ++i) {
    key.color_formats[i] = targets.color[i] ? uint32_t(targets.color[i]->format) : 0;
  }
  key.depth_format = targets.depth ? uint32_t(targets.depth->format) : 0;
  key.samples = 1u << msaa;
  key.topology = uint32_t(topology);
  key.primitive_restart = st.reset_enabled ? 1 : 0;
  auto sc_mode = r.Get<reg::PA_SU_SC_MODE_CNTL>();
  if (primitive_polygonal) {
    key.cull_mode = (sc_mode.cull_front ? VK_CULL_MODE_FRONT_BIT : 0) |
                    (sc_mode.cull_back ? VK_CULL_MODE_BACK_BIT : 0);
    key.front_face_clockwise = sc_mode.face != 0;
    if (properties.fillModeNonSolid &&
        sc_mode.poly_mode == xenos::PolygonModeEnable::kDualMode) {
      xenos::PolygonType type = xenos::PolygonType::kTriangles;
      if (!sc_mode.cull_front) type = std::min(type, sc_mode.polymode_front_ptype);
      if (!sc_mode.cull_back) type = std::min(type, sc_mode.polymode_back_ptype);
      key.polygon_line = type != xenos::PolygonType::kTriangles;
    }
  }
  key.depth_clamp = properties.depthClamp && pa_cl_clip_cntl.clip_disable;
  if (targets.depth) {
    if (depth_control.z_enable) {
      key.depth_test = 1;
      key.depth_write = depth_control.z_write_enable;
      key.depth_compare = uint32_t(VK_COMPARE_OP_NEVER) + uint32_t(depth_control.zfunc);
    }
    if (depth_control.stencil_enable) {
      key.stencil_test = 1;
      key.stencil_front[0] = uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(depth_control.stencilfail);
      key.stencil_front[1] = uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(depth_control.stencilzpass);
      key.stencil_front[2] = uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(depth_control.stencilzfail);
      key.stencil_front[3] = uint32_t(VK_COMPARE_OP_NEVER) + uint32_t(depth_control.stencilfunc);
      if (primitive_polygonal && depth_control.backface_enable) {
        key.stencil_back[0] =
            uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(depth_control.stencilfail_bf);
        key.stencil_back[1] =
            uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(depth_control.stencilzpass_bf);
        key.stencil_back[2] =
            uint32_t(VK_STENCIL_OP_KEEP) + uint32_t(depth_control.stencilzfail_bf);
        key.stencil_back[3] =
            uint32_t(VK_COMPARE_OP_NEVER) + uint32_t(depth_control.stencilfunc_bf);
      } else {
        std::memcpy(key.stencil_back, key.stencil_front, sizeof(key.stencil_back));
      }
    }
  }
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t mask = (color_mask >> (4 * i)) & 0xF;
    if (!targets.color[i]) mask = 0;
    key.write_masks |= mask << (4 * i);
    uint32_t blend = r[reg::RB_BLENDCONTROL::rt_register_indices[i]] & 0x1FFF1FFF;
    key.blend[i] = mask ? blend : 0x00010001;
  }
  if (specialize_signs) {
    for (const SpirvShader* shader : stage_shaders) {
      if (!shader) continue;
      for (const auto& binding : shader->texture_bindings()) {
        uint32_t fetch = binding.fetch_constant;
        uint8_t signs = rex::graphics::texture_util::SwizzleSigns(r.GetTextureFetch(fetch));
        uint32_t& word = key.texture_signs[fetch >> 2];
        uint32_t shift = 8 * (fetch & 3);
        word = (word & ~(0xFFu << shift)) | (uint32_t(signs) << shift);
      }
    }
  }
  key.layout = (uint64_t(st.counts[0]) << 48) | (uint64_t(st.counts[1]) << 32) |
               (uint64_t(st.counts[2]) << 16) | st.counts[3];
  CompiledShader* geometry_module = nullptr;
  if (geometry != PipelineGeometryShader::kNone) {
    GeometryShaderKey gs_key;
    gs_key.type = geometry;
    gs_key.interpolator_count = rex::bit_count(vs_mod.vertex.interpolator_mask);
    gs_key.user_clip_plane_count = vs_mod.vertex.user_clip_plane_count;
    gs_key.user_clip_plane_cull = vs_mod.vertex.user_clip_plane_cull;
    gs_key.has_vertex_kill_and = vs_mod.vertex.vertex_kill_and;
    gs_key.has_point_size = vs_mod.vertex.output_point_parameters;
    gs_key.has_point_coordinates = ps ? ps_mod.pixel.param_gen_point : 0;
    gs_key.point_ps_ucp_mode = vs_mod.vertex.point_ps_ucp_mode;
    key.geometry_shader = gs_key.key;
    auto it = geometry_shaders.find(gs_key.key);
    if (it == geometry_shaders.end()) {
      it = geometry_shaders.emplace(gs_key.key, CompileGeometry(gs_key)).first;
    }
    geometry_module = it->second;
    if (!geometry_module) return fail(12);
  }
  bool pending = false;
  st.pipeline = GetPipeline(key, vs_translation, ps_translation, geometry_module,
                            st.layout->layout, &pending);
  if (!st.pipeline) {
    // A pipeline being compiled: look again on the next draw.
    if (pending) st.valid = false;
    return fail(13);
  }

  // Dynamic state (VulkanCommandProcessor::UpdateDynamicState): all of it
  // comes from state registers.
  st.primitive_polygonal = primitive_polygonal;
  st.depth_control = depth_control;
  if (st.viewport.xy_extent[0] && st.viewport.xy_extent[1]) {
    st.vp.x = float(st.viewport.xy_offset[0]);
    st.vp.y = float(st.viewport.xy_offset[1]);
    st.vp.width = float(st.viewport.xy_extent[0]);
    st.vp.height = float(st.viewport.xy_extent[1]);
  } else {
    st.vp.x = -1.0f;
    st.vp.y = -1.0f;
    st.vp.width = 1.0f;
    st.vp.height = 1.0f;
  }
  st.vp.minDepth = st.viewport.z_min;
  st.vp.maxDepth = st.viewport.z_max;
  draw_util::GetPreferredFacePolygonOffset(r, primitive_polygonal, st.bias_slope,
                                           st.bias_constant);
  const bool guest_depth_unorm = r.Get<reg::RB_DEPTH_INFO>().depth_format ==
                                  xenos::DepthRenderTargetFormat::kD24S8;
  // D3D9 supplies normalized depth bias. SetGuestDepthBias selects the same
  // format-dependent Maxwell representation as the audited Vulkan path and
  // cancels deko3d's API-level doubling before the register write.
  st.bias_constant *= guest_depth_unorm ? draw_util::kD3D10PolygonOffsetFactorUnorm24
                                      : draw_util::kD3D10PolygonOffsetFactorFloat24;
  st.bias_slope *= xenos::kPolygonOffsetScaleSubpixelUnit;
  st.blend_constants[0] = r.Get<float>(rex::graphics::XE_GPU_REG_RB_BLEND_RED);
  st.blend_constants[1] = r.Get<float>(rex::graphics::XE_GPU_REG_RB_BLEND_GREEN);
  st.blend_constants[2] = r.Get<float>(rex::graphics::XE_GPU_REG_RB_BLEND_BLUE);
  st.blend_constants[3] = r.Get<float>(rex::graphics::XE_GPU_REG_RB_BLEND_ALPHA);
  auto front = r.Get<reg::RB_STENCILREFMASK>();
  auto back = front;
  if (primitive_polygonal && depth_control.backface_enable) {
    back = r.Get<reg::RB_STENCILREFMASK>(rex::graphics::XE_GPU_REG_RB_STENCILREFMASK_BF);
  }
  uint32_t stencil[6] = {front.stencilmask,      back.stencilmask, front.stencilwritemask,
                         back.stencilwritemask, front.stencilref, back.stencilref};
  std::memcpy(st.stencil, stencil, sizeof(stencil));
  return true;
}
