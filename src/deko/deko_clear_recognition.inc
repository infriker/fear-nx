bool Renderer::Impl::GetFastClearRectangle(StencilClearRect& rect, bool require_zero_depth) {
  const DrawSetup& st = setup;
  const RegisterFile& r = *regs;
  // Derive the actual three corners, never assume the whole scissor is cleared.
  // This uses the same interpreter and guest viewport mapping as the SDK's
  // RectangleCoversPixels / draw extent estimator.
  class PositionSink final : public rex::graphics::ShaderInterpreter::ExportSink {
   public:
    void Export(rex::graphics::ucode::ExportRegister output, const float* value,
                uint32_t mask) override {
      if (output != rex::graphics::ucode::ExportRegister::kVSPosition) return;
      for (uint32_t i = 0; i < 4; ++i) {
        if (mask & (1u << i)) position[i] = value[i];
      }
      written |= mask;
    }
    float position[4] = {};
    uint32_t written = 0;
  } sink;
  rex::graphics::ShaderInterpreter interpreter(r, *memory);
  interpreter.SetShader(*st.vs);
  interpreter.SetExportSink(&sink);
  const auto vte = r.Get<reg::PA_CL_VTE_CNTL>();
  if (require_zero_depth && vte.vport_z_offset_ena &&
      r.Get<float>(rex::graphics::XE_GPU_REG_PA_CL_VPORT_ZOFFSET) != 0.0f) return false;
  if (require_zero_depth && vte.vport_z_scale_ena) {
    const float scale_z = r.Get<float>(rex::graphics::XE_GPU_REG_PA_CL_VPORT_ZSCALE);
    if (!std::isfinite(scale_z) || scale_z < 0.0f) return false;
  }
  const float scale_x = vte.vport_x_scale_ena
                            ? r.Get<float>(rex::graphics::XE_GPU_REG_PA_CL_VPORT_XSCALE) : 1.0f;
  const float scale_y = vte.vport_y_scale_ena
                            ? r.Get<float>(rex::graphics::XE_GPU_REG_PA_CL_VPORT_YSCALE) : 1.0f;
  float offset_x = vte.vport_x_offset_ena
                            ? r.Get<float>(rex::graphics::XE_GPU_REG_PA_CL_VPORT_XOFFSET) : 0.0f;
  float offset_y = vte.vport_y_offset_ena
                            ? r.Get<float>(rex::graphics::XE_GPU_REG_PA_CL_VPORT_YOFFSET) : 0.0f;
  if (r.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero) {
    offset_x += 0.5f;
    offset_y += 0.5f;
  }
  if (r.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
    const auto offset = r.Get<reg::PA_SC_WINDOW_OFFSET>();
    offset_x += float(offset.window_x_offset);
    offset_y += float(offset.window_y_offset);
  }
  const uint32_t index_offset = r.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  const uint32_t index_min = r.Get<reg::VGT_MIN_VTX_INDX>().min_indx;
  const uint32_t index_max = r.Get<reg::VGT_MAX_VTX_INDX>().max_indx;
  float xs[3], ys[3];
  float first_w = 0.0f;
  for (uint32_t i = 0; i < 3; ++i) {
    sink = PositionSink();
    std::fill_n(interpreter.temp_registers(), xenos::kMaxShaderTempRegisters * 4, 0.0f);
    interpreter.temp_registers()[0] =
        float(std::min(index_max, std::max(index_min, (i + index_offset) & 0xFFFFFF)));
    interpreter.Execute();
    if (sink.written != 0b1111 || !std::isfinite(sink.position[2]) ||
        !std::isfinite(sink.position[3]) ||
        !(sink.position[3] > 0.0f)) return false;
    // With depth clamp enabled, positive W and nonnegative Z scale, any
    // nonpositive Z maps exactly to zero, including D24 rounding bias.
    if (require_zero_depth && sink.position[2] > 0.0f) return false;
    if (!i) first_w = sink.position[3];
    else if (sink.position[3] != first_w) return false;
    float x = sink.position[0], y = sink.position[1];
    if (!vte.vtx_xy_fmt) {
      const float w = vte.vtx_w0_fmt ? sink.position[3] : 1.0f / sink.position[3];
      x /= w;
      y /= w;
    }
    xs[i] = x * scale_x + offset_x;
    ys[i] = y * scale_y + offset_y;
    // Partial pixel/sample coverage requires a shader transfer, not a clear.
    if (!std::isfinite(xs[i]) || !std::isfinite(ys[i]) ||
        std::abs(xs[i]) > 16384.0f || std::abs(ys[i]) > 16384.0f ||
        xs[i] != std::round(xs[i]) || ys[i] != std::round(ys[i])) return false;
  }
  const float x0 = std::min({xs[0], xs[1], xs[2]});
  const float x1 = std::max({xs[0], xs[1], xs[2]});
  const float y0 = std::min({ys[0], ys[1], ys[2]});
  const float y1 = std::max({ys[0], ys[1], ys[2]});
  if (!(x0 < x1 && y0 < y1)) return false;
  // Avoid interpreting clip-plane intersections or partial viewport coverage.
  if (x0 < st.vp.x || y0 < st.vp.y || x1 > st.vp.x + st.vp.width ||
      y1 > st.vp.y + st.vp.height) return false;
  uint32_t corners = 0;
  for (uint32_t i = 0; i < 3; ++i) {
    if ((xs[i] != x0 && xs[i] != x1) || (ys[i] != y0 && ys[i] != y1)) return false;
    corners |= 1u << (uint32_t(xs[i] == x1) | (uint32_t(ys[i] == y1) << 1));
  }
  if (rex::bit_count(corners) != 3) return false;
  rect = {
      std::max(int32_t(x0), bound_scissor.offset.x),
      std::max(int32_t(y0), bound_scissor.offset.y),
      std::min(int32_t(x1), int32_t(bound_scissor.offset.x + bound_scissor.extent.width)),
      std::min(int32_t(y1), int32_t(bound_scissor.offset.y + bound_scissor.extent.height))};

  return rect.x0 < rect.x1 && rect.y0 < rect.y1;
}
