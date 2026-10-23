// SPDX-License-Identifier: GPL-3.0-or-later
// One frontbuffer snapshot; texture reads and source mutations preserve its pixels.
void Renderer::Impl::MaterializeFrontbuffer() {
  if(!frontbuffer.source)return;
  const auto saved=frontbuffer;frontbuffer={};
  ReadBeforeQueuedClears(saved.source->image,[&] {
    Copy(saved.source->image,saved.destination->image,0,0,0,0,saved.width,saved.height);
  });
  ++profile.front_copies;profile.front_copy_pixels+=uint64_t(saved.width)*saved.height;
}
void Renderer::Impl::BeforeImageWrite(Image& image,bool discard_whole_color) {
  if(!frontbuffer.source)return;
  if(&frontbuffer.destination->image==&image){MaterializeFrontbuffer();return;}
  if(&frontbuffer.source->image!=&image)return;
  auto& target=frontbuffer.source->image;
  auto& resolved=frontbuffer.destination->image;
  // Exact storage exchange only when the impending clear discards every pixel.
  // Partial draws/clears retain the source through an ordinary ordered copy.
  if(discard_whole_color && target.format==resolved.format &&
      target.width==resolved.width && target.height==resolved.height &&
      target.layers==1 && resolved.layers==1 && target.mip_levels==1 && resolved.mip_levels==1 &&
      (resolved.flags&DkImageFlags_UsageRender)) {
    auto pending=std::move(target.pending_clear);
    const auto target_swizzle=target.host_format_swizzle;
    const auto resolved_swizzle=resolved.host_format_swizzle;
    std::swap(target,resolved);
    target.pending_clear=std::move(pending);resolved.pending_clear={};
    target.host_format_swizzle=target_swizzle;resolved.host_format_swizzle=resolved_swizzle;
    frontbuffer={};++rt_generation;++resolve_version_clock;
    command_state.Invalidate();draw_textures.Invalidate();
    Barrier(); // old resolve storage may still be sampled by earlier queued work.
    ++profile.front_exchanges;
  } else MaterializeFrontbuffer();
}
bool Renderer::Impl::DeferFrontbuffer(RenderTarget& source,ResolveImage& destination,
    uint32_t root,uint32_t sx,uint32_t sy,uint32_t dx,uint32_t dy,
    uint32_t width,uint32_t height,bool alias,bool swapped) {
  const FrontbufferResolve plan{root,last_frontbuffer,source.image.width,source.image.height,
      destination.image.width,destination.image.height,sx,sy,dx,dy,width,height,
      !destination.is_depth && source.image.format==destination.image.format,
      swapped,alias};
  if(!REXCVAR_GET(nx_native_deferred_frontbuffer)||!CanDeferFrontbuffer(plan))return false;
  if(frontbuffer.source) {
    if(frontbuffer.destination==&destination) {frontbuffer={};++profile.front_replaced;}
    else MaterializeFrontbuffer();
  }
  // All clears predating the resolve are visible in the snapshot.
  MaterializeClear(source.image);
  MaterializeClear(destination.image);
  frontbuffer={&source,&destination,width,height};
  textures->Touch(source.image,work);
  command_state.Invalidate();draw_textures.Invalidate();
  ++profile.front_deferred;profile.front_deferred_pixels+=uint64_t(width)*height;
  return true;
}
