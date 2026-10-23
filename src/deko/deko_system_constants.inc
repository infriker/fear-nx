void Renderer::Impl::UpdateSystemConstants(bool primitive_polygonal,
                                           const draw_util::ViewportInfo& viewport,
                                           uint32_t used_texture_mask, uint32_t,
                                           const SpirvShader* vs, const SpirvShader* ps) {
  // VulkanCommandProcessor::UpdateSystemConstantValues for host render
  // targets, without EDRAM, tessellation or memory export.
  const RegisterFile& r = *regs;
  auto pa_cl_clip_cntl = r.Get<reg::PA_CL_CLIP_CNTL>();
  auto pa_cl_vte_cntl = r.Get<reg::PA_CL_VTE_CNTL>();
  auto rb_colorcontrol = r.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = r.Get<reg::RB_DEPTH_INFO>();
  auto rb_surface_info = r.Get<reg::RB_SURFACE_INFO>();
  auto vgt_draw_initiator = r.Get<reg::VGT_DRAW_INITIATOR>();
  SpirvShaderTranslator::SystemConstants& sc = system_constants;

  uint32_t flags = 0;
  if (pa_cl_vte_cntl.vtx_xy_fmt) flags |= SpirvShaderTranslator::kSysFlag_XYDividedByW;
  if (pa_cl_vte_cntl.vtx_z_fmt) flags |= SpirvShaderTranslator::kSysFlag_ZDividedByW;
  if (pa_cl_vte_cntl.vtx_w0_fmt) flags |= SpirvShaderTranslator::kSysFlag_WNotReciprocal;
  if (primitive_polygonal) flags |= SpirvShaderTranslator::kSysFlag_PrimitivePolygonal;
  if (draw_util::IsPrimitiveLine(r)) flags |= SpirvShaderTranslator::kSysFlag_PrimitiveLine;
  flags |= HostMsaa(rb_surface_info.msaa_samples)
           << SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift;
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= SpirvShaderTranslator::kSysFlag_DepthFloat24;
  }
  xenos::CompareFunction alpha_test = rb_colorcontrol.alpha_test_enable
                                          ? rb_colorcontrol.alpha_func
                                          : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test) << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  for (uint32_t i = 0; i < 4; ++i) {
    auto color_info = r.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    if (color_info.color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
      flags |= SpirvShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
    }
    // Exponent bias, with 16_16 and 16_16_16_16 stored as -1...1 SNORM
    // instead of -32...32.
    int32_t exp_bias = color_info.color_exp_bias;
    if (color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
        color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16) {
      exp_bias -= 5;
    }
    float scale;
    uint32_t scale_bits = UINT32_C(0x3F800000) + (uint32_t(exp_bias) << 23);
    std::memcpy(&scale, &scale_bits, sizeof(scale));
    sc.color_exp_bias[i] = scale;
  }
  sc.flags = flags;
  sc.vertex_index_endian = xenos::Endian::kNone;
  sc.line_loop_closing_index = 0;
  sc.vertex_base_index = r.Get<int32_t>(rex::graphics::XE_GPU_REG_VGT_INDX_OFFSET);
  sc.vertex_index_min = r[rex::graphics::XE_GPU_REG_VGT_MIN_VTX_INDX];
  sc.vertex_index_max = r[rex::graphics::XE_GPU_REG_VGT_MAX_VTX_INDX];
  sc.vertex_index_reset = 0;
  sc.compute_memexport_vertex_count = 0;
  if (!pa_cl_clip_cntl.clip_disable) {
    float* planes = sc.user_clip_planes[0];
    uint32_t remaining = pa_cl_clip_cntl.ucp_ena;
    uint32_t index;
    while (rex::bit_scan_forward(remaining, &index)) {
      remaining &= ~(1u << index);
      std::memcpy(planes, &r[rex::graphics::XE_GPU_REG_PA_CL_UCP_0_X + index * 4],
                  4 * sizeof(float));
      planes += 4;
    }
  }
  for (uint32_t i = 0; i < 3; ++i) {
    sc.ndc_scale[i] = viewport.ndc_scale[i];
    sc.ndc_offset[i] = viewport.ndc_offset[i];
    // deko3d images start at the upper left, but clip-space +Y points up.
    if (i == 1) { sc.ndc_scale[i] = -sc.ndc_scale[i]; sc.ndc_offset[i] = -sc.ndc_offset[i]; }
  }
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto minmax = r.Get<reg::PA_SU_POINT_MINMAX>();
    auto size = r.Get<reg::PA_SU_POINT_SIZE>();
    sc.point_vertex_diameter_min = float(minmax.min_size) * (2.0f / 16.0f);
    sc.point_vertex_diameter_max = float(minmax.max_size) * (2.0f / 16.0f);
    sc.point_constant_diameter[0] = float(size.width) * (2.0f / 16.0f);
    sc.point_constant_diameter[1] = float(size.height) * (2.0f / 16.0f);
    sc.point_screen_diameter_to_ndc_radius[0] =
        1.0f / float(std::max(viewport.xy_extent[0], 1u));
    sc.point_screen_diameter_to_ndc_radius[1] =
        -1.0f / float(std::max(viewport.xy_extent[1], 1u));
  }
  uint32_t remaining = used_texture_mask;
  uint32_t index;
  while (rex::bit_scan_forward(remaining, &index)) {
    remaining &= ~(1u << index);
    uint32_t& word = sc.texture_swizzled_signs[index >> 2];
    uint32_t shift = 8 * (index & 3);
    uint8_t signs = rex::graphics::texture_util::SwizzleSigns(r.GetTextureFetch(index));
    word = (word & ~(0xFFu << shift)) | (uint32_t(signs) << shift);
  }
  sc.textures_resolution_scaled = 0;
  sc.alpha_test_reference = r.Get<float>(rex::graphics::XE_GPU_REG_RB_ALPHA_REF);
  sc.alpha_to_mask = rb_colorcontrol.alpha_to_mask_enable
                         ? (rb_colorcontrol.value >> 24) | (UINT32_C(1) << 8)
                         : 0;
  (void)vs;
  (void)ps;
}
