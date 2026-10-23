// SPDX-License-Identifier: GPL-3.0-or-later
// Included inside fear_nx::deko after Renderer::Impl is complete.

void Renderer::Impl::MaterializeClear(Image& image,const ClearRect* area) {
  if(image.pending_clear.empty())return;
  const bool coalesce=REXCVAR_GET(nx_native_clear_coalesce);
  const auto batch=area && REXCVAR_GET(nx_native_clear_regions)?
      image.pending_clear.Consume(*area,coalesce):image.pending_clear.TakeAll(coalesce);
  profile.clear_regions_merged+=batch.merged;profile.clear_expanded_pixels+=batch.early_pixels;
  if(batch.expanded)++profile.clear_expanded_consumes;
  if(batch.drained)++profile.clears_capacity_drains;
  if(batch.count && !image.pending_clear.empty())++profile.clear_partial_consumes;
  for(uint32_t i=0;i<batch.count;++i) {
    const auto& clear=batch.items[i];
    const VkRect2D rect{{int32_t(clear.rect.x),int32_t(clear.rect.y)},{clear.rect.w,clear.rect.h}};
    ExecuteClear(image,clear.depth,clear.z,clear.stencil,clear.mask,clear.color.data(),rect,clear.clear_z);
  }
  profile.clears_materialized+=batch.count;
}

void Renderer::Impl::Clear(Image& image,bool depth,float z,uint8_t stencil,uint32_t mask,
                           const float* color,const VkRect2D& rect,bool clear_z) {
  if(frontbuffer.destination && &frontbuffer.destination->image==&image)MaterializeFrontbuffer();
  const auto area=ClipClearRect(image.width,image.height,rect.offset.x,rect.offset.y,rect.extent.width,rect.extent.height);
  if(area.empty() || (depth && !clear_z && !(mask&0xFF)))return;
  const bool full=CanDeferClear(image.width,image.height,rect.offset.x,rect.offset.y,
      rect.extent.width,rect.extent.height,depth,clear_z,mask);
  if(REXCVAR_GET(nx_native_deferred_clears) && image.layers==1 && image.mip_levels==1 &&
      (REXCVAR_GET(nx_native_clear_regions) || full)) {
    ImageClear clear{area,depth,clear_z,uint8_t(mask),stencil,z,{}};
    if(!depth)std::copy_n(color,4,clear.color.begin());
    uint32_t replaced=0,merged=0;
    const bool coalesce=REXCVAR_GET(nx_native_clear_coalesce);
    auto result=image.pending_clear.Queue(clear,replaced,coalesce,&merged);
    if(result==ImageClearQueue::Result::Full) {
      MaterializeClear(image);++profile.clears_capacity_drains;
      result=image.pending_clear.Queue(clear,replaced,coalesce,&merged);
    }
    if(result==ImageClearQueue::Result::Repeated) {++profile.clears_repeated;return;}
    profile.clear_regions_merged+=merged;
    profile.clears_elided+=replaced;
    if(!CoversImage(image.width,image.height,area.x,area.y,area.w,area.h))++profile.clears_rectangles;
    if(depth && (!clear_z || uint8_t(mask)!=0xFF))++profile.clears_masked;
    profile.clear_pixels_queued+=area.pixels();
    // A queued clear can target an already-bound pass. Invalidate immediately,
    // and consume it before comparing cached target/scissor state on the draw.
    command_state.Invalidate();textures->Changed(image);
    ++profile.clears_deferred;
    return;
  }
  MaterializeClear(image);
  image.pending_clear.Written(area);
  ExecuteClear(image,depth,z,stencil,mask,color,rect,clear_z);
}

void Renderer::Impl::ExecuteClear(Image& image,bool depth,float z,uint8_t stencil,uint32_t mask,
                                  const float* color,const VkRect2D& rect,bool clear_z) {
  BeforeImageWrite(image,!depth && CoversImage(image.width,image.height,rect.offset.x,rect.offset.y,rect.extent.width,rect.extent.height));
  Begin();command_state.Invalidate();
  const std::array<ImageAccess,1> accesses{{{image.identity,ImageAccess::Write}}};
  BeforeImages(accesses);auto v=View(image);
  if(depth)dkCmdBufBindRenderTargets(core.comandos(),nullptr,0,&v);
  else{const DkImageView* views[]={&v};dkCmdBufBindRenderTargets(core.comandos(),views,1,nullptr);}
  DkScissor sc{uint32_t(rect.offset.x),uint32_t(rect.offset.y),rect.extent.width,rect.extent.height};
  dkCmdBufSetScissors(core.comandos(),0,&sc,1);
  if(depth)dkCmdBufClearDepthStencil(core.comandos(),clear_z,z,mask,stencil);
  else dkCmdBufClearColorFloat(core.comandos(),0,DkColorMask_RGBA,color[0],color[1],color[2],color[3]);
  textures->Touch(image,work);textures->Changed(image);image_barriers.Images(accesses);
  if(!REXCVAR_GET(nx_native_image_barriers))Barrier();
  ++profile.clears_executed;profile.clear_pixels_executed+=uint64_t(rect.extent.width)*rect.extent.height;
}

bool Renderer::Impl::CopyColor3D(Image& from,Image& to,uint32_t sx,uint32_t sy,
                                uint32_t dx,uint32_t dy,uint32_t width,uint32_t height) {
  if(!REXCVAR_GET(nx_native_copy3d) || &from==&to || from.format!=to.format ||
     !Copy3DColorFormat(from.format) || !(to.flags&DkImageFlags_UsageRender) ||
     from.layers!=1 || to.layers!=1 || from.mip_levels!=1 || to.mip_levels!=1 ||
     (from.type!=DkImageType_2D && from.type!=DkImageType_2DArray) ||
     (to.type!=DkImageType_2D && to.type!=DkImageType_2DArray) ||
     !FitsImage(from.width,from.height,sx,sy,width,height) ||
     !FitsImage(to.width,to.height,dx,dy,width,height))return false;
  auto* vs=CompileText(kImageCopyVertex,ShaderStage::kVertex);
  auto* fs=CompileText(kImageCopyFragment,ShaderStage::kFragment);
  if(!vs||!fs)return false;
  const auto slot=textures->GetImageView(from,xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
      DkImageType_2D,DkDsSource_Depth,false,DkImageFormat_None,false);
  if(!slot)return false;
  // This helper may run during draw texture preparation. Never flush here:
  // that would invalidate the caller's already prepared constants/geometry.
  auto data=core.Reservar(256,DK_UNIFORM_BUF_ALIGNMENT);
  if(!data)return false;
  const std::array<int32_t,64> params{int32_t(sx)-int32_t(dx),int32_t(sy)-int32_t(dy)};
  memcpy(data.cpu,params.data(),sizeof(params));
  core.InvalidarCodigoSiHaceFalta();
  // Same-engine ROP writes need fragment ordering, transfers need a full drain.
  // The fresh CPU uniform still needs shader-data invalidation independently.
  const std::array<ImageAccess,2> accesses{{{from.identity,ImageAccess::Fragment},{to.identity,ImageAccess::Write}}};
  BeforeImages(accesses);draw_barriers.CpuUpload();DrawBarrier();
  command_state.Invalidate();
  auto cmd=core.comandos();auto target=View(to);
  const DkImageView* targets[]{&target};
  dkCmdBufBindRenderTargets(cmd,targets,1,nullptr);
  DkViewport vp{float(dx),float(dy),float(width),float(height),0,1};
  DkScissor sc{dx,dy,width,height};
  dkCmdBufSetViewports(cmd,0,&vp,1);dkCmdBufSetScissors(cmd,0,&sc,1);
  DkRasterizerState raster{};dkRasterizerStateDefaults(&raster);raster.cullMode=DkFace_None;
  DkDepthStencilState ds{};dkDepthStencilStateDefaults(&ds);
  ds.depthTestEnable=false;ds.depthWriteEnable=false;ds.stencilTestEnable=false;
  DkColorState color{};dkColorStateDefaults(&color);
  DkColorWriteState write{};dkColorWriteStateDefaults(&write);
  DkMultisampleState ms{};dkMultisampleStateDefaults(&ms);
  dkCmdBufBindRasterizerState(cmd,&raster);dkCmdBufBindDepthStencilState(cmd,&ds);
  dkCmdBufBindColorState(cmd,&color);dkCmdBufBindColorWriteState(cmd,&write);
  dkCmdBufBindMultisampleState(cmd,&ms);dkCmdBufSetSampleMask(cmd,0xFFFFu);
  dkCmdBufSetPrimitiveRestart(cmd,false,UINT32_MAX);
  dkCmdBufBindVtxAttribState(cmd,nullptr,0);dkCmdBufBindVtxBufferState(cmd,nullptr,0);
  const DkShader* shaders[]{&vs->shader,&fs->shader};
  dkCmdBufBindShaders(cmd,DkStageFlag_GraphicsMask,shaders,2);
  dkCmdBufBindUniformBuffer(cmd,DkStage_Fragment,0,data.gpu,256);
  const DkResHandle texture=dkMakeTextureHandle(*slot,0);
  dkCmdBufBindTextures(cmd,DkStage_Fragment,0,&texture,1);
  dkCmdBufDraw(cmd,DkPrimitive_Triangles,3,1,0,0);
  image_barriers.Images(accesses);
  if(!REXCVAR_GET(nx_native_image_barriers))Barrier();
  ++profile.copy3d_calls;profile.copy3d_pixels+=uint64_t(width)*height;
  if(to.flags&DkImageFlags_HwCompression)++profile.copy3d_compressed;
  return true;
}

void Renderer::Impl::Copy(Image& from,Image& to,uint32_t sx,uint32_t sy,uint32_t dx,uint32_t dy,
                          uint32_t width,uint32_t height) {
  Begin();if(!width||!height)return;
  const ClearRect source_area{sx,sy,width,height},target_area{dx,dy,width,height};
  MaterializeClear(from,&source_area);
  // Only a complete, format-identical color overwrite can discard the old
  // destination. Partial copies retain initialized pixels outside the rectangle.
  const bool full=&from!=&to && from.format==to.format && Copy3DColorFormat(to.format) &&
      to.layers==1 && to.mip_levels==1 && CoversImage(to.width,to.height,dx,dy,width,height) &&
      FitsImage(from.width,from.height,sx,sy,width,height);
  profile.clears_elided+=to.pending_clear.DiscardForFullColorOverwrite(full);
  MaterializeClear(to,&target_area);
  BeforeImageWrite(to);
  if(!CopyColor3D(from,to,sx,sy,dx,dy,width,height)) {
    command_state.Invalidate();
    ImageBarrier(image_barriers.BeforeTransfer(REXCVAR_GET(nx_native_image_barriers)));auto sv=View(from),dv=View(to);
    DkImageRect sr{sx,sy,0,width,height,1},dr{dx,dy,0,width,height,1};
    dkCmdBufCopyImage(core.comandos(),&sv,&sr,&dv,&dr,0);image_barriers.Transfer();
    if(!REXCVAR_GET(nx_native_image_barriers))Barrier();
    ++profile.copy_transfer_calls;
  }
  textures->Touch(from,work);textures->Touch(to,work);textures->Changed(to);
  to.pending_clear.Written(target_area);
}
