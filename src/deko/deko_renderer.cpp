// SPDX-License-Identifier: GPL-3.0-or-later
// F.E.A.R. deko3d backend. Xenos register/constant semantics adapted from the
// preserved native Vulkan renderer (Xenia/ReXGlue, BSD-3-Clause portions).
#include "deko_renderer.h"
#include "deko_shaders.h"
#include "deko_textures.h"
#include "deko_geometry_shaders.h"
#include "deko_shader_cache.h"
#include "deko_shader_warmup.h"
#include "deko_profile.h"
#include "deko_gpu_profile.h"
#include "deko_queue_work.h"
#if defined(__SWITCH__)
#include <switch.h>
#endif
#include "deko_validation_cache.h"
#include "deko_vertex_cache.h"
#include "deko_vertex_range_shader.h"
#include "deko_draw_upload_cache.h"
#include "deko_small_index_upload.h"
#include "deko_index_write.h"
#include "deko_index_batch.h"
#include "deko_vertex_write.h"
#include "deko_upload_commands.h"
#include "deko_vertex_epoch_cache.h"
#include "deko_vertex_write_probe.h"
#include "deko_command_cache.h"
#include "deko_shared_geometry.h"
#include "deko_shared_constants.h"
#include "deko_constant_slots.h"
#include "deko_draw_texture_cache.h"
#include "deko_draw_dependencies.h"
#include "deko_buffer_descriptor.h"
#include "../native/native_stencil_clear.h"
#include "../native/native_depth_color_clear.h"
#include "../native/native_resolve_layout.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <unordered_map>
#include <xxhash.h>
#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory/utils.h>
#include <rex/string/buffer.h>
#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/interpreter.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/registers.h>
#include <rex/system/xmemory.h>
#include <rex/system/xvideo.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/ui/deko3d/presentador.h>
#include "deko_frontbuffer.h"
#include "../cpu/fear_cpu.h"
#include <rex/ui/deko3d/cache_dksh.h>

REXCVAR_DECLARE(bool, half_pixel_offset);

// Preserve existing TOML compatibility. GPU scheduling is deliberately ordered
// for the initial correctness candidate: an uncached shader waits, never drops a draw.
REXCVAR_DEFINE_BOOL(nx_native_depth24, true, "F.E.A.R. NX", "Use compact D24S8 for fixed depth; preserve float depth and stencil");
REXCVAR_DEFINE_BOOL(nx_native_gpu_profile, false, "F.E.A.R. NX", "Collect deko3d timing diagnostics");
REXCVAR_DEFINE_BOOL(nx_native_record_thread, false, "F.E.A.R. NX", "Reserved Vulkan recording-thread option");
REXCVAR_DEFINE_BOOL(nx_native_fused_gamma, true, "F.E.A.R. NX", "Apply guest gamma in final presentation");
REXCVAR_DEFINE_BOOL(nx_native_deferred_frontbuffer, true, "F.E.A.R. NX", "Present retained color target; copy only before conflicting use");
REXCVAR_DEFINE_BOOL(nx_native_zcull, true, "F.E.A.R. NX", "Enable depth hierarchy");
REXCVAR_DEFINE_BOOL(nx_native_shared_resolves, true, "F.E.A.R. NX", "Preserve shared resolve destinations");
REXCVAR_DEFINE_BOOL(nx_native_precise_resolve_barriers, true, "F.E.A.R. NX", "Preserve ordered resolve dependencies");
REXCVAR_DEFINE_BOOL(nx_native_full_clear_load_op, true, "F.E.A.R. NX", "Full guest clears discard prior attachment contents");
REXCVAR_DEFINE_BOOL(nx_native_msaa, false, "F.E.A.R. NX", "Guest sample-layout tracking; test backend uses single host samples");
REXCVAR_DEFINE_BOOL(nx_native_async_pipelines, false, "F.E.A.R. NX", "Compatibility option; deko correctness build waits for compilation");
REXCVAR_DEFINE_BOOL(nx_native_trace_output, false, "F.E.A.R. NX", "Log deko rendering diagnostics");
REXCVAR_DEFINE_BOOL(nx_native_shader_warmup, true, "F.E.A.R. NX", "Prepare previously observed deko shader variants before starting the game");
REXCVAR_DEFINE_BOOL(nx_native_cpu_profile, true, "F.E.A.R. NX", "Measure deko renderer phases on the ring thread");
REXCVAR_DEFINE_BOOL(nx_native_validation_cache, true, "F.E.A.R. NX", "Dante-style sampled guest checks; full guards stop after 20000 matches (false: full per-use hashing)");
REXCVAR_DEFINE_BOOL(nx_native_state_cache, true, "F.E.A.R. NX", "Skip identical emitted graphics state between draws");
REXCVAR_DEFINE_BOOL(nx_native_draw_barriers, true, "F.E.A.R. NX", "Avoid full drains between draws; preserve helper transfer barriers and upload cache invalidations");
REXCVAR_DEFINE_BOOL(nx_native_index_cache, true, "F.E.A.R. NX", "Retain exactly validated index uploads in fence-safe GPU slots");
REXCVAR_DEFINE_BOOL(nx_native_delta_constants, true, "F.E.A.R. NX", "Update changed constant ranges with ordered GPU commands");
REXCVAR_DEFINE_BOOL(nx_native_geometry_cache, true, "F.E.A.R. NX", "Reuse immutable geometry across GPU work slots and frames");
REXCVAR_DEFINE_BOOL(nx_native_shared_geometry, true, "F.E.A.R. NX", "Share immutable geometry across all upload slots (startup option)");
REXCVAR_DEFINE_BOOL(nx_native_geometry_fast_lookup, true, "F.E.A.R. NX", "Use exact-key front tables for resident vertices and indices (startup option)");
REXCVAR_DEFINE_BOOL(nx_native_shared_constants, true, "F.E.A.R. NX", "Share exactly compatible constant layouts (startup option)");
REXCVAR_DEFINE_BOOL(nx_native_draw_texture_cache, true, "F.E.A.R. NX", "Reuse the complete validated texture set of the previous draw");

REXCVAR_DEFINE_BOOL(nx_native_small_vertex_batch, true, "F.E.A.R. NX", "Batch transient vertex uploads up to 64 KiB before submission");
REXCVAR_DEFINE_BOOL(nx_native_index_validation_reuse, true, "F.E.A.R. NX", "Reuse index hashes after an exact cached-source comparison");
REXCVAR_DEFINE_BOOL(nx_native_index16_compact, true, "F.E.A.R. NX", "Pack converted index values that fit Uint16; preserve restart fallback");
REXCVAR_DEFINE_BOOL(nx_native_vertex_grid_windows, true, "F.E.A.R. NX", "Bound proven vertex reads on a stable 64 KiB grid");
REXCVAR_DEFINE_BOOL(nx_native_vertex_dynamic_windows, true, "F.E.A.R. NX", "Use exact aligned fetch windows for known-changing geometry; preserve stable cache windows");
REXCVAR_DEFINE_BOOL(nx_native_vertex_epoch_reuse, true, "F.E.A.R. NX", "NFSMW-style work-local vertex upload reuse until guest synchronization (false: content checks per use)");
REXCVAR_DEFINE_BOOL(nx_native_vertex_write_probe, true, "F.E.A.R. NX", "Sample guest-to-RAM, guest-to-upload and RAM-to-upload copies at most once per 60 frames and one second");
REXCVAR_DEFINE_BOOL(nx_native_small_index_batch, true, "F.E.A.R. NX", "Batch small transient index payloads before GPU submission");
REXCVAR_DEFINE_BOOL(nx_native_index_write_fast, true, "F.E.A.R. NX", "Sequential vector index writes into fresh owned GPU memory");
REXCVAR_DEFINE_BOOL(nx_native_index_resident_batch, true, "F.E.A.R. NX", "Coalesce adjacent new resident-index blocks before submission");
REXCVAR_DEFINE_BOOL(nx_native_index_ordered_upload, true, "F.E.A.R. NX", "Prefer consecutive index allocations and gather fresh writes in address order before submission");
REXCVAR_DEFINE_BOOL(nx_native_vertex_write_fast, true, "F.E.A.R. NX", "Sequential vector writes of unchanged guest vertex bytes into fresh GPU memory");
REXCVAR_DEFINE_BOOL(nx_native_vertex_resident_batch, true, "F.E.A.R. NX", "Coalesce adjacent small new resident-vertex blocks before submission");
REXCVAR_DEFINE_BOOL(nx_native_vertex_sequential_upload, true, "F.E.A.R. NX", "Prefer consecutive fresh resident-vertex allocations; retain best-fit fallback (startup option)");
REXCVAR_DEFINE_BOOL(nx_native_geometry_push_upload, false, "F.E.A.R. NX", "A/B: upload fresh geometry through GPU pre-list commands; restart required");
REXCVAR_DEFINE_BOOL(nx_native_index16, true, "F.E.A.R. NX", "Keep eligible guest index streams in 16-bit form");
REXCVAR_DEFINE_BOOL(nx_native_deferred_clears, true, "F.E.A.R. NX", "Defer full image clears until their first consumer");
REXCVAR_DEFINE_BOOL(nx_native_copy3d, true, "F.E.A.R. NX", "Copy compatible UNORM color images through 3D");
// Run 045: skipping volatile hashes lost immutable-copy reuse and increased
// dynamic uploads. Keep run 044's per-use exact checks for those ranges.
REXCVAR_DEFINE_BOOL(nx_native_geometry_epoch_checks, false, "F.E.A.R. NX", "Experimental geometry epoch checks with uncached volatile uploads; false preserves per-use exact checks and immutable-copy reuse");
REXCVAR_DEFINE_BOOL(nx_native_image_barriers, true, "F.E.A.R. NX", "Track 3D image dependencies; retain full drains for transfers, compute and queue boundaries");
REXCVAR_DEFINE_BOOL(nx_native_clear_regions, true, "F.E.A.R. NX", "Defer exact clear rectangles and aspect masks; initialize exposed regions on demand");
REXCVAR_DEFINE_BOOL(nx_native_constant_slots, true, "F.E.A.R. NX", "Retain constant buffers in each fenced work slot and push only changed ranges");
REXCVAR_DEFINE_BOOL(nx_native_isolated_constants, true, "F.E.A.R. NX", "Keep mutable constants outside the geometry storage-buffer range (startup option)");
REXCVAR_DEFINE_BOOL(nx_native_constant_dirty_ranges, true, "F.E.A.R. NX", "Scan only the proven changed envelope of the next packed constant revision");
REXCVAR_DEFINE_BOOL(nx_native_partial_kicks, true, "F.E.A.R. NX", "Submit idle work without retiring the upload slot, bounded to three partial sends");
REXCVAR_DEFINE_BOOL(nx_native_clear_coalesce, true, "F.E.A.R. NX", "Merge compatible clear regions and avoid splitting mostly consumed rectangles");
REXCVAR_DEFINE_BOOL(nx_native_color_compression, true, "F.E.A.R. NX", "Compress compatible color targets (startup option)");

namespace fear_nx::deko {
namespace xenos = rex::graphics::xenos;
namespace reg = rex::graphics::reg;
namespace draw_util = rex::graphics::draw_util;
namespace resolve_layout = fear_nx::native::resolve_layout;
using rex::graphics::RegisterFile;
using rex::graphics::Shader;
using rex::graphics::SpirvShader;
using rex::graphics::SpirvShaderTranslator;
using namespace fear_nx::native;
using Core = rex::deko3d::Nucleo;
constexpr uint32_t kUploadBytes = 64u << 20;
constexpr uint32_t kWorkSlots = 4;
constexpr uint32_t kIdentitySwizzle = 0x688;
constexpr uint32_t kDiscardedVisibilityProbeSkipReason = 14;

// Like NFSMW's thread-time diagnostics: distinguish scheduled CPU time from
// wall time. Eden reports its emulated scheduler ticks, not host GPU time.
static uint64_t RingThreadCpuNs() {
#if defined(__SWITCH__)
  u64 ticks=0;
  if(R_SUCCEEDED(svcGetInfo(&ticks,InfoType_ThreadTickCount,threadGetCurHandle(),UINT64_MAX))) {
    const auto hz=armGetSystemTickFreq();
    if(hz)return ticks/hz*1000000000ull+(ticks%hz)*1000000000ull/hz;
  }
#endif
  return 0;
}

// Vulkan enums here describe the preserved pipeline key only. Every device,
// allocation, command and presentation operation in this file uses deko3d.
struct RenderTargetKey {
  uint32_t base_tiles=0,pitch=0,format=0,msaa=0,guest_msaa=0,is_depth=0;
  bool operator==(const RenderTargetKey& o) const { return !memcmp(this,&o,sizeof(o)); }
};
struct TargetHash { size_t operator()(const RenderTargetKey& k) const {return XXH3_64bits(&k,sizeof(k));} };
struct RenderTarget {
  RenderTargetKey key{};
  Image image;
  VkFormat format=VK_FORMAT_UNDEFINED;
  uint32_t width=0,height=0,guest_format=0,last_draw_guest_msaa=UINT32_MAX;
  uint64_t last_frame=0;
};
struct PassTargets {
  RenderTarget* color[4]{}; RenderTarget* depth=nullptr;
  bool Empty() const {return !color[0]&&!color[1]&&!color[2]&&!color[3]&&!depth;}
};
struct PipelineKey {
  uint64_t vs_hash,vs_modification,ps_hash,ps_modification;
  uint32_t color_formats[4],depth_format,samples,topology,geometry_shader,primitive_restart;
  uint32_t cull_mode,front_face_clockwise,polygon_line,depth_clamp,depth_test,depth_write,depth_compare,stencil_test;
  uint32_t stencil_front[4],stencil_back[4],blend[4],write_masks,texture_signs[8]; uint64_t layout;
};
struct CompiledShader { DkShader shader{}; GlslShader info; };
struct Pipeline { PipelineKey key{}; CompiledShader *vs=nullptr,*ps=nullptr,*gs=nullptr; };
struct PipelineLayout { uint64_t layout=0; };
struct Capabilities {
  bool geometryShader=true,shaderClipDistance=true,shaderCullDistance=true,fillModeNonSolid=true,depthClamp=true;
  uint32_t maxViewportDimensions[2]={8192,8192};
};
struct DrawSetup {
  bool valid=false; uint64_t state_generation=0,fetch_generation=0,rt_generation=0;
  SpirvShader *active_vs=nullptr,*active_ps=nullptr,*vs=nullptr,*ps=nullptr;
  uint32_t initiator_state=0,skip_reason=UINT32_MAX;
  bool primitive_polygonal=false; reg::RB_DEPTHCONTROL depth_control{};
  bool convert_fan=false,convert_loop=false,strip=false,reset_enabled=false;
  draw_util::Scissor scissor{}; draw_util::ViewportInfo viewport{}; PassTargets targets;
  PipelineLayout* layout=nullptr; uint32_t counts[4]{}; Pipeline* pipeline=nullptr;
  VkViewport vp{}; float bias_constant=0,bias_slope=0;
  float blend_constants[4]{}; uint32_t stencil[6]{};
};

static VkFormat ColorFormatToVk(xenos::ColorRenderTargetFormat f) {
  using F=xenos::ColorRenderTargetFormat;
  switch(f) {
    case F::k_8_8_8_8: case F::k_8_8_8_8_GAMMA:return VK_FORMAT_R8G8B8A8_UNORM;
    case F::k_2_10_10_10:case F::k_2_10_10_10_AS_10_10_10_10:return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case F::k_2_10_10_10_FLOAT:case F::k_2_10_10_10_FLOAT_AS_16_16_16_16:case F::k_16_16_16_16_FLOAT:return VK_FORMAT_R16G16B16A16_SFLOAT;
    case F::k_16_16:return VK_FORMAT_R16G16_SNORM;
    case F::k_16_16_16_16:return VK_FORMAT_R16G16B16A16_SNORM;
    case F::k_16_16_FLOAT:return VK_FORMAT_R16G16_SFLOAT;
    case F::k_32_FLOAT:return VK_FORMAT_R32_SFLOAT;
    case F::k_32_32_FLOAT:return VK_FORMAT_R32G32_SFLOAT;
    default:return VK_FORMAT_UNDEFINED;
  }
}
static DkImageFormat NativeFormat(VkFormat f) {
  switch(f) {
    case VK_FORMAT_R8G8B8A8_UNORM:return DkImageFormat_RGBA8_Unorm;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:return DkImageFormat_RGB10A2_Unorm;
    case VK_FORMAT_R16G16B16A16_SFLOAT:return DkImageFormat_RGBA16_Float;
    case VK_FORMAT_R16G16_SNORM:return DkImageFormat_RG16_Snorm;
    case VK_FORMAT_R16G16B16A16_SNORM:return DkImageFormat_RGBA16_Snorm;
    case VK_FORMAT_R16G16_SFLOAT:return DkImageFormat_RG16_Float;
    case VK_FORMAT_R32_SFLOAT:return DkImageFormat_R32_Float;
    case VK_FORMAT_R32G32_SFLOAT:return DkImageFormat_RG32_Float;
    case VK_FORMAT_D24_UNORM_S8_UINT:return DkImageFormat_Z24S8;
    case VK_FORMAT_D32_SFLOAT_S8_UINT:return DkImageFormat_ZF32_X24S8;
    default:return DkImageFormat_None;
  }
}
static bool IsColorFormat64bpp(xenos::ColorRenderTargetFormat f) {
  using F=xenos::ColorRenderTargetFormat;
  return f==F::k_16_16_16_16||f==F::k_16_16_16_16_FLOAT||f==F::k_32_32_FLOAT;
}
static DkImageView View(Image& i) {DkImageView v;dkImageViewDefaults(&v,&i.image);return v;}
static DkBlendFactor BlendFactor(uint32_t f) {
  switch(f) {
    case 0:return DkBlendFactor_Zero;case 1:return DkBlendFactor_One;
    case 4:return DkBlendFactor_SrcColor;case 5:return DkBlendFactor_InvSrcColor;
    case 6:return DkBlendFactor_SrcAlpha;case 7:return DkBlendFactor_InvSrcAlpha;
    case 8:return DkBlendFactor_DstColor;case 9:return DkBlendFactor_InvDstColor;
    case 10:return DkBlendFactor_DstAlpha;case 11:return DkBlendFactor_InvDstAlpha;
    case 12:return DkBlendFactor_ConstColor;case 13:return DkBlendFactor_InvConstColor;
    case 14:return DkBlendFactor_ConstAlpha;case 15:return DkBlendFactor_InvConstAlpha;
    case 16:return DkBlendFactor_SrcAlphaSaturate;default:return DkBlendFactor_Zero;
  }
}
static DkBlendOp BlendOp(uint32_t op) {
  constexpr DkBlendOp ops[]={DkBlendOp_Add,DkBlendOp_Sub,DkBlendOp_Min,DkBlendOp_Max,DkBlendOp_RevSub};
  return op<5?ops[op]:DkBlendOp_Add;
}
static DkPrimitive Primitive(uint32_t p) {
  switch(p) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:return DkPrimitive_Points;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:return DkPrimitive_Lines;
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:return DkPrimitive_LineStrip;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:return DkPrimitive_TriangleStrip;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:return DkPrimitive_LinesAdjacency;
    default:return DkPrimitive_Triangles;
  }
}

static void SetGuestDepthBias(DkCmdBuf commands,float constant,float slope) {
  // Match NVK's FORMAT representation explicitly: NV9097_SET_DEPTH_BIAS_CONTROL
  // (0x1110), DEPTH_FORMAT_DEPENDENT=1. deko3d exposes no setter for this bit.
  // The replay API takes an increasing-method packet: one argument, graphics
  // subchannel 0, and the method expressed in words (deko3d command.h/helpers.h).
  constexpr uint32_t format_dependent[]={
      (1u<<29)|(1u<<16)|(0x1110u>>2),1u};
  dkCmdBufReplayCmds(commands,format_dependent,2);
  // deko3d doubles units before writing SET_DEPTH_BIAS. Cancel that API scale
  // to retain the audited Vulkan register value for D24 and float depth alike.
  // Slope bias has no such scale. Source evidence is recorded in docs/deko3d.md.
  dkCmdBufSetDepthBias(commands,constant*0.5f,0.0f,slope);
}

struct Renderer::Impl {
  Renderer* owner=nullptr;
  rex::deko3d::ProveedorDeko3d* provider=nullptr;
  rex::memory::Memory* memory=nullptr; RegisterFile* regs=nullptr;
  Core core; std::unique_ptr<TextureCache> textures;
  CommandStateCache command_state;
  DrawBarrierPolicy draw_barriers;
  ImageBarrierPolicy image_barriers;
  std::unique_ptr<SpirvShaderTranslator> translator;
  rex::string::StringBuffer disasm;
  std::unordered_map<uint64_t,std::unique_ptr<SpirvShader>> shaders;
  SpirvShader *active_vs=nullptr,*active_ps=nullptr;
  std::unordered_map<uint64_t,std::unique_ptr<CompiledShader>> compiled;
  std::unordered_map<uint64_t,std::unique_ptr<Pipeline>> pipelines;
  std::unordered_map<uint32_t,CompiledShader*> geometry_shaders;
  std::unordered_map<RenderTargetKey,std::unique_ptr<RenderTarget>,TargetHash> render_targets;
  struct ResolveImage { Image image; uint32_t address=0,guest_format=0,origin_x=0,origin_y=0; uint64_t version=0,last_frame=0; bool is_depth=false,depth_words=false; };
  std::unordered_map<uint32_t,std::unique_ptr<ResolveImage>> resolves;
  struct Frontbuffer {RenderTarget* source=nullptr;ResolveImage* destination=nullptr;uint32_t width=0,height=0;} frontbuffer;
  uint32_t last_frontbuffer=0;
  // alias address -> {canonical root address, pixel origin x, pixel origin y}.
  std::unordered_map<uint32_t,std::array<uint32_t,3>> resolve_aliases;
  struct Crop { Image image; uint64_t version=0,last_frame=0; };
  std::map<std::array<uint32_t,5>,Crop> crops;
  uint64_t frame=1,work=0,rt_generation=1,resolve_version_clock=0,last_trim_frame=0;
  bool initialized=false,specialize_signs=false,retry=false;
  LocalVertexLookup<rex::deko3d::Subida> vertex_copies;
  VertexEpochCache<rex::deko3d::Subida> vertex_epoch_copies;
  VertexEpochCache<rex::deko3d::Subida>::Stats reported_vertex_epoch{};
  VertexWriteProbe vertex_write_probe;
  VertexWriteProbe::Stats reported_vertex_probe{};
  std::unordered_map<const Shader*,VertexRangePlan> vertex_range_plans;
  struct IndexCopy { rex::deko3d::Subida upload; IndexBounds bounds; bool output16=false; };
  WorkUploadCache<IndexCopy,4> index_copies{2048,8u<<20};
  WorkUploadCache<rex::deko3d::Subida,2> constant_copies{4096,4u<<20};
  // CPU conversions have unique identities assigned after an exact source
  // check. GPU residency is shared across slots and pinned through its last use.
  struct IndexConversion { IndexBounds bounds; uint64_t identity; bool output16=false; };
  WorkUploadCache<IndexConversion,4,16> converted_indices{32768,24u<<20};
  uint64_t index_identity=0;
  SmallIndexUpload small_indices;
  ResidentIndexUploadBatch resident_index_uploads;
  DeferredIndexUploadBatch deferred_index_uploads;
  DeferredIndexUploadBatch::Stats reported_deferred_indices{};
  bool ordered_index_uploads=false;
  uint64_t reported_index_sequential=0,reported_index_best_fit=0;
  ResidentVertexUploadBatch resident_vertex_uploads;
  SmallVertexUpload small_vertices;
  bool sequential_vertex_uploads=false;
  uint64_t reported_vertex_sequential=0,reported_vertex_best_fit=0;
  IndexValidationMemo index_validation;
  PersistentVertexCache index_gpu_cache{8u<<20};
  rex::deko3d::Subida index_cache_storage{};
  using ConstantArena=ConstantSlotArena<kWorkSlots>;
  static constexpr uint32_t kConstantSlotBytes=256u<<10;
  static constexpr uint32_t kConstantUploadSlotBytes=kConstantSlotBytes+(1u<<20);
  ConstantArena constant_arena;
  rex::deko3d::Subida constant_slot_base{};
  DkMemBlock constant_memory=nullptr;
  rex::deko3d::Subida constant_memory_base{};
  uint32_t constant_upload_used=0,constant_upload_peak=0;
  uint64_t constant_upload_overflows=0;
  struct ConstantBufferState {
    ConstantDeltaShadow shadow; DkBufExtents buffer{};
    // Revision of this buffer's owning immutable packing plan; zero means
    // unversioned input. It is never proof of GPU residency or slot ownership.
    uint64_t content_revision=0;
  };
  struct ResidentConstant : ConstantBufferState { ConstantArena::Lease lease;uint64_t last_work=UINT64_MAX;uint32_t logical_bytes=0; };
  struct DeltaConstant : ConstantBufferState { std::array<ResidentConstant,kWorkSlots> slots; };
  struct ConstantState {
    ConstantPackingPlan plan;
    GenerationUpload<DkBufExtents> upload;
    DeltaConstant delta;
    uint64_t generation=0,hash=0,hash_revision=0;
  };
  ConstantLayoutPool<ConstantState> constant_layouts;
  std::unordered_map<const Shader*,ConstantState*> float_constants;
  std::unordered_map<const CompiledShader*,ConstantState*> packed_system_constants;
  bool shared_constants_enabled=true;
  struct TextureDependency {
    uint32_t slot,address;uint64_t identity,content,resolve;uint32_t access;
    GuestMemoryLease guest_memory;
  };
  struct DrawTextureSet {
    std::array<DkResHandle,32> handles[2]{};uint32_t count[2]{};
    // Most draws use only a handful of entries. Don't initialize or copy the
    // unused 64-entry tail on every draw/cache store.
    std::array<TextureDependency,64> dependencies;
    uint32_t dependency_count=0;
    uint64_t resolve_clock=0;
    DrawTextureSet()=default;
    DrawTextureSet(const DrawTextureSet& from) {*this=from;}
    DrawTextureSet& operator=(const DrawTextureSet& from) {
      for(uint32_t stage=0;stage<2;++stage){handles[stage]=from.handles[stage];count[stage]=from.count[stage];}
      dependency_count=from.dependency_count;resolve_clock=from.resolve_clock;
      std::copy_n(from.dependencies.begin(),dependency_count,dependencies.begin());return *this;
    }
  };
  using DrawTextureMemo=PreviousDrawTextures<DrawTextureSet,128,4,128>;
  DrawTextureMemo draw_textures;
  DrawTextureMemo::Stats reported_draw_texture_stats{};
  GenerationUpload<DkBufExtents> bool_constants;
  DeltaConstant bool_delta,fetch_delta;
  std::vector<uint32_t> index_staging;
  std::vector<uint16_t> index16_staging;
  bool color_compression_enabled=true;
  std::vector<uint8_t> index_source_staging;
  PersistentVertexCache vertex_cache;
  SharedGeometryCache shared_vertices{kWorkSlots,kUploadBytes,4096,(16u<<20)-4096};
  SharedGeometryCache shared_indices{kWorkSlots,kUploadBytes,16u<<20,8u<<20};
  uint64_t completed_before=0;
  bool shared_geometry_enabled=true;
  uint32_t stream_bytes=kUploadBytes,slot_stream_offset=0;
  PersistentVertexCache::Stats reported_vertex_cache_stats{};
  rex::deko3d::Subida vertex_cache_storage{};
  bool geometry_cache_enabled=true,transient_next_work=false;
  uint64_t transient_work_fallbacks=0,reported_transient_work_fallbacks=0;
  DkGpuAddr stream_base=0; uint32_t gamma_ramp[256]{};
  uint8_t* stream_cpu_base=nullptr;
  bool geometry_push_upload=false;
  ZeroPrefixTracker<kWorkSlots> zero_prefixes;
  std::filesystem::path cache_root,prepared_cache_root,warmup_root;
  std::string compiler_id=rex::deko3d::IdCompilador();
  std::set<std::pair<uint64_t,uint64_t>> recorded_variants;
  bool warming_up=false;
  std::chrono::steady_clock::time_point variant_retry_time{};
  uint64_t shader_code_budget_used=0;
  bool cpu_profile_enabled=true,gpu_profile_enabled=false;
  struct CpuProfile {
    uint64_t draw_ns=0,resolve_ns=0,swap_ns=0,load_shader_ns=0;
    uint64_t draw_begin_ns=0,setup_ns=0,index_ns=0,vertex_ns=0,constants_ns=0;
    uint64_t texture_ns=0,record_ns=0,mirror_ns=0;
    uint64_t vertex_hash_ns=0,vertex_hash_bytes=0,vertex_hash_calls=0,vertex_copy_ns=0;
    uint64_t geometry_push_uploads=0,geometry_push_commands=0,geometry_push_bytes=0,geometry_push_fallbacks=0;
    uint64_t zero_prefix_writes=0,zero_prefix_reuses=0,zero_prefix_avoided_bytes=0;
    uint64_t vertex_declared_bytes=0,vertex_window_bytes=0,vertex_clipped=0,vertex_range_fallbacks=0;
    uint64_t vertex_local_hits=0,vertex_local_misses=0,vertex_padding_bytes=0;
    uint64_t dynamic_reuse_hits=0,dynamic_reuse_bytes=0,dynamic_copy_bytes=0;
    uint64_t index_hits=0,index_misses=0,index_uncached=0,index_copy_bytes=0,index_avoided_bytes=0;
    uint64_t index_hash_ns=0,index_convert_ns=0,index_copy_ns=0,index_hash_bytes=0;
    uint64_t vertex_grid_reduced=0,vertex_grid_selected_saved=0,vertex_grid_copy_saved=0;
    uint64_t index_batch_payloads=0,index_batch_payload_bytes=0,index_batch_writes=0,index_batch_write_bytes=0;
    uint64_t index_batch_capture_ns=0,index_batch_write_ns=0,index_batch_submit_ns=0,index_direct_writes=0,index_batch_reserve_bytes=0;
    uint64_t index_resident_payloads=0,index_resident_payload_bytes=0,index_resident_writes=0,index_resident_written_bytes=0;
    uint64_t index_resident_capture_ns=0,index_resident_drain_ns=0,index_resident_submit_ns=0;
    uint64_t index_write_size_count[3]{},index_write_size_bytes[3]{},index_write_size_ns[3]{};
    uint64_t index_direct_resident_ns=0,index_direct_transient_ns=0,index_direct_bytes=0,index_direct_written_bytes=0;
    uint64_t index_direct_unaligned_sources=0,index_direct_tails=0;
    uint64_t vertex_batch_payloads=0,vertex_batch_bytes=0,vertex_batch_writes=0,vertex_batch_write_bytes=0,vertex_batch_reserved=0;
    uint64_t vertex_batch_capture_ns=0,vertex_batch_write_ns=0,vertex_batch_submit_ns=0,vertex_direct_writes=0;
    uint64_t vertex_resident_payloads=0,vertex_resident_payload_bytes=0,vertex_resident_writes=0,vertex_resident_written_bytes=0;
    uint64_t vertex_resident_capture_ns=0,vertex_resident_drain_ns=0,vertex_resident_submit_ns=0;
    uint64_t vertex_direct_resident_ns=0,vertex_direct_transient_ns=0,vertex_direct_resident_writes=0,vertex_direct_transient_writes=0;
    uint64_t vertex_direct_resident_bytes=0,vertex_direct_transient_bytes=0,vertex_direct_written_bytes=0,vertex_direct_unaligned_sources=0,vertex_direct_tails=0;
    uint64_t index_validation_probe_ns=0,index_validation_candidates=0,index_validation_hits=0,index_validation_rejected=0,index_full_hashes=0;
    uint64_t index_compact_attempts=0,index_compact_accepted=0,index_compact_rejected=0,index_compact_ns=0,index_compact_draws=0;
    uint64_t index16_draws=0,index32_draws=0,index16_saved_bytes=0;
    uint64_t clears_deferred=0,clears_materialized=0,clears_elided=0,clears_executed=0;
    uint64_t copy3d_calls=0,copy3d_pixels=0,copy3d_compressed=0,copy_transfer_calls=0;
    uint64_t front_deferred=0,front_direct=0,front_copies=0,front_exchanges=0,front_replaced=0;
    uint64_t front_deferred_pixels=0,front_copy_pixels=0;
    uint64_t image_fragments=0,image_primitives=0,image_barriers_skipped=0;
    uint64_t clears_rectangles=0,clears_masked=0,clears_repeated=0,clears_capacity_drains=0;
    uint64_t clear_pixels_queued=0,clear_pixels_executed=0,clear_partial_consumes=0;
    uint64_t clear_regions_merged=0,clear_expanded_pixels=0,clear_expanded_consumes=0;
    uint64_t constant_slot_allocations=0,constant_slot_reuses=0,constant_slot_unchanged=0;
    uint64_t constant_slot_initial_avoided=0,constant_slot_reentry_push_bytes=0,constant_slot_fallbacks=0;
    uint64_t constant_hits=0,constant_misses=0,constant_uncached=0,constant_copy_bytes=0,constant_avoided_bytes=0;
    uint64_t constant_lookup_ns=0,constant_copy_ns=0,vertex_cache_ns=0;
    uint64_t constant_initial_ns=0,constant_scan_ns=0,constant_push_ns=0;
    uint64_t texture_set_hits=0,texture_set_misses=0,texture_set_ineligible=0;
    uint64_t setup_hits=0,setup_misses=0,setup_state_misses=0,setup_target_misses=0,setup_shader_misses=0,setup_sign_misses=0;
    uint64_t constant_version_checks=0,constant_version_skips=0,texture_dependency_misses=0;
    uint64_t constant_group_checks=0,constant_group_skips=0,constant_revision_hits=0;
    uint64_t constant_packed_copy_bytes=0,constant_partial_packs=0,constant_bulk_updates=0;
    uint64_t system_generation_hits=0;
    uint64_t constant_content_hashes=0,constant_content_hash_bytes=0;
    uint64_t index_cpu_hits=0,index_cpu_misses=0,index_cpu_resets=0,index_cpu_lookup_ns=0;
    uint64_t constant_generation_hits=0,constant_pack_ns=0,constant_packs=0,system_pack_reuses=0;
    uint64_t constant_pack_bank_bytes=0,constant_pack_used_bytes=0;
    uint64_t vertex_fast_hits=0,vertex_table_lookups=0,index_fast_hits=0,index_table_lookups=0;
    uint64_t upload_cache_clear_ns=0;
    uint64_t vertex_dynamic_windows=0,vertex_dynamic_window_saved=0,vertex_dynamic_copy_saved=0;
    uint64_t vertex_epoch_lookup_ns=0,vertex_epoch_dynamic_hits=0,vertex_epoch_dynamic_bytes=0;
    uint64_t range_write_ns=0;
    uint64_t index_gpu_hits=0,index_gpu_misses=0,index_gpu_avoided_bytes=0,index_gpu_lookup_ns=0;
    uint64_t constant_initial_bytes=0,constant_push_bytes=0,constant_push_commands=0,constant_delta_unchanged=0;
    uint64_t constant_scan_extent_bytes=0,constant_scan_skipped_bytes=0,constant_narrow_scans=0;
    uint64_t state_emitted=0,state_skipped=0,draw_full=0,draw_data_invalidations=0,draw_no_barrier=0;
    uint64_t guest_samples[4]{},max_target_width=0,max_target_height=0;
    uint64_t begin_ns=0,submit_ns=0,maintenance_ns=0,barriers=0;
    uint64_t swap_prepare_ns=0,refresh_ns=0;
    uint64_t queue_partial_sends=0,queue_final_sends=0,queue_empty_kicks=0;
    uint64_t queue_slot_checks=0,queue_slot_blocked=0,queue_blocked_ns=0,queue_ready_ns=0;
    uint64_t queue_reuse_age_ns=0,queue_wait_draws=0,queue_wait_pushes=0;
    uint64_t queue_begin_cpu_ns=0,queue_begin_cpu_samples=0,ring_cpu_ns=0;
  } profile,reported_profile;
  QueueWorkState queue_work;
  struct SubmittedWork {uint64_t time=0,draws=0,pushes=0;};
  std::array<SubmittedWork,kWorkSlots> submitted_work{};
  uint64_t work_draw_begin=0,work_push_begin=0,ring_cpu_before=0;
  TextureProfile reported_texture_profile{};
  GpuProfile gpu_profile;
  ValidationCache validation_cache;
  ValidationCache::Stats reported_validation_stats{};
  GuestMemoryWrites::Stats reported_guest_writes{};
  std::atomic<uint64_t> guest_memory_epoch{0};
  uint64_t observed_guest_memory_epoch=0;
  std::atomic<uint64_t> vertex_reuse_epoch{0};
  uint64_t observed_vertex_reuse_epoch=0;
  std::array<std::atomic<uint64_t>,size_t(GuestMemoryBoundary::kCount)> guest_boundary_counts{};
  std::array<uint64_t,size_t(GuestMemoryBoundary::kCount)> reported_guest_boundary_counts{};
  uint64_t guest_revalidations=0,vertex_reuse_boundaries=0;
  uint64_t reported_guest_revalidations=0,reported_vertex_reuse_boundaries=0;
  uint64_t reported_vertex_reuse_epoch=0;
  uint64_t reported_fence_ns=0,reported_gpu_ns=0,reported_gpu_samples=0,reported_gpu_rejected=0;
  uint64_t reported_sample_draws=0,reported_draw_work_ns=0,reported_draw_work_samples=0;
  uint64_t frame_fence_before=0,frame_texture_before=0,last_profile_frame_ns=0;
  uint64_t frame_draws_before=0,frame_shaders_before=0,frame_pipelines_before=0;
  uint64_t frame_uploads_before=0,frame_targets_before=0;
  PipelineLayout layout{}; DrawSetup setup{};
  DrawSetupIdentity setup_identity{};
  bool setup_cached=false;
  CpuRegisterVersions system_versions;
  uint64_t reported_local_fast=0,reported_local_map=0,reported_image_capacity=0;
  TextureChecks::Stats reported_texture_checks{};
  SpirvShaderTranslator::SystemConstants system_constants{};
  VkRect2D bound_scissor{};
  RendererStats& stats(){return owner->stats_;}
  uint32_t HostMsaa(xenos::MsaaSamples) const {return 0;}
  void SyncGuestMemory() {
    const uint64_t ownership=vertex_reuse_epoch.load(std::memory_order_acquire);
    if(ownership!=observed_vertex_reuse_epoch) {
      observed_vertex_reuse_epoch=ownership;++vertex_reuse_boundaries;
    }
    const uint64_t epoch=guest_memory_epoch.load(std::memory_order_acquire);
    if(epoch!=observed_guest_memory_epoch) {
      validation_cache.Revalidate();observed_guest_memory_epoch=epoch;
      ++guest_revalidations;
    }
    validation_cache.BeginFrame(frame);
  }
  void Begin() {
    if(core.TrabajoAbierto())return;
    if(small_indices.pending()||small_vertices.pending()||resident_index_uploads.pending()||deferred_index_uploads.pending()||resident_vertex_uploads.pending())rex::deko3d::Fatal("pending geometry uploads at work-slot reuse");
    validation_cache.Revalidate();
    ProfileScope timer(profile.begin_ns,cpu_profile_enabled);
    const bool reused=work>=kWorkSlots;
    const bool ready=!cpu_profile_enabled || !reused || core.TrabajoTerminado(work-kWorkSlots);
    const uint64_t fence_before=core.NsEsperandoValla();
    const uint64_t cpu_before=cpu_profile_enabled?RingThreadCpuNs():0;
    if(cpu_profile_enabled && reused) {
      ++profile.queue_slot_checks;
      const auto& submitted=submitted_work[work%kWorkSlots];
      if(submitted.time)profile.queue_reuse_age_ns+=ProfileClockNs()-submitted.time;
      if(!ready) {
        ++profile.queue_slot_blocked;
        profile.queue_wait_draws+=submitted.draws;profile.queue_wait_pushes+=submitted.pushes;
      }
    }
    core.EmpezarTrabajo(work);
    if(cpu_profile_enabled) {
      const uint64_t cpu_after=RingThreadCpuNs();
      if(cpu_before && cpu_after>=cpu_before){profile.queue_begin_cpu_ns+=cpu_after-cpu_before;++profile.queue_begin_cpu_samples;}
      (ready?profile.queue_ready_ns:profile.queue_blocked_ns)+=core.NsEsperandoValla()-fence_before;
    }
    queue_work.Begin();work_draw_begin=stats().draws;work_push_begin=profile.constant_push_commands;
    command_state.Invalidate();draw_textures.Invalidate();draw_barriers.BeginWork();image_barriers.BeginWork();
    gpu_profile.BeginWork(core,work,gpu_profile_enabled);
    timer.Next(profile.maintenance_ns);
    textures->BeginWork(work);textures->BeginFrame(frame);TrimCaches();vertex_copies.Clear();
    { ProfileScope clear_timer(profile.upload_cache_clear_ns,cpu_profile_enabled);
      index_copies.Clear();constant_copies.Clear(); }
    // The SDK upload heap is contiguous: four 64 MiB slots, with a protected
    // vertex/index prefix in each. Shared offsets address the whole heap.
    vertex_cache.BeginWork(work,kWorkSlots);
    index_gpu_cache.BeginWork(work,kWorkSlots);
    const bool full_slot_retry=transient_next_work;
    const bool persistent=geometry_cache_enabled&&!full_slot_retry;
    if(full_slot_retry)++transient_work_fallbacks;
    transient_next_work=false;
    if(shared_geometry_enabled) {
      if(work>=kWorkSlots)completed_before=std::max(completed_before,work-kWorkSlots+1);
      while(completed_before<work&&core.TrabajoTerminado(completed_before))++completed_before;
      // Other slots may reference this slot's shared prefix. A rare oversized
      // upload retry must drain those readers before using the complete slot.
      if(full_slot_retry){core.EsperarGpu();completed_before=work;}
      shared_vertices.BeginWork(work,completed_before);shared_indices.BeginWork(work,completed_before);
      if(full_slot_retry && (!shared_vertices.BlockRegion(work%kWorkSlots)||!shared_indices.BlockRegion(work%kWorkSlots)))
        rex::deko3d::Fatal("shared geometry retry attempted to overwrite live GPU work");
    }
    if(!persistent)vertex_cache.DiscardActiveSlot();
    const bool persistent_indices=persistent&&REXCVAR_GET(nx_native_index_cache);
    if(!persistent_indices)index_gpu_cache.DiscardActiveSlot();
    const uint32_t prefix=shared_geometry_enabled?(persistent?(24u<<20):256):
        (persistent?vertex_cache.capacity_bytes():256)+(persistent_indices?index_gpu_cache.capacity_bytes():0);
    const bool retain_constants=!full_slot_retry && REXCVAR_GET(nx_native_constant_slots) && REXCVAR_GET(nx_native_delta_constants);
    // Nucleo::EmpezarTrabajo has waited for this slot's previous fence. These
    // buffers are never bound across slots, so its exclusive boundary suffices.
    const uint64_t constant_completed=work>=kWorkSlots?work-kWorkSlots+1:0;
    if(!constant_arena.BeginWork(work,constant_completed,constant_memory?0:prefix,kConstantSlotBytes,retain_constants))
      rex::deko3d::Fatal("constant slot reused before its GPU fence");
    if(core.SubidaLibre()!=kUploadBytes)rex::deko3d::Fatal("shared geometry requires the first reservation in a work slot");
    auto first=core.Reservar(prefix+(!constant_memory&&retain_constants?kConstantSlotBytes:0),256);
    if(!first)rex::deko3d::Fatal("cannot reserve the protected geometry prefix");
    constant_slot_base=first;
    if(constant_memory) {
      // EmpezarTrabajo above has waited for this slot. No constant allocation
      // is shared across slots, including overflow / non-delta snapshots.
      const uint32_t offset=uint32_t(work%kWorkSlots)*kConstantUploadSlotBytes;
      constant_slot_base={constant_memory_base.cpu+offset,constant_memory_base.gpu+offset};
      constant_upload_used=retain_constants?kConstantSlotBytes:0;
    }
    slot_stream_offset=shared_geometry_enabled?uint32_t(work%kWorkSlots)*kUploadBytes:0;
    stream_bytes=shared_geometry_enabled?kWorkSlots*kUploadBytes:kUploadBytes;
    stream_base=first.gpu-slot_stream_offset;stream_cpu_base=first.cpu-slot_stream_offset;
    vertex_cache_storage=persistent&&first?rex::deko3d::Subida{first.cpu-slot_stream_offset,stream_base}:rex::deko3d::Subida{};
    index_cache_storage=persistent_indices&&first?
        (shared_geometry_enabled?vertex_cache_storage:rex::deko3d::Subida{first.cpu+vertex_cache.capacity_bytes(),first.gpu+vertex_cache.capacity_bytes()}):rex::deko3d::Subida{};
    if(first) {
      const uint32_t zero_bytes=persistent?PersistentVertexCache::kReservedPrefixBytes:256;
      // This prefix is excluded from every allocation while this work owns it.
      // Retain its zero contents across fenced reuse of the same slot, but a
      // smaller transient prefix or remapped heap invalidates the larger proof.
      if(zero_prefixes.NeedsInitialize(work%kWorkSlots,first.cpu,first.gpu,zero_bytes)) {
        memset(first.cpu,0,zero_bytes);++profile.zero_prefix_writes;
      } else {++profile.zero_prefix_reuses;profile.zero_prefix_avoided_bytes+=zero_bytes;}
    }
    core.BindDescriptores();
  }
  uint32_t WriteGeometryUpload(uint8_t* destination,const uint8_t* source,
                               uint32_t bytes,uint32_t capacity,bool vertex) {
    if(geometry_push_upload) {
      const uintptr_t base=reinterpret_cast<uintptr_t>(stream_cpu_base);
      const uintptr_t target=reinterpret_cast<uintptr_t>(destination);
      if(base && target>=base && target-base<=stream_bytes && capacity<=stream_bytes-(target-base) &&
         uint64_t(target-base)<=UINT64_MAX-stream_base) {
        DkCmdBuf commands=nullptr;
        const uint32_t written=EmitUploadCommands(stream_base+uint64_t(target-base),source,bytes,capacity,
            [&](uint64_t address,const uint8_t* data,uint32_t size) {
              if(!commands)commands=core.ComandosPrevios();
              dkCmdBufPushData(commands,DkGpuAddr(address),data,size);
              ++profile.geometry_push_commands;
            });
        if(written) {
          ++profile.geometry_push_uploads;profile.geometry_push_bytes+=written;return written;
        }
      }
      ++profile.geometry_push_fallbacks;
    }
    // The default native path is unchanged. Command uploads are an explicit
    // A/B option: they add GPU DMA and a pre-list barrier, which CPU store
    // timings alone cannot evaluate. All destinations here are fresh owned
    // allocations; immutable cache hits never call this function.
    if(vertex?REXCVAR_GET(nx_native_vertex_write_fast):REXCVAR_GET(nx_native_index_write_fast))
      return vertex?WriteVertexBytes(destination,source,bytes,capacity):WriteIndexBytes(destination,source,bytes,capacity);
    memcpy(destination,source,bytes);return bytes;
  }
  void WriteSmallIndices(uint8_t* destination,const uint8_t* source,uint32_t bytes) {
    ProfileScope timer(profile.index_batch_write_ns,cpu_profile_enabled);
    const uint32_t written=WriteGeometryUpload(destination,source,bytes,(bytes+15)&~15u,false);
    if(cpu_profile_enabled){++profile.index_batch_writes;profile.index_batch_write_bytes+=written;}
  }
  void ProbeVertexWrite(const uint8_t* source,uint32_t bytes) {
    if(!cpu_profile_enabled || !REXCVAR_GET(nx_native_vertex_write_probe) ||
       !vertex_write_probe.Due(frame,bytes))return;
    const uint64_t start=ProfileClockNs();
    bytes=std::min(bytes,VertexWriteProbe::kMaxBytes);
    // Reserve a separate throwaway range in the actual upload heap. Never
    // rewrite a cached payload or a range already referenced by a draw.
    const auto scratch=core.Reservar((bytes+15)&~15u,16);
    if(scratch)vertex_write_probe.Run(source,scratch.cpu,bytes,REXCVAR_GET(nx_native_vertex_write_fast));
    else vertex_write_probe.AllocationSkipped();
    vertex_write_probe.AddOverhead(ProfileClockNs()-start);
  }
  void WriteResidentIndices(uint8_t* destination,const uint8_t* source,uint32_t bytes) {
    const uint64_t start=cpu_profile_enabled?ProfileClockNs():0;
    WriteGeometryUpload(destination,source,bytes,bytes,false);
    if(cpu_profile_enabled) {
      const uint64_t ns=ProfileClockNs()-start;
      const unsigned bucket=bytes<=256?0:bytes<=4096?1:2;
      profile.index_resident_drain_ns+=ns;
      ++profile.index_resident_writes;profile.index_resident_written_bytes+=bytes;
      ++profile.index_write_size_count[bucket];profile.index_write_size_bytes[bucket]+=bytes;profile.index_write_size_ns[bucket]+=ns;
    }
  }
  void WriteSmallVertices(uint8_t* destination,const uint8_t* source,uint32_t bytes) {
    ProfileScope timer(profile.vertex_batch_write_ns,cpu_profile_enabled);
    const uint32_t written=WriteGeometryUpload(destination,source,bytes,(bytes+15)&~15u,true);
    if(cpu_profile_enabled){++profile.vertex_batch_writes;profile.vertex_batch_write_bytes+=written;}
  }
  void WriteResidentVertices(uint8_t* destination,const uint8_t* source,uint32_t bytes) {
    ProfileScope timer(profile.vertex_resident_drain_ns,cpu_profile_enabled);
    WriteGeometryUpload(destination,source,bytes,bytes,true);
    if(cpu_profile_enabled){++profile.vertex_resident_writes;profile.vertex_resident_written_bytes+=bytes;}
  }
  void WriteDirectVertices(uint8_t* destination,const uint8_t* source,uint32_t bytes,bool resident) {
    ProfileScope timer(profile.vertex_copy_ns,cpu_profile_enabled);
    ProfileScope kind(resident?profile.vertex_direct_resident_ns:profile.vertex_direct_transient_ns,cpu_profile_enabled);
    const uint32_t written=WriteGeometryUpload(destination,source,bytes,(bytes+15)&~15u,true);
    if(cpu_profile_enabled) {
      ++profile.vertex_direct_writes;profile.vertex_direct_written_bytes+=written;
      if(resident){++profile.vertex_direct_resident_writes;profile.vertex_direct_resident_bytes+=bytes;}
      else {++profile.vertex_direct_transient_writes;profile.vertex_direct_transient_bytes+=bytes;}
      profile.vertex_direct_unaligned_sources+=(reinterpret_cast<uintptr_t>(source)&15)!=0;profile.vertex_direct_tails+=(bytes&15)!=0;
    }
  }
  void DrainGeometryUploads() {
    if(resident_vertex_uploads.pending()) { ProfileScope timer(profile.vertex_resident_submit_ns,cpu_profile_enabled);
      resident_vertex_uploads.Flush([&](auto* dst,const auto* src,uint32_t bytes){WriteResidentVertices(dst,src,bytes);}); }
    if(resident_index_uploads.pending()) { ProfileScope timer(profile.index_resident_submit_ns,cpu_profile_enabled);
      resident_index_uploads.Flush([&](auto* dst,const auto* src,uint32_t bytes){WriteResidentIndices(dst,src,bytes);}); }
    if(deferred_index_uploads.pending()) { ProfileScope timer(profile.index_resident_submit_ns,cpu_profile_enabled);
      deferred_index_uploads.Flush([&](auto* dst,const auto* src,uint32_t bytes){WriteResidentIndices(dst,src,bytes);}); }
    if(small_vertices.pending()) { ProfileScope timer(profile.vertex_batch_submit_ns,cpu_profile_enabled);
      small_vertices.Flush([&](auto* dst,const auto* src,uint32_t bytes){WriteSmallVertices(dst,src,bytes);}); }
    // Drain before EVERY send, including partial sends. These snapshots own
    // fresh allocations; later uploads cannot overwrite earlier GPU readers.
    if(small_indices.pending()) { ProfileScope timer(profile.index_batch_submit_ns,cpu_profile_enabled);
      small_indices.Flush([&](auto* dst,const auto* src,uint32_t bytes){WriteSmallIndices(dst,src,bytes);}); }
  }
  void Flush() {if(core.TrabajoAbierto()){
    DrainGeometryUploads();
    ProfileScope timer(profile.submit_ns,cpu_profile_enabled);
    gpu_profile.EndWork(core,work,stats().draws-work_draw_begin);
    if(cpu_profile_enabled)submitted_work[work%kWorkSlots]={ProfileClockNs(),stats().draws-work_draw_begin,profile.constant_push_commands-work_push_begin};
    core.TerminarTrabajo();++profile.queue_final_sends;
    queue_work.Closed();++work;++stats().submissions;
  }}
  void Kick() {
    if(!core.TrabajoAbierto())return;
    if(!REXCVAR_GET(nx_native_partial_kicks)){Flush();return;}
    if(!queue_work.pending()){++profile.queue_empty_kicks;return;}
    // Present, explicit Flush and allocation retries still close the slot.
    // Idle kicks only need execution to progress, not a fresh upload arena.
    if(!queue_work.CanSendPartial()){Flush();return;}
    DrainGeometryUploads();
    ProfileScope timer(profile.submit_ns,cpu_profile_enabled);
    core.EnviarParcial();queue_work.SentPartial();
    // Preserve the former idle/work boundary's guest-memory validation. Only
    // immutable upload residency and ordered GPU state survive this send.
    validation_cache.Revalidate();
    ++profile.queue_partial_sends;++stats().submissions;
  }
  void Barrier(){if(cpu_profile_enabled)++profile.barriers;dkCmdBufBarrier(core.comandos(),DkBarrier_Full,DkInvalidateFlags_Image|DkInvalidateFlags_Descriptors|DkInvalidateFlags_Shader|DkInvalidateFlags_Zcull);draw_barriers.FullBarrier();image_barriers.FullBarrier();}
  void ImageBarrier(ImageBarrierPolicy::Action action) {
    using A=ImageBarrierPolicy::Action;
    if(action==A::Full)Barrier();
    else if(action==A::None)++profile.image_barriers_skipped;
    else {
      dkCmdBufBarrier(core.comandos(),action==A::Fragments?DkBarrier_Fragments:DkBarrier_Primitives,DkInvalidateFlags_Image);
      if(action==A::Fragments)++profile.image_fragments;else ++profile.image_primitives;
    }
  }
  void BeforeGraphics(bool samples=false,bool helper=false) {
    const bool optimized=REXCVAR_GET(nx_native_image_barriers);
    if(!optimized && !samples && !helper)return; // retain the existing guest draw policy in A/B mode
    ImageBarrier(image_barriers.BeforeGraphics(samples,helper,optimized));
  }
  void BeforeImages(std::span<const ImageAccess> accesses) {
    ImageBarrier(image_barriers.BeforeImages(accesses,REXCVAR_GET(nx_native_image_barriers)));
  }
  uint64_t ResolveVersion(uint32_t address) const {
    if(auto alias=resolve_aliases.find(address);alias!=resolve_aliases.end())address=alias->second[0];
    const auto it=resolves.find(address);return it==resolves.end()?0:it->second->version;
  }
  bool TextureSetCurrent(DrawTextureSet& set) const {
    const bool check_resolves=set.resolve_clock!=resolve_version_clock;
    for(uint32_t i=0;i<set.dependency_count;++i) {
      const auto& dep=set.dependencies[i];
      if(!textures->BindingCurrent(dep.slot,dep.identity,dep.content) ||
          !dep.guest_memory.Current(validation_cache.guest_writes()) ||
          (check_resolves && ResolveVersion(dep.address)!=dep.resolve))return false;
    }
    set.resolve_clock=resolve_version_clock;return true;
  }
  void DrawBarrier() {
    const auto action=draw_barriers.BeforeDraw(REXCVAR_GET(nx_native_draw_barriers));
    if(action==DrawBarrierPolicy::Action::Full) {Barrier();if(cpu_profile_enabled)++profile.draw_full;}
    else if(action==DrawBarrierPolicy::Action::InvalidateData) {
      dkCmdBufBarrier(core.comandos(),DkBarrier_None,DkInvalidateFlags_Shader);
      if(cpu_profile_enabled)++profile.draw_data_invalidations;
    } else if(cpu_profile_enabled)++profile.draw_no_barrier;
  }
  void FinishProfileFrame();
  void TrimCaches();
  CompiledShader* Compile(std::span<const uint32_t> words,ShaderStage stage);
  CompiledShader* CompileText(std::string_view source,ShaderStage stage);
  bool LoadDksh(CompiledShader& shader,ShaderStage stage,std::vector<uint8_t>* saved_binary=nullptr);
  bool LoadShaderCode(CompiledShader& shader,std::span<const uint8_t> binary,const char* label);
  void RecordVariant(const SpirvShader::SpirvTranslation& translation);
  void WarmupObservedShaders();
  CompiledShader* CompileGeometry(GeometryShaderKey key);
  bool EnsureTranslated(SpirvShader::SpirvTranslation* t,bool* pending);
  PipelineLayout* GetPipelineLayout(uint32_t,uint32_t,uint32_t,uint32_t){return &layout;}
  Pipeline* GetPipeline(const PipelineKey& key,SpirvShader::SpirvTranslation* vs,SpirvShader::SpirvTranslation* ps,CompiledShader* gs,uint64_t,bool* pending);
  RenderTarget* GetRenderTarget(const RenderTargetKey& key,uint32_t height);
  void Clear(Image& image,bool depth,float z,uint8_t stencil,uint32_t stencil_mask,const float* color,const VkRect2D& rect,bool clear_z=true);
  void ExecuteClear(Image&,bool,float,uint8_t,uint32_t,const float*,const VkRect2D&,bool);
  void MaterializeClear(Image&,const ClearRect* area=nullptr);
  void MaterializeFrontbuffer();
  void BeforeImageWrite(Image&,bool discard_whole_color=false);
  bool DeferFrontbuffer(RenderTarget&,ResolveImage&,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,bool,bool);
  bool CopyColor3D(Image&,Image&,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
  void Copy(Image& from,Image& to,uint32_t sx,uint32_t sy,uint32_t dx,uint32_t dy,uint32_t width,uint32_t height);
  void UpdateSystemConstants(bool,const draw_util::ViewportInfo&,uint32_t,uint32_t,const SpirvShader*,const SpirvShader*);
  bool BuildDrawSetup(uint32_t initiator);
  bool GetFastClearRectangle(StencilClearRect&,bool);
  void MirrorClears();
  bool DrawOnce(const uint32_t*,uint32_t);
  void Draw(const uint32_t*,uint32_t);
  void Resolve();
  bool DispatchDepthResolve(RenderTarget&, ResolveImage&, ResolveImage*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, bool);
  std::optional<TextureResourceBinding> Texture(const SpirvShader::TextureBinding&,const SpirvShader::SamplerBinding&);
  Image* ResolveSample(uint32_t address,uint32_t width,uint32_t height);
  void Swap(rex::ui::Presenter*,uint32_t,uint32_t,uint32_t);
};

CompiledShader* Renderer::Impl::Compile(std::span<const uint32_t> words,ShaderStage stage) {
  const uint64_t key=shader_cache::SpirvKey(words,stage,compiler_id);
  if(auto it=compiled.find(key);it!=compiled.end())return it->second.get();
  auto result=std::make_unique<CompiledShader>();
  std::filesystem::path prepared_path;
  if(!prepared_cache_root.empty()) {
    prepared_path=prepared_cache_root/fmt::format("{:016X}.fshader",key);
    shader_cache::Entry entry;std::string error;
    if(shader_cache::Load(prepared_path,key,stage,compiler_id,entry,error)) {
      const bool valid_constants=std::all_of(entry.info.system_constant_copy_ranges.begin(),entry.info.system_constant_copy_ranges.end(),[](const auto& range) {
        return uint64_t(range.source_offset)+range.size<=sizeof(SpirvShaderTranslator::SystemConstants);
      });
      if(valid_constants)result->info=std::move(entry.info);
      if(valid_constants&&LoadShaderCode(*result,entry.dksh,"FEAR prepared shader")) {
        ++stats().shader_prepared_hits;
        auto* ptr=result.get();compiled.emplace(key,std::move(result));return ptr;
      }
      if(!valid_constants) {
        ++stats().shader_cache_invalid;
        if(stats().shader_cache_invalid<=8)REXLOG_WARN("[deko-cache] ignored {}: system constants exceed runtime ABI",prepared_path.filename().string());
      }
    } else {
      std::error_code ec;
      if(std::filesystem::exists(prepared_path,ec)) {
        ++stats().shader_cache_invalid;
        if(stats().shader_cache_invalid<=8)REXLOG_WARN("[deko-cache] ignored {}: {}",prepared_path.filename().string(),error);
      }
    }
  }
  const auto convert_start=std::chrono::steady_clock::now();
  result->info=ConvertSpirv(words,stage);
  stats().shader_convert_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-convert_start).count();
  if(!result->info){REXLOG_ERROR("[deko] SPIRV conversion {:016X}: {}",key,result->info.error);return nullptr;}
  for(const auto& warning:result->info.warnings)REXLOG_WARN("[deko] shader {:016X}: {}",key,warning);
  std::vector<uint8_t> binary;
  if(!LoadDksh(*result,stage,&binary))return nullptr;
  if(!prepared_path.empty()) {
    shader_cache::Entry entry{result->info,std::move(binary)};std::string error;
    if(!shader_cache::Save(prepared_path,key,stage,compiler_id,entry,error)) {
      ++stats().shader_cache_write_failures;
      if(stats().shader_cache_write_failures<=8)REXLOG_WARN("[deko-cache] cannot save prepared shader: {}",error);
    }
  }
  auto* ptr=result.get();compiled.emplace(key,std::move(result));return ptr;
}
CompiledShader* Renderer::Impl::CompileText(std::string_view source,ShaderStage stage) {
  uint64_t key=XXH3_64bits_withSeed(source.data(),source.size(),uint64_t(stage));
  if(auto it=compiled.find(key);it!=compiled.end())return it->second.get();
  auto result=std::make_unique<CompiledShader>();result->info.source.assign(source.data(),source.size());result->info.stage=stage;
  if(!LoadDksh(*result,stage))return nullptr;
  auto* ptr=result.get();compiled.emplace(key,std::move(result));return ptr;
}
bool Renderer::Impl::LoadShaderCode(CompiledShader& shader,std::span<const uint8_t> binary,const char* label) {
  if(!core.CargarShader(shader.shader,binary,label))return false;
  // Conservative accounting includes the control section as well as alignment.
  // Warmup stops below half the fixed 64 MiB code pool, leaving room for new scenes.
  shader_code_budget_used+=rex::align(uint64_t(binary.size()),uint64_t(256));
  return true;
}
bool Renderer::Impl::LoadDksh(CompiledShader& shader,ShaderStage stage,std::vector<uint8_t>* saved_binary) {
  // Cache the exact generated GLSL and compiler identity. Converter changes,
  // shader stage changes and compiler updates cannot reuse an older binary.
  uint64_t seed=XXH3_64bits(compiler_id.data(),compiler_id.size())^uint64_t(stage);
  uint64_t key=XXH3_64bits_withSeed(shader.info.source.data(),shader.info.source.size(),seed);
  struct Header {uint64_t key,bytes,checksum;};
  std::filesystem::path path;
  if(!cache_root.empty()) {
    path=cache_root/(std::to_string(key)+".fcache");
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    if(input) {
      auto length=input.tellg();input.seekg(0);Header header{};
      input.read(reinterpret_cast<char*>(&header),sizeof(header));
      if(input&&header.key==key&&header.bytes>=24&&header.bytes<=(4u<<20)&&
         length==std::streamoff(sizeof(header)+header.bytes)) {
        std::vector<uint8_t> bytes(size_t(header.bytes));
        input.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
        if(input&&header.checksum==XXH3_64bits(bytes.data(),bytes.size())&&
           LoadShaderCode(shader,bytes,"FEAR cached shader")) {
          ++stats().shader_dksh_hits;
          if(saved_binary)*saved_binary=std::move(bytes);
          return true;
        }
      }
    }
  }
  using E=rex::deko3d::Compilador::Etapa;
  E e=stage==ShaderStage::kVertex?E::kVertices:stage==ShaderStage::kGeometry?E::kGeometria:stage==ShaderStage::kCompute?E::kComputo:E::kFragmentos;
  const auto compile_start=std::chrono::steady_clock::now();
  auto binary=provider->compilador().Compilar(e,shader.info.source);
  const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-compile_start).count();
  ++stats().shader_compiles;stats().shader_compile_ns+=elapsed;
  if(stats().shader_compiles<=16||REXCVAR_GET(nx_native_trace_output)) {
    REXLOG_INFO("[deko-cache] UAM {} shader {:016X}: {:.1f} ms ({})",uint32_t(stage),key,double(elapsed)/1e6,warming_up?"warmup":"first use");
  }
  if(!binary.ok||!LoadShaderCode(shader,binary.dksh,"FEAR shader")) {
    REXLOG_ERROR("[deko] DKSH compile {:016X}: {}",key,binary.log);return false;
  }
  if(!path.empty()) {
    std::error_code ec;std::filesystem::create_directories(cache_root,ec);
    if(!ec) {
      auto temporary=path;temporary+=".tmp";
      std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
      Header header{key,binary.dksh.size(),XXH3_64bits(binary.dksh.data(),binary.dksh.size())};
      output.write(reinterpret_cast<const char*>(&header),sizeof(header));
      output.write(reinterpret_cast<const char*>(binary.dksh.data()),binary.dksh.size());
      output.flush();bool saved=bool(output);output.close();
      if(saved)std::filesystem::rename(temporary,path,ec);
      if(!saved||ec){++stats().shader_cache_write_failures;if(stats().shader_cache_write_failures<=8)REXLOG_WARN("[deko-cache] cannot save DKSH cache {}",path.filename().string());}
    } else {
      ++stats().shader_cache_write_failures;
      if(stats().shader_cache_write_failures<=8)REXLOG_WARN("[deko-cache] cannot create cache directory: {}",ec.message());
    }
  }
  if(saved_binary)*saved_binary=std::move(binary.dksh);
  return true;
}

bool Renderer::Impl::EnsureTranslated(SpirvShader::SpirvTranslation* t,bool* pending) {
  *pending=false;
  if(!t->is_translated()) {
    const auto start=std::chrono::steady_clock::now();
    const bool ok=translator->TranslateAnalyzedShader(*t);
    stats().shader_translate_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
    if(!ok||!t->is_valid())return false;
    ++stats().shaders_translated;
  }
  if(t->is_valid())RecordVariant(*t);
  return t->is_valid();
}

void Renderer::Impl::RecordVariant(const SpirvShader::SpirvTranslation& translation) {
  if(warmup_root.empty()||warming_up)return;
  const auto& shader=translation.shader();
  const auto identity=std::make_pair(shader.ucode_data_hash(),translation.modification());
  if(recorded_variants.contains(identity))return;
  if(std::chrono::steady_clock::now()<variant_retry_time)return;
  shader_warmup::Entry entry;
  entry.shader_type=uint32_t(shader.type());entry.shader_hash=shader.ucode_data_hash();
  entry.modification=translation.modification();entry.ucode=shader.ucode_data();
  std::string error;
  if(!shader_warmup::Save(warmup_root/shader_warmup::FileName(entry),entry,error)) {
    ++stats().shader_cache_write_failures;
    variant_retry_time=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    if(stats().shader_cache_write_failures<=8)REXLOG_WARN("[deko-cache] cannot record warmup variant: {}",error);
  } else recorded_variants.insert(identity);
}
CompiledShader* Renderer::Impl::CompileGeometry(GeometryShaderKey key) {
  auto words=CreateGeometryShaderSpirv(key);return Compile(words,ShaderStage::kGeometry);
}
Pipeline* Renderer::Impl::GetPipeline(const PipelineKey& key,SpirvShader::SpirvTranslation* vs,SpirvShader::SpirvTranslation* ps,CompiledShader* gs,uint64_t,bool* pending) {
  *pending=false;uint64_t hash=XXH3_64bits(&key,sizeof(key));
  if(auto it=pipelines.find(hash);it!=pipelines.end())return it->second.get();
  auto compile=[&](SpirvShader::SpirvTranslation* t,ShaderStage s){auto& b=t->translated_binary();return Compile({reinterpret_cast<const uint32_t*>(b.data()),b.size()/4},s);};
  auto p=std::make_unique<Pipeline>();p->key=key;p->gs=gs;
  p->vs=compile(vs,ShaderStage::kVertex);if(ps)p->ps=compile(ps,ShaderStage::kFragment);
  if(!p->vs||(ps&&!p->ps))return nullptr;
  auto* ptr=p.get();pipelines.emplace(hash,std::move(p));++stats().pipelines_created;return ptr;
}

#include "deko_image_ops.inc"
#include "deko_frontbuffer.inc"

RenderTarget* Renderer::Impl::GetRenderTarget(const RenderTargetKey& requested,uint32_t height) {
  if(!requested.pitch||!height)return nullptr;Begin();
  RenderTargetKey key=requested;
  VkFormat format=key.is_depth?(key.format==uint32_t(xenos::DepthRenderTargetFormat::kD24S8)&&REXCVAR_GET(nx_native_depth24)?VK_FORMAT_D24_UNORM_S8_UINT:VK_FORMAT_D32_SFLOAT_S8_UINT):ColorFormatToVk(xenos::ColorRenderTargetFormat(key.format));
  if(format==VK_FORMAT_UNDEFINED)return nullptr;
  // Identical storage aliases preserve the same contents. Guest format remains
  // in registers for gamma, exponent bias and shader-sign specialization.
  if(!key.is_depth)key.format=uint32_t(format);
  height=std::min(rex::align(height,32u),2048u);
  auto found=render_targets.find(key);
  if(found!=render_targets.end()){found->second->last_frame=frame;found->second->guest_format=requested.format;if(found->second->height>=height)return found->second.get();}
  uint32_t flags=ColorImageFlags(NativeFormat(format),color_compression_enabled&&!key.is_depth);
  auto image=textures->CreateImage(NativeFormat(format),key.pitch,height,DkImageType_2D,1,1,flags);
  if(!image){retry=textures->NeedsFlushRetry();return nullptr;}
  const float zero[4]{};VkRect2D whole{{0,0},{key.pitch,height}};
  Clear(*image,bool(key.is_depth),0,0,0xFF,zero,whole);
  if(found!=render_targets.end()) {
    BeforeImageWrite(found->second->image);
    Copy(found->second->image,*image,0,0,0,0,key.pitch,found->second->height);
    textures->FreeImage(found->second->image);
    found->second->image=std::move(*image);found->second->height=height;found->second->guest_format=requested.format;++rt_generation;
    return found->second.get();
  }
  auto rt=std::make_unique<RenderTarget>();rt->key=key;rt->image=std::move(*image);rt->format=format;rt->width=key.pitch;rt->height=height;rt->guest_format=requested.format;
  rt->last_frame=frame;auto* result=rt.get();render_targets.emplace(key,std::move(rt));++rt_generation;++stats().render_targets_created;return result;
}

void Renderer::Impl::TrimCaches() {
  if(last_trim_frame==frame)return;last_trim_frame=frame;
  auto stale=[&](uint64_t used){return frame%60==0&&frame>used&&frame-used>600;};
  if(frontbuffer.source && (stale(frontbuffer.source->last_frame)||stale(frontbuffer.destination->last_frame)))
    MaterializeFrontbuffer();
  for(auto it=render_targets.begin();it!=render_targets.end();) {
    if(stale(it->second->last_frame)){textures->FreeImage(it->second->image);it=render_targets.erase(it);++rt_generation;}
    else ++it;
  }
  for(auto it=resolves.begin();it!=resolves.end();) {
    if(stale(it->second->last_frame)){textures->FreeImage(it->second->image);it=resolves.erase(it);}
    else ++it;
  }
  for(auto it=resolve_aliases.begin();it!=resolve_aliases.end();) {
    if(!resolves.contains(it->second[0]))it=resolve_aliases.erase(it);else ++it;
  }
  for(auto it=crops.begin();it!=crops.end();) {
    if(stale(it->second.last_frame)||!resolves.contains(it->first[0])){textures->FreeImage(it->second.image);it=crops.erase(it);}
    else ++it;
  }
  // Match the baseline's bounded sized views. A frame may temporarily need
  // more variants; retire only old-frame crops so prepared draw resources stay
  // live until their consuming work has been recorded.
  uint64_t bytes=0;std::map<uint32_t,uint32_t> per_root;
  std::vector<std::pair<uint64_t,std::array<uint32_t,5>>> candidates;
  for(const auto& [key,crop]:crops) {
    bytes+=crop.image.allocation.bytes;++per_root[key[0]];
    if(crop.last_frame<frame)candidates.emplace_back(crop.last_frame,key);
  }
  std::sort(candidates.begin(),candidates.end());
  for(const auto& [used,key]:candidates) {
    if(bytes<=(128u<<20)&&per_root[key[0]]<=8)continue;
    auto it=crops.find(key);bytes-=it->second.image.allocation.bytes;--per_root[key[0]];
    textures->FreeImage(it->second.image);crops.erase(it);
  }
}

#include "deko_system_constants.inc"
#include "deko_draw_setup.inc"
#include "deko_clear_recognition.inc"
#include "deko_resolve.inc"

// Draw/resolve/presentation implementations below keep allocations in one work
// slot until the draw has been recorded. No allocator rotates the slot midway.

bool Renderer::Impl::DrawOnce(const uint32_t* immediate,uint32_t immediate_words) {
  ProfileScope phase(profile.draw_begin_ns,cpu_profile_enabled);
  Begin();retry=false;textures->ClearFlushRetry();
  const RegisterFile& r=*regs;auto initiator=r.Get<reg::VGT_DRAW_INITIATOR>();
  uint32_t count=initiator.num_indices;if(!count)return true;
  phase.Next(profile.setup_ns);
  DrawSetupIdentity identity{owner->state_generation_,rt_generation,uintptr_t(active_vs),uintptr_t(active_ps),
      initiator.value&0xFFFFu,REXCVAR_GET(nx_native_depth24),REXCVAR_GET(half_pixel_offset)};
  bool setup_hit=REXCVAR_GET(nx_native_state_cache) && setup_cached && identity==setup_identity;
  // Vertex fetches change constantly but cannot affect the pipeline. Only the
  // signedness of textures used by these shaders participates in its key.
  if(setup_hit && specialize_signs && setup.fetch_generation!=owner->fetch_generation_) {
    std::array<uint32_t,8> signs{};
    for(const auto* shader:{setup.vs,setup.ps})if(shader)
      for(const auto& binding:shader->texture_bindings()) {
        const uint32_t fetch=binding.fetch_constant,shift=8*(fetch&3);
        signs[fetch>>2]|=uint32_t(rex::graphics::texture_util::SwizzleSigns(r.GetTextureFetch(fetch)))<<shift;
      }
    setup_hit=std::equal(signs.begin(),signs.end(),std::begin(setup.pipeline->key.texture_signs));
    if(!setup_hit)++profile.setup_sign_misses;
  }
  if(setup_hit) {
    ++profile.setup_hits;setup.fetch_generation=owner->fetch_generation_;
    // GetRenderTarget normally refreshes these even when no new image is made.
    for(uint32_t i=0;i<4;++i)if(auto* rt=setup.targets.color[i]) {
      rt->last_frame=frame;
      rt->guest_format=uint32_t(r.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]).color_format);
    }
    if(setup.targets.depth)setup.targets.depth->last_frame=frame;
  } else {
    ++profile.setup_misses;++stats().setups_built;
    if(identity.state!=setup_identity.state)++profile.setup_state_misses;
    if(identity.targets!=setup_identity.targets)++profile.setup_target_misses;
    if(identity.vs!=setup_identity.vs || identity.ps!=setup_identity.ps)++profile.setup_shader_misses;
    setup_cached=BuildDrawSetup(initiator.value);
    identity.targets=rt_generation;setup_identity=identity;
  }
  if(!setup_cached) {
    if(retry)return false;
    static uint32_t messages[16]{};
    uint32_t reason=setup.skip_reason&15;
    if(reason==kDiscardedVisibilityProbeSkipReason) {
      ++stats().visibility_probes_discarded;
    } else if(reason>=4&&messages[reason]++<4) {
      REXLOG_WARN("[deko] draw setup rejected (reason={})",reason);
    }
    ++stats().draws_skipped;++stats().skipped_by_reason[setup.skip_reason&15];return true;
  }
  phase.Next(profile.index_ns);
  auto& st=setup;auto& key=st.pipeline->key;
  auto alloc=[&](uint32_t bytes,uint32_t alignment){
    auto a=core.Reservar(bytes,alignment);if(!a)retry=true;return a;
  };
  bool indexed=initiator.source_select!=xenos::SourceSelect::kAutoIndex;
  bool index32=initiator.index_size==xenos::IndexFormat::kInt32;
  auto endian=r.Get<reg::VGT_DMA_SIZE>().swap_mode;
  const uint8_t* indices=nullptr;
  if(indexed) {
    if(initiator.source_select==xenos::SourceSelect::kDMA) {
      uint32_t address=r[rex::graphics::XE_GPU_REG_VGT_DMA_BASE]&0x1FFFFFFF;
      uint64_t bytes=uint64_t(count)*(index32?4:2);
      if(uint64_t(address)+bytes>0x20000000ULL)return false;
      indices=memory->TranslatePhysical<const uint8_t*>(address);
    } else if(immediate) {
      indices=reinterpret_cast<const uint8_t*>(immediate);endian=xenos::Endian::kNone;
      count=std::min(count,immediate_words*(index32?1u:2u));
    } else return false;
  }
  auto read_index=[&](uint32_t i)->uint32_t {
    if(!indexed)return i;
    if(index32){uint32_t v;memcpy(&v,indices+i*4,4);return xenos::GpuSwap(v,endian);}
    uint16_t v;memcpy(&v,indices+i*2,2);return xenos::GpuSwap(v,endian);
  };
  uint32_t draw_count=count;
  const uint32_t index_base=r[rex::graphics::XE_GPU_REG_VGT_INDX_OFFSET];
  const uint32_t index_min=r[rex::graphics::XE_GPU_REG_VGT_MIN_VTX_INDX];
  const uint32_t index_max=r[rex::graphics::XE_GPU_REG_VGT_MAX_VTX_INDX];
  IndexBounds vertex_bounds;
  bool host_indexed=indexed||st.convert_fan||st.convert_loop;
  rex::deko3d::Subida index_data{};
  bool use_index16=false,index_compact_output=false;
  if(host_indexed) {
    IndexUploadDescription description;
    description.count=count;description.base=index_base;
    description.minimum=index_min;description.maximum=index_max;
    description.restart=r.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
    description.endian=uint32_t(endian);description.generated=!indexed;description.index32=index32;
    description.fan=st.convert_fan;description.loop=st.convert_loop;description.reset=st.reset_enabled;
    description.output16=use_index16=REXCVAR_GET(nx_native_index16)&&CanUseIndex16(description);
    description.compact16=REXCVAR_GET(nx_native_index16)&&REXCVAR_GET(nx_native_index16_compact)&&!description.output16&&!description.reset;
    const uint32_t index_bytes=use_index16?2:4;
    const uint64_t converted_count=IndexUploadCount(description);
    if(!converted_count)return true;
    if(converted_count*index_bytes>kUploadBytes)return false;
    draw_count=uint32_t(converted_count);
    uint32_t bytes=draw_count*index_bytes;
    const std::span<const uint8_t> source(indices,indexed?size_t(count)*(index32?4:2):0);
    std::array<uint64_t,4> upload_key;
    const IndexCopy* cached=nullptr;
    const IndexConversion* cpu_bounds=nullptr;
    std::span<const uint8_t> captured=source,converted;
    bool validated=false;
    if(REXCVAR_GET(nx_native_index_validation_reuse)) {
      ProfileScope timer(profile.index_validation_probe_ns,cpu_profile_enabled);
      validated=index_validation.TryReuse(source,[&](uint64_t hash) {
        upload_key=IndexUploadKey(description,hash);
        cached=index_copies.Find(upload_key,source);
        if(!cached) {
          ProfileScope lookup(profile.index_cpu_lookup_ns,cpu_profile_enabled);
          cpu_bounds=converted_indices.Find(upload_key,source,&converted,&captured);
        }
        // Both probes compare every source byte against the cached snapshot.
        // A hint alone cannot retain stale bounds after a within-work write.
        return cached||cpu_bounds;
      });
    }
    if(!validated) {
      ProfileScope timer(profile.index_hash_ns,cpu_profile_enabled);
      upload_key=IndexUploadKey(description,source.empty()?0:XXH3_64bits(source.data(),source.size()));
      cached=index_copies.Find(upload_key,source);
      if(cpu_profile_enabled){profile.index_hash_bytes+=source.size();if(!source.empty())++profile.index_full_hashes;}
    }
    if(cached) {
      index_data=cached->upload;vertex_bounds=cached->bounds;use_index16=cached->output16;
      bytes=draw_count*(use_index16?2:4);
      if(cpu_profile_enabled){++profile.index_hits;profile.index_avoided_bytes+=bytes;}
    } else {
      if(cpu_profile_enabled)++profile.index_misses;
      uint64_t identity=0;
      if(!cpu_bounds) {
        ProfileScope timer(profile.index_cpu_lookup_ns,cpu_profile_enabled);
        cpu_bounds=converted_indices.Find(upload_key,source,&converted,&captured);
      }
      if(cpu_bounds) {
        vertex_bounds=cpu_bounds->bounds;identity=cpu_bounds->identity;
        use_index16=cpu_bounds->output16;bytes=draw_count*(use_index16?2:4);
        if(cpu_profile_enabled)++profile.index_cpu_hits;
      } else {
        if(cpu_profile_enabled)++profile.index_cpu_misses;
      if(!source.empty()) {
        ProfileScope timer(profile.index_hash_ns,cpu_profile_enabled);
        // Convert and remember the same snapshot, even if the guest changes
        // its source while the renderer is preparing this upload.
        index_source_staging.assign(source.begin(),source.end());
        captured=index_source_staging;indices=captured.data();
        upload_key=IndexUploadKey(description,XXH3_64bits(captured.data(),captured.size()));
        if(cpu_profile_enabled){profile.index_hash_bytes+=captured.size();++profile.index_full_hashes;}
      }
      {
        ProfileScope timer(profile.index_convert_ns,cpu_profile_enabled);
        if(use_index16) {
          index16_staging.resize(draw_count);
          if(!BuildIndexUpload16(description,read_index,index16_staging,vertex_bounds))return false;
          converted={reinterpret_cast<const uint8_t*>(index16_staging.data()),bytes};
        } else {
          index_staging.resize(draw_count);
          if(!BuildIndexUpload(description,read_index,index_staging,vertex_bounds))return false;
          if(description.compact16) {
            ProfileScope pack(profile.index_compact_ns,cpu_profile_enabled);
            index16_staging.resize(draw_count);
            use_index16=PackIndexUpload16(description,index_staging,index16_staging);
            if(cpu_profile_enabled){++profile.index_compact_attempts;if(use_index16)++profile.index_compact_accepted;else ++profile.index_compact_rejected;}
          }
          bytes=draw_count*(use_index16?2:4);
          converted={use_index16?reinterpret_cast<const uint8_t*>(index16_staging.data()):reinterpret_cast<const uint8_t*>(index_staging.data()),bytes};
        }
      }
        // Do not reuse an identity when CPU metadata is cleared or replaced.
        if(index_identity!=UINT64_MAX)identity=++index_identity;
      }
      PersistentVertexCache::Result resident;
      if(index_cache_storage&&identity) {
        ProfileScope timer(profile.index_gpu_lookup_ns,cpu_profile_enabled);
        resident=shared_geometry_enabled?shared_indices.FindOrAllocate(0,bytes,identity):index_gpu_cache.FindOrAllocate(0,bytes,identity);
      }
      bool batched=false;
      if(resident.valid)index_data={index_cache_storage.cpu+resident.offset,index_cache_storage.gpu+resident.offset};
      else {
        if(REXCVAR_GET(nx_native_small_index_batch)&&bytes<=SmallIndexUpload::kMaxPayloadBytes) {
          ProfileScope timer(profile.index_batch_capture_ns,cpu_profile_enabled);
          const auto staged=small_indices.Append(converted,
              [&](uint32_t chunk_bytes,uint32_t alignment)->SmallIndexUpload::Upload {
                // Avoid a false upload-overflow/retry when only the exact
                // payload fits. The ordinary allocation remains the fallback.
                const uint32_t used=kUploadBytes-core.SubidaLibre();
                if(uint64_t(rex::align(used,alignment))+chunk_bytes>kUploadBytes)return {};
                const auto a=core.Reservar(chunk_bytes,alignment);
                if(cpu_profile_enabled&&a)profile.index_batch_reserve_bytes+=chunk_bytes;
                return {a.cpu,a.gpu};
              },[&](auto* dst,const auto* src,uint32_t size){WriteSmallIndices(dst,src,size);});
          if(staged) {
            index_data={staged.cpu,staged.gpu};batched=true;
            if(cpu_profile_enabled){++profile.index_batch_payloads;profile.index_batch_payload_bytes+=bytes;}
          }
        }
        if(!index_data)index_data=alloc((bytes+15)&~15u,16);
      }
      if(!index_data)return false;
      if(!resident.hit) {
        if(resident.valid&&REXCVAR_GET(nx_native_index_resident_batch)) {
          ProfileScope timer(profile.index_resident_capture_ns,cpu_profile_enabled);
          // Shared cache owns 64-byte blocks; the legacy cache owns whole
          // pages. Both prove ownership of this rounded fresh range. Hits
          // never reach this path, and pending blocks remain GPU-fence pinned.
          auto write=[&](auto* dst,const auto* src,uint32_t size){WriteResidentIndices(dst,src,size);};
          batched=ordered_index_uploads?
              deferred_index_uploads.Append(index_data.cpu,index_data.gpu,converted,(bytes+63)&~63u,write):
              resident_index_uploads.Append(index_data.cpu,index_data.gpu,converted,(bytes+63)&~63u,write);
          if(cpu_profile_enabled&&batched){++profile.index_resident_payloads;profile.index_resident_payload_bytes+=bytes;}
        }
        if(!batched) {
          ProfileScope timer(profile.index_copy_ns,cpu_profile_enabled);
          ProfileScope kind(resident.valid?profile.index_direct_resident_ns:profile.index_direct_transient_ns,cpu_profile_enabled);
          const uint32_t written=WriteGeometryUpload(index_data.cpu,converted.data(),bytes,(bytes+15)&~15u,false);
          if(cpu_profile_enabled){++profile.index_direct_writes;profile.index_direct_bytes+=bytes;profile.index_direct_written_bytes+=written;
            profile.index_direct_unaligned_sources+=(reinterpret_cast<uintptr_t>(converted.data())&15)!=0;profile.index_direct_tails+=(bytes&15)!=0;}
        }
        // The invalidation is recorded before this draw, including for a
        // snapshot whose fresh destination will be written before submission.
        draw_barriers.CpuUpload();
      }
      if(cpu_profile_enabled) {
        if(resident.hit){++profile.index_gpu_hits;profile.index_gpu_avoided_bytes+=bytes;}
        else {++profile.index_gpu_misses;profile.index_copy_bytes+=bytes;if(use_index16)profile.index16_saved_bytes+=bytes;}
      }
      const bool remembered=index_copies.Insert(upload_key,captured,{index_data,vertex_bounds,use_index16});
      if(cpu_profile_enabled&&!remembered)++profile.index_uncached;
      if(!cpu_bounds&&captured.size()+converted.size()<=converted_indices.capacity_bytes()) {
        if(!converted_indices.Insert(upload_key,captured,{vertex_bounds,identity,use_index16},converted)) {
          // All live spans on a conversion miss belong to CPU staging. A
          // bounded reset cannot invalidate any submitted GPU allocation.
          converted_indices.Clear();
          if(cpu_profile_enabled)++profile.index_cpu_resets;
          converted_indices.Insert(upload_key,captured,{vertex_bounds,identity,use_index16},converted);
        }
      }
    }
    if(REXCVAR_GET(nx_native_index_validation_reuse))index_validation.Remember(source,upload_key[0]);
    index_compact_output=use_index16&&!description.output16;
  }
  else vertex_bounds=IndexBounds::AutoDraw(0,count,index_base,index_min,index_max);
  phase.Next(profile.vertex_ns);
  std::array<uint32_t,6*32> fetch_words;
  memcpy(fetch_words.data(),&r[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],sizeof(fetch_words));
  const auto& vmap=st.vs->constant_register_map();
  auto plan_it=vertex_range_plans.find(st.vs);
  if(plan_it==vertex_range_plans.end())
    plan_it=vertex_range_plans.emplace(st.vs,BuildVertexRangePlan(*st.vs)).first;
  const auto& range_plan=plan_it->second;
  const bool normal_vertex_shader=SpirvShaderTranslator::Modification(key.vs_modification)
      .vertex.host_vertex_shader_type==Shader::HostVertexShaderType::kVertex;
  // Stable data retains its content-keyed persistent windows. Known-changing
  // data uses exact proven windows and immutable local copies. A separate
  // NFSMW-style epoch table avoids repeated validation/upload until the next
  // guest synchronization, frame, work-slot change or guard invalidation.
  for(uint32_t word=0;word<3;++word) {
    uint32_t bits=vmap.vertex_fetch_bitmap[word],bit;
    while(rex::bit_scan_forward(bits,&bit)) {
      bits&=~(1u<<bit);uint32_t fi=word*32+bit;
      uint32_t d0=fetch_words[fi*2],d1=fetch_words[fi*2+1];
      uint32_t address=d0&0x1FFFFFFC,bytes=((d1>>2)&0xFFFFFF)*4;
      // Reject invalid descriptors before selecting a proven fetch window.
      if(!bytes||bytes>kUploadBytes||uint64_t(address)+bytes>0x20000000ULL)return false;
      if(cpu_profile_enabled)profile.vertex_declared_bytes+=bytes;
      const uint32_t descriptor_address=address,descriptor_bytes=bytes,endian=d1&3;
      VertexByteRange range{};
      VertexWindow window{0,bytes};
      uint32_t grid_saved=0;
      if(normal_vertex_shader&&range_plan[fi].valid) {
        range=UsedVertexPrefix(bytes,vertex_bounds,range_plan[fi].accesses);
        if(range.valid) {
          const auto legacy=SelectVertexWindowLegacy(bytes,range);
          window=REXCVAR_GET(nx_native_vertex_grid_windows)?SelectVertexWindow(bytes,range):legacy;
          grid_saved=legacy.bytes-window.bytes;
          if(cpu_profile_enabled&&grid_saved){++profile.vertex_grid_reduced;profile.vertex_grid_selected_saved+=grid_saved;}
          if(cpu_profile_enabled&&window.bytes<bytes)++profile.vertex_clipped;
        } else if(cpu_profile_enabled)++profile.vertex_range_fallbacks;
      } else if(cpu_profile_enabled)++profile.vertex_range_fallbacks;
      const VertexWindow stable_window=window;
      bool dynamic_window=REXCVAR_GET(nx_native_vertex_dynamic_windows) && range.valid &&
          validation_cache.GeometryNeedsExactWindow(address+window.first_byte,window.bytes);
      if(dynamic_window)window=SelectDynamicVertexWindow(descriptor_bytes,range);
      address+=window.first_byte;bytes=window.bytes;
      const bool epoch_reuse=REXCVAR_GET(nx_native_vertex_epoch_reuse) && validation_cache.enabled() &&
          !validation_cache.stats().sampling_disabled;
      const auto sync_vertex_epoch=[&] {
        vertex_epoch_copies.BeginEpoch(work,frame,observed_vertex_reuse_epoch,
            validation_cache.memo_epoch(),validation_cache.stats().guard_misses);
      };
      const auto record_window=[&] {
        if(!cpu_profile_enabled)return;
        profile.vertex_window_bytes+=bytes;
        if(dynamic_window){++profile.vertex_dynamic_windows;profile.vertex_dynamic_window_saved+=stable_window.bytes-bytes;}
      };
      if(epoch_reuse) {
        ProfileScope lookup(profile.vertex_epoch_lookup_ns,cpu_profile_enabled);
        sync_vertex_epoch();
        if(const auto* same=vertex_epoch_copies.Find(address,bytes,endian,stream_base+window.first_byte,
            validation_cache.guest_writes())) {
          uint32_t fetch_base;
          if(!RebaseVertexWindow(window,same->gpu-stream_base,stream_bytes,fetch_base))return false;
          fetch_words[fi*2]=(d0&3)|fetch_base;
          record_window();
          if(cpu_profile_enabled) {
            ++profile.vertex_local_hits;
            if(dynamic_window){++profile.vertex_epoch_dynamic_hits;profile.vertex_epoch_dynamic_bytes+=bytes;}
          }
          continue;
        }
      }
      const uint8_t* data=memory->TranslatePhysical<const uint8_t*>(address);
      ValidationCache::Result checked;
      {
        ProfileScope hash_timer(profile.vertex_hash_ns,cpu_profile_enabled);
        checked=dynamic_window?validation_cache.HashDynamicGeometry(data,bytes):validation_cache.Hash(data,address,bytes);
        if(cpu_profile_enabled){profile.vertex_hash_bytes+=checked.bytes_hashed;++profile.vertex_hash_calls;}
      }
      if(checked.guard_miss&&validation_cache.stats().guard_misses<=16)
        REXLOG_WARN("[deko-validation] local guard miss: geometry address={:08X} bytes={} frame={}; overlapping ranges now use full checks at geometry epoch boundaries; global sampling disabled={}",
            address,bytes,frame,validation_cache.stats().sampling_disabled);
      // The current validation may have just discovered volatility. Narrow
      // this very first miss too, without admitting a transient slice under
      // the stable window's different hash identity.
      if(!dynamic_window && REXCVAR_GET(nx_native_vertex_dynamic_windows) && range.valid &&
         (checked.volatile_geometry || checked.guard_miss)) {
        dynamic_window=true;window=SelectDynamicVertexWindow(descriptor_bytes,range);
        address=descriptor_address+window.first_byte;bytes=window.bytes;
        data=memory->TranslatePhysical<const uint8_t*>(address);
        ProfileScope hash_timer(profile.vertex_hash_ns,cpu_profile_enabled);
        checked=validation_cache.HashDynamicGeometry(data,bytes);
        if(cpu_profile_enabled){profile.vertex_hash_bytes+=checked.bytes_hashed;++profile.vertex_hash_calls;}
      }
      record_window();
      std::array<uint64_t,3> vk{address,bytes,checked.hash};
      const bool can_reuse=!checked.unhashed_geometry && (!checked.bypass_cache||checked.volatile_geometry);
      const auto* local=can_reuse?vertex_copies.Find(vk):nullptr;
      if(local && local->gpu-stream_base<window.first_byte)local=nullptr;
      rex::deko3d::Subida a{};
      if(local) {
        a=*local;
        if(cpu_profile_enabled)++profile.vertex_local_hits;
        if(cpu_profile_enabled&&checked.volatile_geometry) {
          ++profile.dynamic_reuse_hits;profile.dynamic_reuse_bytes+=bytes;
        }
      }
      else {
        if(cpu_profile_enabled)++profile.vertex_local_misses;
        PersistentVertexCache::Result cached;
        if(!checked.bypass_cache&&vertex_cache_storage) {
          ProfileScope cache_timer(profile.vertex_cache_ns,cpu_profile_enabled);
          cached=shared_geometry_enabled?shared_vertices.FindOrAllocate(address,bytes,checked.hash,window.first_byte):
              vertex_cache.FindOrAllocate(address,bytes,checked.hash,window.first_byte);
        }
        bool batched=false;
        if(cached.valid)a={vertex_cache_storage.cpu+cached.offset,stream_base+cached.offset};
        else {
          if(REXCVAR_GET(nx_native_small_vertex_batch)&&bytes<=SmallVertexUpload::kMaxPayloadBytes) {
            ProfileScope capture(profile.vertex_batch_capture_ns,cpu_profile_enabled);
            const auto staged=small_vertices.Append({data,bytes},
                [&](uint32_t chunk_bytes,uint32_t alignment)->SmallVertexUpload::Upload {
                  const uint32_t used=rex::align(kUploadBytes-core.SubidaLibre(),alignment);
                  const uint32_t at=std::max(used,window.first_byte>slot_stream_offset?window.first_byte-slot_stream_offset:0u);
                  if(uint64_t(at)+chunk_bytes>kUploadBytes)return {};
                  if(at>used) {
                    if(!core.Reservar(at-used,alignment))return {};
                    if(cpu_profile_enabled)profile.vertex_padding_bytes+=at-used;
                  }
                  const auto upload=core.Reservar(chunk_bytes,alignment);
                  if(cpu_profile_enabled&&upload)profile.vertex_batch_reserved+=chunk_bytes;
                  return {upload.cpu,upload.gpu};
                },[&](auto* dst,const auto* src,uint32_t size){WriteSmallVertices(dst,src,size);},stream_base+window.first_byte);
            if(staged) {
              a={staged.cpu,staged.gpu};batched=true;
              if(cpu_profile_enabled){++profile.vertex_batch_payloads;profile.vertex_batch_bytes+=bytes;}
            }
          }
          if(!a) {
            // Leave address space before a high-index slice, without copying
            // its unused bytes. The fetch base must remain nonnegative.
            const uint32_t used=slot_stream_offset+rex::align(kUploadBytes-core.SubidaLibre(),4u);
            if(used<window.first_byte) {
              const uint32_t padding=window.first_byte-used;
              if(!alloc(padding,4))return false;
              if(cpu_profile_enabled)profile.vertex_padding_bytes+=padding;
            }
            a=alloc((bytes+15)&~15u,16);
          }
        }
        if(!a)return false;
        if(!cached.hit) {
          if(cached.valid&&REXCVAR_GET(nx_native_vertex_resident_batch)&&bytes<=ResidentVertexUploadBatch::kMaxPayloadBytes) {
            ProfileScope capture(profile.vertex_resident_capture_ns,cpu_profile_enabled);
            // Fresh shared blocks own round64(bytes); legacy entries own
            // whole pages. No existing hit or in-flight allocation is written.
            batched=resident_vertex_uploads.Append(a.cpu,a.gpu,{data,bytes},(bytes+63)&~63u,
                [&](auto* dst,const auto* src,uint32_t size){WriteResidentVertices(dst,src,size);});
            if(cpu_profile_enabled&&batched){++profile.vertex_resident_payloads;profile.vertex_resident_payload_bytes+=bytes;}
          }
          if(!batched)WriteDirectVertices(a.cpu,data,bytes,cached.valid);
          draw_barriers.CpuUpload();stats().vertex_bytes_copied+=bytes;
          if(cpu_profile_enabled)profile.vertex_grid_copy_saved+=grid_saved;
          if(cpu_profile_enabled&&dynamic_window)profile.vertex_dynamic_copy_saved+=stable_window.bytes-bytes;
          if(cpu_profile_enabled&&checked.volatile_geometry)profile.dynamic_copy_bytes+=bytes;
          ProbeVertexWrite(data,bytes);
        }
        // Metadata may be dropped without reclaiming any in-flight GPU bytes.
        if(can_reuse&&vertex_copies.size()<8192)vertex_copies.Store(vk,a);
      }
      // Independent of persistent admission/bypass: volatile uploads are
      // reusable inside the current ownership epoch, including unhashed ones.
      if(epoch_reuse && !validation_cache.stats().sampling_disabled) {
        sync_vertex_epoch();vertex_epoch_copies.Store(address,bytes,endian,a,validation_cache.guest_writes());
      }
      uint32_t fetch_base;
      if(!RebaseVertexWindow(window,a.gpu-stream_base,stream_bytes,fetch_base))return false;
      fetch_words[fi*2]=(d0&3)|fetch_base;
    }
  }
  phase.Next(profile.constants_ns);
  DkBufExtents uniforms[SpirvShaderTranslator::kConstantBufferCount]{};
  auto alloc_constant=[&](uint32_t bytes)->rex::deko3d::Subida {
    if(!constant_memory)return alloc(bytes,256);
    // Keep even transient constants out of the vertex SSBO. Otherwise Eden
    // synchronizes their dirty pages as geometry at every following draw,
    // breaking render passes and inserting transfer barriers for unused data.
    const uint32_t offset=rex::align(constant_upload_used,256u);
    if(bytes>kConstantUploadSlotBytes-offset) {
      ++constant_upload_overflows;retry=true;return {};
    }
    constant_upload_used=offset+bytes;
    constant_upload_peak=std::max(constant_upload_peak,constant_upload_used);
    return {constant_slot_base.cpu+offset,constant_slot_base.gpu+offset};
  };
  auto upload_constant=[&](const void* data,uint32_t bytes,const uint64_t* prepared_hash=nullptr)->rex::deko3d::Subida {
    const uint32_t allocated=rex::align(bytes,256u);
    const std::span<const uint8_t> source(static_cast<const uint8_t*>(data),bytes);
    std::array<uint64_t,2> upload_key;
    {
      ProfileScope timer(profile.constant_lookup_ns,cpu_profile_enabled);
      upload_key={prepared_hash?*prepared_hash:XXH3_64bits(data,bytes),bytes};
      if(const auto* cached=constant_copies.Find(upload_key,source)) {
        if(cpu_profile_enabled){++profile.constant_hits;profile.constant_avoided_bytes+=allocated;}
        return *cached;
      }
    }
    if(cpu_profile_enabled)++profile.constant_misses;
    auto a=alloc_constant(allocated);if(!a)return {};
    {
      ProfileScope timer(profile.constant_copy_ns,cpu_profile_enabled);
      ProfileScope initial_timer(profile.constant_initial_ns,cpu_profile_enabled);
      memcpy(a.cpu,data,bytes);draw_barriers.CpuUpload();
      if(allocated>bytes)memset(a.cpu+bytes,0,allocated-bytes);
    }
    const bool remembered=constant_copies.Insert(upload_key,source,a);
    if(cpu_profile_enabled){profile.constant_copy_bytes+=allocated;if(!remembered)++profile.constant_uncached;}
    return a;
  };
  const bool delta_constants=REXCVAR_GET(nx_native_delta_constants);
  auto update_constant=[&](DeltaConstant& state,const void* data,uint32_t bytes,
                          const uint64_t* prepared_hash=nullptr,uint64_t content_revision=0,
                          const ConstantPackingPlan* packing=nullptr)->DkBufExtents {
    if(!delta_constants) {
      auto a=upload_constant(data,bytes,prepared_hash);
      return a?DkBufExtents{a.gpu,rex::align(bytes,16u)}:DkBufExtents{};
    }
    const std::span<const uint8_t> source(static_cast<const uint8_t*>(data),bytes);
    const uint32_t allocated=rex::align(bytes,256u);
    ConstantBufferState* selected=&state;
    rex::deko3d::Subida resident_upload{};
    uint64_t shadow_epoch=work;
    bool reentered=false;
    if(constant_arena.active()) {
      auto& resident=state.slots[constant_arena.slot()];
      if(!constant_arena.Matches(resident.lease,allocated) || resident.logical_bytes!=bytes) {
        resident.lease=constant_arena.Allocate(allocated);resident.logical_bytes=bytes;
      }
      if(resident.lease) {
        selected=&resident;shadow_epoch=resident.lease.epoch;
        resident_upload={constant_slot_base.cpu+resident.lease.offset,constant_slot_base.gpu+resident.lease.offset};
        reentered=resident.last_work!=work && resident.shadow.Ready(shadow_epoch,bytes);
        resident.last_work=work;
        if(cpu_profile_enabled && reentered) {
          ++profile.constant_slot_reuses;profile.constant_slot_initial_avoided+=allocated;
        }
      } else if(cpu_profile_enabled)++profile.constant_slot_fallbacks;
    }
    auto& current=*selected;
    // Reentering the same fenced slot with the same packed content needs no
    // second byte scan. Lease, epoch and logical extent are validated above;
    // only a revision from this state's own packing plan may use this path.
    const bool shadow_ready=current.shadow.Ready(shadow_epoch,bytes);
    if(shadow_ready && content_revision && current.content_revision==content_revision) {
      if(cpu_profile_enabled) {
        ++profile.constant_hits;++profile.constant_revision_hits;
        profile.constant_avoided_bytes+=allocated;
        if(reentered)++profile.constant_slot_unchanged;
      }
      return current.buffer;
    }
    if(!shadow_ready) {
      auto a=resident_upload?resident_upload:alloc_constant(allocated);if(!a)return {};
      if(!current.shadow.Initialize(shadow_epoch,source))return {};
      ProfileScope timer(profile.constant_copy_ns,cpu_profile_enabled);
      ProfileScope initial_timer(profile.constant_initial_ns,cpu_profile_enabled);
      memcpy(a.cpu,current.shadow.bytes().data(),allocated);draw_barriers.CpuUpload();
      current.buffer={a.gpu,rex::align(bytes,16u)};
      if(cpu_profile_enabled && resident_upload)++profile.constant_slot_allocations;
      if(cpu_profile_enabled){++profile.constant_misses;profile.constant_copy_bytes+=allocated;profile.constant_initial_bytes+=allocated;}
    } else {
      // PushConstants records ordered Maxwell constant-buffer loads. CPU
      // memcpy to an existing buffer would corrupt earlier queued draws.
      ProfileScope timer(profile.constant_copy_ns,cpu_profile_enabled);
      const uint64_t scan_start=cpu_profile_enabled?ProfileClockNs():0,push_before=profile.constant_push_ns;
      const auto dirty=packing && REXCVAR_GET(nx_native_constant_dirty_ranges)?
          packing->dirty_since(current.content_revision):ConstantDirtyRange{0,source.size()};
      if(cpu_profile_enabled) {
        profile.constant_scan_extent_bytes+=dirty.end-dirty.begin;
        profile.constant_scan_skipped_bytes+=source.size()-(dirty.end-dirty.begin);
        if(dirty.end-dirty.begin<source.size())++profile.constant_narrow_scans;
      }
      const auto changed=current.shadow.UpdateRange(source,dirty,[&](uint32_t offset,std::span<const uint8_t> range){
        ProfileScope push_timer(profile.constant_push_ns,cpu_profile_enabled);
        dkCmdBufPushConstants(core.comandos(),current.buffer.addr,allocated,offset,uint32_t(range.size()),range.data());
      });
      if(cpu_profile_enabled) {
        profile.constant_scan_ns+=ProfileClockNs()-scan_start-(profile.constant_push_ns-push_before);
        profile.constant_push_bytes+=changed.bytes;profile.constant_push_commands+=changed.commands;
        profile.constant_avoided_bytes+=allocated-changed.bytes;
        if(changed.commands)++profile.constant_misses;
        else {++profile.constant_hits;++profile.constant_delta_unchanged;}
        if(reentered) {
          profile.constant_slot_reentry_push_bytes+=changed.bytes;
          if(!changed.commands)++profile.constant_slot_unchanged;
        }
      }
    }
    current.content_revision=content_revision;
    return current.buffer;
  };
  auto constant=[&](uint32_t slot,DeltaConstant& state,const void* data,uint32_t bytes,
                    const uint64_t* prepared_hash=nullptr,uint64_t content_revision=0,
                    const ConstantPackingPlan* packing=nullptr){
    auto a=update_constant(state,data,bytes,prepared_hash,content_revision,packing);if(!a.size)return false;
    uniforms[slot]=a;return true;
  };
  auto generation_hit=[&](DkBufExtents data) {
    if(cpu_profile_enabled){++profile.constant_hits;++profile.constant_generation_hits;
      profile.constant_avoided_bytes+=rex::align(data.size,256u);}
  };
  auto prepared_constant_hash=[&](ConstantState& state)->const uint64_t* {
    // Ordered delta buffers compare exact CPU bytes and never consult this
    // content hash. Compute it lazily only for the alternate upload cache,
    // retaining its revision so a runtime option change cannot use a stale hash.
    if(delta_constants)return nullptr;
    if(state.hash_revision!=state.plan.revision()) {
      ProfileScope timer(profile.constant_pack_ns,cpu_profile_enabled);
      const auto packed=state.plan.bytes();
      state.hash=XXH3_64bits(packed.data(),packed.size());state.hash_revision=state.plan.revision();
      if(cpu_profile_enabled){++profile.constant_content_hashes;profile.constant_content_hash_bytes+=packed.size();}
    }
    return &state.hash;
  };
  auto floats=[&](const Shader* shader,uint32_t base,uint32_t slot,uint64_t generation){
    auto& pointer=float_constants[shader];
    if(!pointer) {
      const auto& map=shader->constant_register_map();
      std::vector<ConstantCopyRange> ranges;
      uint32_t at=0;
      for(uint32_t w=0;w<4;++w){uint64_t bits=map.float_bitmap[w];uint32_t bit;
        while(rex::bit_scan_forward(bits,&bit)){bits&=~(uint64_t(1)<<bit);
          ranges.push_back({size_t(w*64+bit)*16,at,16});at+=16;}}
      if(ranges.empty())ranges.push_back({0,0,0});
      pointer=constant_layouts.Intern(base,shared_constants_enabled?0:uintptr_t(shader),4096,
          std::max(map.float_count,1u)*16u,ranges);
      if(!pointer)return false;
    }
    auto& state=*pointer;
    if(state.generation==generation)if(const auto* cached=state.upload.Find(state.plan.revision(),work)) {
      uniforms[slot]=*cached;generation_hit(*cached);return true;
    }
    {
      ProfileScope timer(profile.constant_pack_ns,cpu_profile_enabled);
      if(state.generation!=generation||!state.plan.revision()) {
        if(cpu_profile_enabled){profile.constant_pack_bank_bytes+=4096;profile.constant_pack_used_bytes+=state.plan.source_bytes_used();}
        const auto checked=state.plan.version_checks(),skipped=state.plan.version_skips();
        const auto copied=state.plan.packed_copy_bytes(),partial=state.plan.partial_packs(),bulk=state.plan.version_bulk_updates();
        const auto groups=state.plan.version_group_checks(),groups_skipped=state.plan.version_group_skips();
        static_assert(ConstantPackingPlan::kVersionGroupRegisters==8);
        if(state.plan.UpdateVersioned({reinterpret_cast<const uint8_t*>(&r[base]),4096},
            {owner->float_register_versions_+(base-0x4000)/4,256},
            {owner->float_register_group_versions_+(base-0x4000)/32,32})) {
          if(cpu_profile_enabled)++profile.constant_packs;
        }
        profile.constant_version_checks+=state.plan.version_checks()-checked;
        profile.constant_version_skips+=state.plan.version_skips()-skipped;
        profile.constant_packed_copy_bytes+=state.plan.packed_copy_bytes()-copied;
        profile.constant_partial_packs+=state.plan.partial_packs()-partial;
        profile.constant_bulk_updates+=state.plan.version_bulk_updates()-bulk;
        profile.constant_group_checks+=state.plan.version_group_checks()-groups;
        profile.constant_group_skips+=state.plan.version_group_skips()-groups_skipped;
        state.generation=generation;
      }
    }
    if(const auto* cached=state.upload.Find(state.plan.revision(),work)) {
      uniforms[slot]=*cached;generation_hit(*cached);return true;
    }
    const auto packed=state.plan.bytes();
    if(!constant(slot,state.delta,packed.data(),uint32_t(packed.size()),prepared_constant_hash(state),state.plan.revision(),&state.plan))return false;
    state.upload.Store(state.plan.revision(),work,uniforms[slot]);return true;
  };
  if(!floats(st.vs,rex::graphics::XE_GPU_REG_SHADER_CONSTANT_000_X,SpirvShaderTranslator::kConstantBufferFloatVertex,owner->vs_float_generation_))return false;
  if(st.ps){if(!floats(st.ps,rex::graphics::XE_GPU_REG_SHADER_CONSTANT_256_X,SpirvShaderTranslator::kConstantBufferFloatPixel,owner->ps_float_generation_))return false;}
  else uniforms[SpirvShaderTranslator::kConstantBufferFloatPixel]=uniforms[SpirvShaderTranslator::kConstantBufferFloatVertex];
  if(!constant(SpirvShaderTranslator::kConstantBufferFetch,fetch_delta,fetch_words.data(),sizeof(fetch_words)))return false;
  const auto bool_slot=SpirvShaderTranslator::kConstantBufferBoolLoop;
  if(const auto* cached=bool_constants.Find(owner->bool_loop_generation_,work)) {
    uniforms[bool_slot]=*cached;generation_hit(*cached);
  } else {
    if(!constant(bool_slot,bool_delta,&r[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],40*4))return false;
    bool_constants.Store(owner->bool_loop_generation_,work,uniforms[bool_slot]);
  }
  uint32_t texture_mask=st.vs->GetUsedTextureMaskAfterTranslation()|(st.ps?st.ps->GetUsedTextureMaskAfterTranslation():0);
  UpdateSystemConstants(st.primitive_polygonal,st.viewport,texture_mask,0,st.vs,st.ps);
  // The pinned translator emits scalar-packed system offsets. GLSL/UAM
  // requires std140, so copy exactly the ranges reflected by the converter.
  // Geometry shaders may declare only a subset and need their own layout.
  system_versions.Update({reinterpret_cast<const uint8_t*>(&system_constants),sizeof(system_constants)});
  DkBufExtents stage_system[3]{};
  CompiledShader* all_stage_shaders[]={st.pipeline->vs,st.pipeline->ps,st.pipeline->gs};
  for(uint32_t stage=0;stage<3;++stage) {
    const auto* shader=all_stage_shaders[stage];
    if(!shader||!shader->info.system_constant_size)continue;
    const auto& info=shader->info;
    if(info.system_constant_size>65536u||info.system_constant_copy_ranges.empty())return false;
    auto& pointer=packed_system_constants[shader];
    if(!pointer) {
      std::vector<ConstantCopyRange> ranges;
      for(const auto& range:info.system_constant_copy_ranges)
        ranges.push_back({range.source_offset,range.destination_offset,range.size});
      pointer=constant_layouts.Intern(UINT64_MAX,shared_constants_enabled?0:uintptr_t(shader),
          sizeof(system_constants),info.system_constant_size,ranges);
      if(!pointer){REXLOG_ERROR("[deko] system constant copy range exceeds the shader or CPU block");return false;}
    }
    auto& state=*pointer;
    // Every compatible stage shares this immutable packing plan. Once the CPU
    // system block is unchanged, repeated stages/draws need no second walk of
    // its reflected register versions. GPU uploads still require the same work.
    const uint64_t system_generation=system_versions.generation();
    if(state.plan.revision() && state.generation==system_generation) {
      if(cpu_profile_enabled){++profile.system_pack_reuses;++profile.system_generation_hits;}
    } else {
      ProfileScope timer(profile.constant_pack_ns,cpu_profile_enabled);
      const auto checked=state.plan.version_checks(),skipped=state.plan.version_skips();
      const auto copied=state.plan.packed_copy_bytes(),partial=state.plan.partial_packs(),bulk=state.plan.version_bulk_updates();
      if(state.plan.UpdateVersioned({reinterpret_cast<const uint8_t*>(&system_constants),sizeof(system_constants)},system_versions.versions())) {
        if(cpu_profile_enabled)++profile.constant_packs;
      } else if(cpu_profile_enabled)++profile.system_pack_reuses;
      profile.constant_version_checks+=state.plan.version_checks()-checked;
      profile.constant_version_skips+=state.plan.version_skips()-skipped;
      profile.constant_packed_copy_bytes+=state.plan.packed_copy_bytes()-copied;
      profile.constant_partial_packs+=state.plan.partial_packs()-partial;
      profile.constant_bulk_updates+=state.plan.version_bulk_updates()-bulk;
      state.generation=system_generation;
      if(cpu_profile_enabled){profile.constant_pack_bank_bytes+=sizeof(system_constants);profile.constant_pack_used_bytes+=state.plan.source_bytes_used();}
    }
    if(const auto* cached=state.upload.Find(state.plan.revision(),work)) {
      stage_system[stage]=*cached;generation_hit(*cached);continue;
    }
    const auto packed=state.plan.bytes();
    auto data=update_constant(state.delta,packed.data(),uint32_t(packed.size()),prepared_constant_hash(state),state.plan.revision(),&state.plan);if(!data.size)return false;
    stage_system[stage]=data;
    state.upload.Store(state.plan.revision(),work,stage_system[stage]);
  }
  phase.Next(profile.texture_ns);
  DrawTextureSet texture_set;
  auto& handles=texture_set.handles;auto& handle_count=texture_set.count;
  using TextureMemo=DrawTextureMemo;
  const TextureMemo::Shaders texture_shaders{uintptr_t(st.vs),uintptr_t(st.ps),uintptr_t(st.pipeline->vs),uintptr_t(st.pipeline->ps)};
  auto texture_stamp=[&]() -> TextureMemo::Stamp {
    return {work,frame,validation_cache.memo_epoch(),validation_cache.stats().guard_misses,
        0,0,uint64_t(validation_cache.enabled()),
        uint64_t(validation_cache.stats().sampling_disabled)};
  };
  const auto* texture_fetch=&r[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0];
  const auto before_textures=texture_stamp();
  const bool texture_cache_enabled=REXCVAR_GET(nx_native_draw_texture_cache);
  auto* previous=texture_cache_enabled?draw_textures.Find(texture_shaders,before_textures,texture_fetch):nullptr;
  if(previous && !TextureSetCurrent(*previous)) {draw_textures.Reject(previous);previous=nullptr;++profile.texture_dependency_misses;}
  if(previous) {
    texture_set=*previous;
    if(cpu_profile_enabled)++profile.texture_set_hits;
  } else {
    ++stats().texture_bindings_built;
    if(cpu_profile_enabled)++profile.texture_set_misses;
    uint32_t fetch_mask=0;bool eligible=texture_cache_enabled;
    SpirvShader* stage_shaders[]={st.vs,st.ps};CompiledShader* shaders_dk[]={st.pipeline->vs,st.pipeline->ps};
    for(uint32_t stage=0;stage<2;++stage) {
      if(!stage_shaders[stage]||!shaders_dk[stage])continue;
      auto& tb=stage_shaders[stage]->GetTextureBindingsAfterTranslation();
      auto& sb=stage_shaders[stage]->GetSamplerBindingsAfterTranslation();
      for(const auto& mapping:shaders_dk[stage]->info.textures) {
        if(mapping.image.binding>=tb.size()||mapping.sampler.binding<tb.size()||mapping.sampler.binding-tb.size()>=sb.size()||mapping.slot>=32)return false;
        const auto& image=tb[mapping.image.binding];const auto& sampler=sb[mapping.sampler.binding-tb.size()];
        if(image.fetch_constant>=32||sampler.fetch_constant>=32)return false;
        fetch_mask|=(uint32_t(1)<<image.fetch_constant)|(uint32_t(1)<<sampler.fetch_constant);
        auto binding=Texture(image,sampler);
        if(!binding){retry=textures->NeedsFlushRetry();return false;}
        eligible&=binding->reusable_in_draw_cache;
        if(texture_set.dependency_count>=texture_set.dependencies.size())return false;
        const auto dependency=texture_set.dependency_count++;
        texture_set.dependencies[dependency]={binding->image_slot,binding->guest_address,binding->image_identity,
            binding->content_version,binding->resolve_version,stage==0?ImageAccess::Vertex:ImageAccess::Fragment,
            binding->guest_memory};
        handles[stage][mapping.slot]=dkMakeTextureHandle(binding->image_slot,binding->sampler_slot);
        handle_count[stage]=std::max(handle_count[stage],mapping.slot+1);
      }
    }
    eligible&=TextureSetCurrent(texture_set);
    const auto after_textures=texture_stamp();
    if(cpu_profile_enabled&&(!eligible||before_textures!=after_textures))++profile.texture_set_ineligible;
    draw_textures.Store(texture_shaders,before_textures,after_textures,texture_fetch,fetch_mask,texture_set,eligible);
  }
  // All allocations are complete now. Nothing can rotate this work slot before
  // its consuming draw, including uploads performed by the texture cache.
  phase.Next(profile.record_ns);
  // Clears change targets and scissor; consume them before updating state keys.
  uint32_t width=8192,height=8192;
  for(auto* rt:st.targets.color)if(rt){width=std::min(width,rt->width);height=std::min(height,rt->height);}
  if(auto* rt=st.targets.depth){width=std::min(width,rt->width);height=std::min(height,rt->height);}
  bound_scissor.offset={int32_t(std::min(st.scissor.offset[0],width)),int32_t(std::min(st.scissor.offset[1],height))};
  bound_scissor.extent={std::min(st.scissor.extent[0],width-uint32_t(bound_scissor.offset.x)),std::min(st.scissor.extent[1],height-uint32_t(bound_scissor.offset.y))};
  const ClearRect draw_area{uint32_t(bound_scissor.offset.x),uint32_t(bound_scissor.offset.y),bound_scissor.extent.width,bound_scissor.extent.height};
  for(auto* rt:st.targets.color)if(rt)MaterializeClear(rt->image,&draw_area);
  if(st.targets.depth)MaterializeClear(st.targets.depth->image,&draw_area);
  for(auto* rt:st.targets.color)if(rt)BeforeImageWrite(rt->image);
  std::array<ImageAccess,69> draw_images{};
  size_t draw_image_count=texture_set.dependency_count;
  for(size_t i=0;i<draw_image_count;++i) {
    const auto& dep=texture_set.dependencies[i];draw_images[i]={dep.identity,dep.access};
  }
  for(auto* rt:st.targets.color)if(rt)draw_images[draw_image_count++]={rt->image.identity,ImageAccess::Write};
  if(st.targets.depth)draw_images[draw_image_count++]={st.targets.depth->image.identity,ImageAccess::Write};
  const std::span<const ImageAccess> draw_accesses{draw_images.data(),draw_image_count};
  core.InvalidarCodigoSiHaceFalta();BeforeImages(draw_accesses);DrawBarrier();
  const bool cache_state=REXCVAR_GET(nx_native_state_cache);
  using S=CommandStateCache;
  auto changed=[&](S::Slot slot,const auto& value) {
    const bool emit=command_state.Update(slot,value,cache_state);
    if(cpu_profile_enabled){if(emit)++profile.state_emitted;else ++profile.state_skipped;}
    return emit;
  };
  DkImageView colors[4],depth;const DkImageView* color_ptrs[4]{};
  uint32_t color_count=0;
  for(uint32_t i=0;i<4;++i)if(auto* rt=st.targets.color[i]) {
    colors[i]=View(rt->image);color_ptrs[i]=&colors[i];color_count=i+1;
    width=std::min(width,rt->width);height=std::min(height,rt->height);textures->Touch(rt->image,work);
    rt->last_draw_guest_msaa=uint32_t(r.Get<reg::RB_SURFACE_INFO>().msaa_samples);
  }
  if(st.targets.depth){depth=View(st.targets.depth->image);width=std::min(width,st.targets.depth->width);height=std::min(height,st.targets.depth->height);textures->Touch(st.targets.depth->image,work);}
  std::array<const DkImage*,5> target_key{};
  for(uint32_t i=0;i<4;++i)if(st.targets.color[i])target_key[i]=&st.targets.color[i]->image.image;
  if(st.targets.depth)target_key[4]=&st.targets.depth->image.image;
  auto cmd=core.comandos();
  if(changed(S::Targets,target_key))dkCmdBufBindRenderTargets(cmd,color_ptrs,color_count,st.targets.depth?&depth:nullptr);
  const DkShader* stages[3]={&st.pipeline->vs->shader,st.pipeline->ps?&st.pipeline->ps->shader:nullptr,st.pipeline->gs?&st.pipeline->gs->shader:nullptr};
  const DkShader* active[3]{};uint32_t ns=0;for(auto* shader:stages)if(shader)active[ns++]=shader;
  if(changed(S::Shaders,stages))dkCmdBufBindShaders(cmd,DkStageFlag_GraphicsMask,active,ns);
  DkBufExtents shared;
  InitializeStorageBufferDescriptor(shared,stream_base,stream_bytes);
  uint32_t stage_index=0;
  const bool bind_storage=changed(S::Storage,std::array<uint64_t,2>{shared.addr,shared.size});
  for(DkStage stage:{DkStage_Vertex,DkStage_Fragment,DkStage_Geometry}) {
    uniforms[SpirvShaderTranslator::kConstantBufferSystem]=stage_system[stage_index++];
    std::array<uint64_t,2*SpirvShaderTranslator::kConstantBufferCount> uniform_key{};
    for(uint32_t i=0;i<SpirvShaderTranslator::kConstantBufferCount;++i) {
      uniform_key[i*2]=uniforms[i].addr;uniform_key[i*2+1]=uniforms[i].size;
    }
    if(changed(S::Slot(S::UniformVS+stage_index-1),uniform_key))
      dkCmdBufBindUniformBuffers(cmd,stage,0,uniforms,SpirvShaderTranslator::kConstantBufferCount);
    if(bind_storage)dkCmdBufBindStorageBuffers(cmd,stage,0,&shared,1);
  }
  for(uint32_t stage=0;stage<2;++stage)if(handle_count[stage]) {
    std::array<uint32_t,33> texture_key{};
    std::copy(handles[stage].begin(),handles[stage].end(),texture_key.begin());texture_key[32]=handle_count[stage];
    if(changed(stage?S::TexturesPS:S::TexturesVS,texture_key))
      dkCmdBufBindTextures(cmd,stage?DkStage_Fragment:DkStage_Vertex,0,handles[stage].data(),handle_count[stage]);
  }
  DkRasterizerState raster{};dkRasterizerStateDefaults(&raster);
  raster.cullMode=DkFace(key.cull_mode);raster.frontFace=key.front_face_clockwise?DkFrontFace_CW:DkFrontFace_CCW;
  raster.depthClampEnable=key.depth_clamp;raster.provokingVertex=DkProvokingVertex_First;
  raster.polygonModeFront=raster.polygonModeBack=key.polygon_line?DkPolygonMode_Line:DkPolygonMode_Fill;
  raster.depthBiasEnableMask=7;if(changed(S::Raster,raster))dkCmdBufBindRasterizerState(cmd,&raster);
  DkMultisampleState ms{};dkMultisampleStateDefaults(&ms);if(changed(S::Multisample,ms))dkCmdBufBindMultisampleState(cmd,&ms);
  // Maxwell B197 SET_SAMPLE_MASK_X*_Y* (0xFBC..0xFC8) only accepts bits 15:0.
  // deko3d 0.5.0 forwards the mask unchanged; UINT32_MAX caused the physical
  // Switch run047 method error [b197:3ef]=0xffffffff. Keep all valid samples.
  if(changed(S::SampleMask,uint32_t(0xFFFF)))dkCmdBufSetSampleMask(cmd,0xFFFFu);
  DkDepthStencilState ds{};dkDepthStencilStateDefaults(&ds);
  ds.depthTestEnable=key.depth_test;ds.depthWriteEnable=key.depth_write;ds.depthCompareOp=DkCompareOp(key.depth_compare+1);ds.stencilTestEnable=key.stencil_test;
  ds.stencilFrontFailOp=DkStencilOp(key.stencil_front[0]+1);ds.stencilFrontPassOp=DkStencilOp(key.stencil_front[1]+1);ds.stencilFrontDepthFailOp=DkStencilOp(key.stencil_front[2]+1);ds.stencilFrontCompareOp=DkCompareOp(key.stencil_front[3]+1);
  ds.stencilBackFailOp=DkStencilOp(key.stencil_back[0]+1);ds.stencilBackPassOp=DkStencilOp(key.stencil_back[1]+1);ds.stencilBackDepthFailOp=DkStencilOp(key.stencil_back[2]+1);ds.stencilBackCompareOp=DkCompareOp(key.stencil_back[3]+1);
  if(changed(S::DepthStencil,ds))dkCmdBufBindDepthStencilState(cmd,&ds);
  if(changed(S::Stencil,st.stencil)) {
    dkCmdBufSetStencil(cmd,DkFace_Front,st.stencil[2],st.stencil[4],st.stencil[0]);
    dkCmdBufSetStencil(cmd,DkFace_Back,st.stencil[3],st.stencil[5],st.stencil[1]);
  }
  DkColorState cs{};dkColorStateDefaults(&cs);DkBlendState blends[4]{};
  for(uint32_t i=0;i<4;++i){auto raw=key.blend[i];dkBlendStateDefaults(&blends[i]);
    dkColorStateSetBlendEnable(&cs,i,raw!=0x00010001);
    dkBlendStateSetOps(&blends[i],BlendOp((raw>>5)&7),BlendOp((raw>>21)&7));
    dkBlendStateSetFactors(&blends[i],BlendFactor(raw&31),BlendFactor((raw>>8)&31),BlendFactor((raw>>16)&31),BlendFactor((raw>>24)&31));}
  if(changed(S::Color,cs))dkCmdBufBindColorState(cmd,&cs);
  if(changed(S::Blends,blends))dkCmdBufBindBlendStates(cmd,0,blends,4);
  DkColorWriteState cw{};cw.masks=key.write_masks;if(changed(S::ColorWrite,cw))dkCmdBufBindColorWriteState(cmd,&cw);
  if(changed(S::BlendConstant,st.blend_constants))dkCmdBufSetBlendConst(cmd,st.blend_constants[0],st.blend_constants[1],st.blend_constants[2],st.blend_constants[3]);
  DkViewport vp{st.vp.x,st.vp.y,st.vp.width,st.vp.height,st.vp.minDepth,st.vp.maxDepth};if(changed(S::Viewport,vp))dkCmdBufSetViewports(cmd,0,&vp,1);
  DkScissor sc{uint32_t(bound_scissor.offset.x),uint32_t(bound_scissor.offset.y),bound_scissor.extent.width,bound_scissor.extent.height};if(changed(S::Scissor,sc))dkCmdBufSetScissors(cmd,0,&sc,1);
  if(changed(S::DepthBias,std::array<float,2>{st.bias_constant,st.bias_slope}))SetGuestDepthBias(cmd,st.bias_constant,st.bias_slope);
  if(changed(S::Restart,st.reset_enabled))dkCmdBufSetPrimitiveRestart(cmd,st.reset_enabled,UINT32_MAX);
  if(changed(S::EmptyVertex,true)){dkCmdBufBindVtxAttribState(cmd,nullptr,0);dkCmdBufBindVtxBufferState(cmd,nullptr,0);}
  if(host_indexed){
    const auto format=use_index16?DkIdxFormat_Uint16:DkIdxFormat_Uint32;
    if(changed(S::Index,std::array<uint64_t,2>{index_data.gpu,uint64_t(format)}))dkCmdBufBindIdxBuffer(cmd,format,index_data.gpu);
    dkCmdBufDrawIndexed(cmd,Primitive(key.topology),draw_count,1,0,0,0);
    if(use_index16)++profile.index16_draws;else ++profile.index32_draws;
    if(index_compact_output)++profile.index_compact_draws;
  }
  else dkCmdBufDraw(cmd,Primitive(key.topology),draw_count,1,0,0);
  image_barriers.Images(draw_accesses);
  for(auto* rt:st.targets.color)if(rt)textures->Changed(rt->image);
  if(st.targets.depth)textures->Changed(st.targets.depth->image);
  // Conservative write coverage includes attachments with disabled writes.
  // This may lose a repeated-clear hit, but can never reuse stale clear state.
  for(auto* rt:st.targets.color)if(rt)rt->image.pending_clear.Written(draw_area);
  if(st.targets.depth)st.targets.depth->image.pending_clear.Written(draw_area);
  ++stats().draws;
  if(cpu_profile_enabled) {
    ++profile.guest_samples[std::min(uint32_t(r.Get<reg::RB_SURFACE_INFO>().msaa_samples),3u)];
    profile.max_target_width=std::max(profile.max_target_width,uint64_t(width));
    profile.max_target_height=std::max(profile.max_target_height,uint64_t(height));
  }
  phase.Next(profile.mirror_ns);MirrorClears();return true;
}
void Renderer::Impl::Draw(const uint32_t* indices,uint32_t words) {
  if(DrawOnce(indices,words))return;
  // A large draw may need the full original 64 MiB slot. Drop only the next
  // slot's metadata after its fence and retry without a persistent prefix.
  // Shared prefixes additionally drain cross-slot readers in Begin().
  if(retry){Flush();transient_next_work=true;if(DrawOnce(indices,words))return;}
  ++stats().draws_skipped;++stats().skipped_by_reason[retry?9:8];
  static uint32_t messages=0;if(messages++<16)REXLOG_ERROR("[deko] draw rejected after complete resource validation (allocation_retry={})",retry);
}

Image* Renderer::Impl::ResolveSample(uint32_t address,uint32_t width,uint32_t height) {
  uint32_t root=address,x=0,y=0;
  if(auto a=resolve_aliases.find(address);a!=resolve_aliases.end()){root=a->second[0];x=a->second[1];y=a->second[2];}
  auto it=resolves.find(root);if(it==resolves.end())return nullptr;
  if(frontbuffer.destination==it->second.get())MaterializeFrontbuffer();
  auto& source=*it->second;
  source.last_frame=frame;
  if(!width)width=source.image.width-x;if(!height)height=source.image.height-y;
  if(x>=source.image.width||y>=source.image.height||width>source.image.width-x||height>source.image.height-y)return nullptr;
  if(!x&&!y&&width==source.image.width&&height==source.image.height){MaterializeClear(source.image);textures->Touch(source.image,work);return &source.image;}
  const std::array<uint32_t,5> key{root,x,y,width,height};auto& crop=crops[key];
  crop.last_frame=frame;
  if(crop.image.valid && crop.image.format!=source.image.format)textures->FreeImage(crop.image);
  if(!crop.image.valid) {
    auto img=textures->CreateImage(source.image.format,width,height,DkImageType_2D,1,1,ColorImageFlags(source.image.format,color_compression_enabled&&!source.is_depth));
    if(!img)return nullptr;crop.image=std::move(*img);crop.version=UINT64_MAX;
  }
  crop.image.host_format_swizzle=source.image.host_format_swizzle;
  // First-use crops are copied in the ordered stream before binding. The full
  // region is known covered (initially-cleared roots included); no rescaling fallback.
  if(crop.version!=source.version){Copy(source.image,crop.image,x,y,0,0,width,height);crop.version=source.version;}
  textures->Touch(crop.image,work);return &crop.image;
}
std::optional<TextureResourceBinding> Renderer::Impl::Texture(const SpirvShader::TextureBinding& binding,const SpirvShader::SamplerBinding& sampler) {
  const auto fetch=regs->GetTextureFetch(binding.fetch_constant);
  uint32_t address=(fetch.base_address<<12)&0x1FFFFFFF;
  uint32_t root=address;if(auto a=resolve_aliases.find(address);a!=resolve_aliases.end())root=a->second[0];
  auto resolved=resolves.find(root);
  if(fetch.type==xenos::FetchConstantType::kTexture&&
     fetch.dimension==xenos::DataDimension::k2DOrStacked&&!fetch.stacked&&
     resolved!=resolves.end()&&binding.dimension==xenos::FetchOpDimension::k2D) {
    const bool depth_format=fetch.format==xenos::TextureFormat::k_24_8||fetch.format==xenos::TextureFormat::k_24_8_FLOAT;
    if(resolved->second->is_depth&&!depth_format)address|=1;
    auto* image=ResolveSample(address,fetch.size_2d.width+1,fetch.size_2d.height+1);
    if(!image)return std::nullopt;
    auto view=textures->GetTextureImageView(*image,fetch,bool(binding.is_signed),DkImageType_2DArray);
    auto samp=textures->GetSampler(regs->GetTextureFetch(sampler.fetch_constant),sampler);
    if(!view||!samp)return std::nullopt;
    return TextureResourceBinding{*view,*samp,image->width,image->height,true,image->identity,image->content_version,
        (uint32_t(fetch.base_address)<<12)&0x1FFFFFFFu,ResolveVersion((uint32_t(fetch.base_address)<<12)&0x1FFFFFFFu)};
  }
  auto sampler_fetch=regs->GetTextureFetch(sampler.fetch_constant);
  auto result=textures->GetTexture(fetch,binding.dimension,bool(binding.is_signed),sampler,&sampler_fetch);
  if(result) {result->guest_address=address;result->resolve_version=ResolveVersion(address);}
  return result;
}

void Renderer::Impl::Swap(rex::ui::Presenter* presenter,uint32_t frontbuffer_address,uint32_t width,uint32_t height) {
  ProfileScope phase(profile.swap_prepare_ns,cpu_profile_enabled);
  Begin();auto fetch=regs->GetTextureFetch(0);uint32_t address=frontbuffer_address&0x1FFFFFFF,swizzle=xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  if(fetch.type==xenos::FetchConstantType::kTexture&&fetch.base_address){address=(fetch.base_address<<12)&0x1FFFFFFF;swizzle=fetch.swizzle;
    if(fetch.dimension==xenos::DataDimension::k2DOrStacked){
      if(!width)width=fetch.size_2d.width+1;
      if(!height)height=fetch.size_2d.height+1;
    }}
  // Keep VdSwap's requested visible dimensions when the fetch includes padding.
  // Clamp only to covered source storage, then make an exact crop for sampling.
  uint32_t root=address,origin_x=0,origin_y=0;
  if(auto alias=resolve_aliases.find(address);alias!=resolve_aliases.end()) {
    root=alias->second[0];origin_x=alias->second[1];origin_y=alias->second[2];
  }
  if(auto resolved=resolves.find(root);resolved!=resolves.end()) {
    const auto& image=resolved->second->image;
    if(origin_x<image.width&&origin_y<image.height) {
      width=std::min(width?width:image.width-origin_x,image.width-origin_x);
      height=std::min(height?height:image.height-origin_y,image.height-origin_y);
    }
  }
  last_frontbuffer=root;
  const bool direct=frontbuffer.source && frontbuffer.destination->address==root &&
      !origin_x && !origin_y && width && height && width<=frontbuffer.width && height<=frontbuffer.height;
  Image* source=direct?&frontbuffer.source->image:ResolveSample(address,width,height);
  if(!source||!presenter){static uint32_t n=0;if(n++<12)REXLOG_WARN("[deko] missing complete frontbuffer {:08X} {}x{}",address,width,height);Flush();++frame;return;}
  if(!direct){width=source->width;height=source->height;}
  static constexpr std::string_view vs=R"(#version 460
void main(){vec2 p=vec2(float((gl_VertexID&1)<<2)-1.0,float((gl_VertexID&2)<<1)-1.0);gl_Position=vec4(p,0,1);})";
  static constexpr std::string_view fs=R"(#version 460
layout(binding=0) uniform sampler2D source_image;
layout(std140,binding=0) uniform Output { uvec4 ramp[64]; vec4 size; } output_data;
layout(location=0) out vec4 color;
uint lut(uint i){uvec4 p=output_data.ramp[i>>2];uint c=i&3u;
 return c==0u?p.x:(c==1u?p.y:(c==2u?p.z:p.w));}
void main(){vec4 c=textureLod(source_image,(gl_FragCoord.xy/output_data.size.xy)*output_data.size.zw,0.0);
 uvec3 i=uvec3(clamp(c.rgb,0.0,1.0)*255.0+0.5);
 color=vec4(float((lut(i.r)>>20)&1023u),float((lut(i.g)>>10)&1023u),float(lut(i.b)&1023u),c.a*1023.0)/1023.0;})";
  auto* vertex=CompileText(vs,ShaderStage::kVertex);auto* fragment=CompileText(fs,ShaderStage::kFragment);
  if(!vertex||!fragment){Flush();++frame;return;}
  auto image_slot=textures->GetImageView(*source,swizzle,DkImageType_2D);
  SpirvShader::SamplerBinding sb{};sb.mag_filter=xenos::TextureFilter::kPoint;sb.min_filter=xenos::TextureFilter::kPoint;sb.mip_filter=xenos::TextureFilter::kPoint;
  auto sampler_slot=textures->GetSampler(fetch,sb,true);
  if(!image_slot||!sampler_slot){Flush();++frame;return;}
  rex::system::X_VIDEO_MODE mode;
  rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
  const uint32_t display_width=std::max(1u,uint32_t(mode.display_width));
  const uint32_t display_height=std::max(1u,uint32_t(mode.display_height));
  phase.Next(profile.refresh_ns);
  bool presented=presenter->RefreshGuestOutput(width,height,display_width,display_height,[&](rex::ui::Presenter::GuestOutputRefreshContext& generic){
    auto& output=static_cast<rex::deko3d::ContextoSalidaDeko3d&>(generic);
    Begin();struct Constants{uint32_t ramp[256];float size[4];} constants{};
    memcpy(constants.ramp,gamma_ramp,sizeof(gamma_ramp));constants.size[0]=float(output.ancho());constants.size[1]=float(output.alto());
    constants.size[2]=float(width)/float(source->width);constants.size[3]=float(height)/float(source->height);
    auto data=core.Reservar(sizeof(constants),256);if(!data){Flush();Begin();data=core.Reservar(sizeof(constants),256);}if(!data)return false;
    memcpy(data.cpu,&constants,sizeof(constants));command_state.Invalidate();draw_textures.Invalidate();
    BeforeGraphics(true,true);draw_barriers.CpuUpload();DrawBarrier();core.InvalidarCodigoSiHaceFalta();
    DkImageView view;dkImageViewDefaults(&view,&output.imagen());const DkImageView* views[]={&view};
    auto cmd=core.comandos();dkCmdBufBindRenderTargets(cmd,views,1,nullptr);
    const DkShader* shaders[]={&vertex->shader,&fragment->shader};dkCmdBufBindShaders(cmd,DkStageFlag_GraphicsMask,shaders,2);
    DkBufExtents uniform{data.gpu,sizeof(constants)};dkCmdBufBindUniformBuffers(cmd,DkStage_Fragment,0,&uniform,1);
    DkResHandle handle=dkMakeTextureHandle(*image_slot,*sampler_slot);dkCmdBufBindTextures(cmd,DkStage_Fragment,0,&handle,1);
    DkRasterizerState rs;dkRasterizerStateDefaults(&rs);rs.cullMode=DkFace_None;dkCmdBufBindRasterizerState(cmd,&rs);
    DkDepthStencilState ds;dkDepthStencilStateDefaults(&ds);ds.depthTestEnable=false;ds.depthWriteEnable=false;ds.stencilTestEnable=false;dkCmdBufBindDepthStencilState(cmd,&ds);
    DkColorState cs;dkColorStateDefaults(&cs);dkCmdBufBindColorState(cmd,&cs);
    DkColorWriteState cw;dkColorWriteStateDefaults(&cw);dkCmdBufBindColorWriteState(cmd,&cw);
    DkMultisampleState ms;dkMultisampleStateDefaults(&ms);dkCmdBufBindMultisampleState(cmd,&ms);
    DkViewport vp{0,0,float(output.ancho()),float(output.alto()),0,1};DkScissor sc{0,0,output.ancho(),output.alto()};
    dkCmdBufSetViewports(cmd,0,&vp,1);dkCmdBufSetScissors(cmd,0,&sc,1);dkCmdBufSetDepthBias(cmd,0,0,0);dkCmdBufSetPrimitiveRestart(cmd,false,0);
    dkCmdBufDraw(cmd,DkPrimitive_Triangles,3,1,0,0);image_barriers.Graphics();
    // Presenter is outside this tracker: keep the full outgoing handoff.
    Barrier();textures->Touch(*source,work);Flush();generic.SetIs8bpc(false);return true;
  });
  if(presented&&direct)++profile.front_direct;
  if(!presented)Flush();++frame;
}

void Renderer::Impl::WarmupObservedShaders() {
  if(!REXCVAR_GET(nx_native_shader_warmup)) {
    REXLOG_INFO("[deko-cache] startup warmup disabled; observed variants will still be recorded");
    return;
  }
  const auto start=std::chrono::steady_clock::now();
  const auto previous=stats();
  warming_up=true;
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  if(std::filesystem::exists(warmup_root,ec)) {
    std::filesystem::directory_iterator it(warmup_root,ec),end;
    for(;!ec&&it!=end&&files.size()<4096;it.increment(ec)) {
      if(it->path().extension()==".fwarm"&&it->is_regular_file(ec))files.push_back(it->path());
    }
  }
  if(ec)REXLOG_WARN("[deko-cache] warmup directory: {}",ec.message());
  std::sort(files.begin(),files.end());
  REXLOG_INFO("[deko-cache] startup warmup: {} observed variants; prepared cache {}",files.size(),prepared_cache_root.string());
  uint64_t input_bytes=0;
  for(size_t i=0;i<files.size();++i) {
    if(shader_code_budget_used>=(32u<<20)||input_bytes>=(32u<<20)) {
      REXLOG_WARN("[deko-cache] warmup budget reached after {} variants; remaining shaders load on demand",i);
      break;
    }
    shader_warmup::Entry entry;std::string error;
    if(!shader_warmup::Load(files[i],entry,error)) {
      ++stats().shader_warmup_failed;
      if(stats().shader_warmup_failed<=8)REXLOG_WARN("[deko-cache] ignored warmup {}: {}",files[i].filename().string(),error);
      continue;
    }
    input_bytes+=entry.ucode.size()*sizeof(uint32_t);
    auto& shader=shaders[entry.shader_hash];
    if(!shader)shader=std::make_unique<SpirvShader>(xenos::ShaderType(entry.shader_type),entry.shader_hash,entry.ucode.data(),entry.ucode.size(),std::endian::native);
    if(!shader->is_ucode_analyzed())shader->AnalyzeUcode(disasm);
    auto* translation=static_cast<SpirvShader::SpirvTranslation*>(shader->GetOrCreateTranslation(entry.modification));
    bool pending=false;
    if(!EnsureTranslated(translation,&pending)) {++stats().shader_warmup_failed;continue;}
    const auto& binary=translation->translated_binary();
    const auto stage=entry.shader_type==shader_warmup::kVertexShader?ShaderStage::kVertex:ShaderStage::kFragment;
    if(Compile({reinterpret_cast<const uint32_t*>(binary.data()),binary.size()/4},stage)) {
      ++stats().shader_warmup_loaded;
      recorded_variants.emplace(entry.shader_hash,entry.modification);
    } else {++stats().shader_warmup_failed;}
    if((i+1)%32==0)REXLOG_INFO("[deko-cache] startup warmup {}/{}",i+1,files.size());
  }
  // These compute programs do not depend on the guest draw state.
  if(REXCVAR_GET(nx_native_copy3d)) {
    if(!CompileText(kImageCopyVertex,ShaderStage::kVertex))++stats().shader_warmup_failed;
    if(!CompileText(kImageCopyFragment,ShaderStage::kFragment))++stats().shader_warmup_failed;
  }
  if(!CompileText(kDepthResolveGlsl,ShaderStage::kCompute))++stats().shader_warmup_failed;
  if(!CompileText(kDepthFloatResolveGlsl,ShaderStage::kCompute))++stats().shader_warmup_failed;
  warming_up=false;
  REXLOG_INFO("[deko-cache] warmup complete: {} ready, {} failed, {} prepared hits, {} DKSH hits, {} UAM compiles in {:.1f} ms",
      stats().shader_warmup_loaded-previous.shader_warmup_loaded,
      stats().shader_warmup_failed-previous.shader_warmup_failed,
      stats().shader_prepared_hits-previous.shader_prepared_hits,
      stats().shader_dksh_hits-previous.shader_dksh_hits,
      stats().shader_compiles-previous.shader_compiles,
      double(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count())/1000.0);
}

void Renderer::Impl::FinishProfileFrame() {
  const uint64_t now=cpu_profile_enabled?ProfileClockNs():0;
  if(cpu_profile_enabled) {
    const uint64_t cpu=RingThreadCpuNs();
    if(ring_cpu_before && cpu>=ring_cpu_before)profile.ring_cpu_ns+=cpu-ring_cpu_before;
    ring_cpu_before=cpu;
  }
  if(last_profile_frame_ns) {
    const uint64_t elapsed=now-last_profile_frame_ns;
    if(elapsed>50000000) {
      ++stats().frames_over_50ms;
      if(elapsed>100000000)++stats().frames_over_100ms;
      stats().slow_frame_shaders+=stats().shaders_translated-frame_shaders_before;
      stats().slow_frame_pipelines+=stats().pipelines_created-frame_pipelines_before;
      stats().slow_frame_uploads+=stats().textures_uploaded-frame_uploads_before;
      stats().slow_frame_render_targets+=stats().render_targets_created-frame_targets_before;
    }
  }
  last_profile_frame_ns=now;
  stats().frame_draws=stats().draws-frame_draws_before;frame_draws_before=stats().draws;
  const uint64_t fence=core.NsEsperandoValla();
  stats().frame_fence_wait_ns=fence-frame_fence_before;frame_fence_before=fence;
  stats().frame_texture_ns=profile.texture_ns-frame_texture_before;frame_texture_before=profile.texture_ns;
  frame_shaders_before=stats().shaders_translated;frame_pipelines_before=stats().pipelines_created;
  frame_uploads_before=stats().textures_uploaded;frame_targets_before=stats().render_targets_created;
  stats().gpu_ns=gpu_profile.gpu_ns;stats().gpu_submissions=gpu_profile.gpu_submissions;
}

Renderer::Renderer():impl_(std::make_unique<Impl>()){impl_->owner=this;}
Renderer::~Renderer(){Shutdown();}
bool Renderer::Initialize(rex::deko3d::ProveedorDeko3d* provider,rex::memory::Memory* memory,RegisterFile* regs) {
  auto& p=*impl_;p.provider=provider;p.memory=memory;p.regs=regs;
  p.zero_prefixes={};p.stream_cpu_base=nullptr;
  REXLOG_INFO("[deko-front] deferred frontbuffer v1: {}; exact color snapshot, crop-aware presentation, copy before conflicting access; compatible whole-clear exchange",
      REXCVAR_GET(nx_native_deferred_frontbuffer));
  p.cpu_profile_enabled=REXCVAR_GET(nx_native_cpu_profile);
  p.color_compression_enabled=REXCVAR_GET(nx_native_color_compression);
  p.gpu_profile_enabled=REXCVAR_GET(nx_native_gpu_profile);
  p.geometry_push_upload=REXCVAR_GET(nx_native_geometry_push_upload);
  p.validation_cache.SetEnabled(REXCVAR_GET(nx_native_validation_cache));
  p.geometry_cache_enabled=REXCVAR_GET(nx_native_geometry_cache);
  p.shared_geometry_enabled=p.geometry_cache_enabled&&REXCVAR_GET(nx_native_shared_geometry);
  p.shared_vertices.FastLookup(REXCVAR_GET(nx_native_geometry_fast_lookup));
  p.shared_indices.FastLookup(REXCVAR_GET(nx_native_geometry_fast_lookup));
  p.ordered_index_uploads=REXCVAR_GET(nx_native_index_ordered_upload)&&REXCVAR_GET(nx_native_index_resident_batch);
  p.shared_indices.PreferSequentialAllocations(p.ordered_index_uploads);
  // Give the existing adjacent vertex batch consecutive fresh destinations.
  // Only free space is considered; minimum SSBO offsets and fences still apply.
  p.sequential_vertex_uploads=REXCVAR_GET(nx_native_vertex_sequential_upload);
  p.shared_vertices.PreferSequentialAllocations(p.sequential_vertex_uploads);
  p.shared_constants_enabled=REXCVAR_GET(nx_native_shared_constants);
  p.validation_cache.BeginFrame(p.frame);
  rex::deko3d::ConfigNucleo config;config.ranuras=kWorkSlots;config.mb_subida=kUploadBytes>>20;config.mb_codigo=64;config.descriptores_imagen=32768;config.descriptores_sampler=1024;config.kb_previos=p.geometry_push_upload?1024:0;
  p.core.Crear(provider->dispositivo(),config);p.textures=std::make_unique<TextureCache>(p.core,memory,p.validation_cache);
  if(REXCVAR_GET(nx_native_isolated_constants)) {
    // Separate memory, not merely a different offset in the 256 MiB geometry
    // heap: the translated vertex shader exposes that entire heap as an SSBO.
    p.constant_memory=provider->dispositivo().CrearBloque(kWorkSlots*p.kConstantUploadSlotBytes,
        DkMemBlockFlags_CpuUncached|DkMemBlockFlags_GpuCached,"FEAR isolated constants");
    if(!p.constant_memory)rex::deko3d::Fatal("cannot allocate isolated constant ring");
    p.constant_memory_base={static_cast<uint8_t*>(dkMemBlockGetCpuAddr(p.constant_memory)),
        dkMemBlockGetGpuAddr(p.constant_memory)};
  }
  p.textures->SetProfilingEnabled(p.cpu_profile_enabled);p.Begin();
  if(!p.textures->Initialize())return false;
  SpirvShaderTranslator::Features features(false);features.spirv_version=0x10000;features.max_storage_buffer_range=512u<<20;
  features.full_draw_index_uint32=true;features.clip_distance=true;features.cull_distance=true;features.image_view_format_swizzle=true;
  p.translator=std::make_unique<SpirvShaderTranslator>(features,true,true,false);
  for(uint32_t i=0;i<256;++i){uint32_t v=i*1023/255;p.gamma_ramp[i]=(v<<20)|(v<<10)|v;}
  p.initialized=true;REXLOG_INFO("[deko] FEAR renderer initialized; synchronous DKSH compilation, ordered uploads, preserved Xenos shader ABI; Maxwell sample mask 0xFFFF (run047 fix)");
  REXLOG_INFO("[deko-constants] patch=060: isolated ring {}; {} KiB/slot, {} KiB resident; unchanged delta updates and slot fences; geometry SSBO excludes mutable constants",
      p.constant_memory!=nullptr,p.kConstantUploadSlotBytes>>10,p.kConstantSlotBytes>>10);
  REXLOG_INFO("[deko-cpu] patch=061: packed dirty envelopes {}; texture memo 4-way, material sets 512; partial idle sends {} (max 3, guest validation preserved); SDK unchanged",
      REXCVAR_GET(nx_native_constant_dirty_ranges),REXCVAR_GET(nx_native_partial_kicks));
  REXLOG_INFO("[deko-upload] index writes v1: CPU payload alignment 16; sequential writes {}; new resident batching {}; adjacent fresh 64-byte blocks only, 64 KiB capture, drain before submit",
      REXCVAR_GET(nx_native_index_write_fast),REXCVAR_GET(nx_native_index_resident_batch));
  REXLOG_INFO("[deko-upload] index ordered v2: {}; 64 KiB immediate snapshots; skip sorting and assembly when already ordered; join exact CPU/GPU neighbors only; unchanged fences and NEON",
      p.ordered_index_uploads);
  REXLOG_INFO("[deko-geometry] vertex writes v1: sequential writes {}; resident batching {}; unchanged guest bytes, shader endian preserved; small fresh blocks up to 64 KiB, 256 KiB capture, drain before submit",
      REXCVAR_GET(nx_native_vertex_write_fast),REXCVAR_GET(nx_native_vertex_resident_batch));
  REXLOG_INFO("[deko-geometry] vertex placement v1: sequential preference {}, shared geometry {}; existing adjacent batch, best-fit fallback, unchanged minimum offsets and GPU lifetime",
      p.sequential_vertex_uploads,p.shared_geometry_enabled);
  REXLOG_INFO("[deko-image] stage1: index16 {}, deferred clears {}, color copy3D {}, UNORM color compression {}; depth/stencil compute preserved",
      REXCVAR_GET(nx_native_index16),REXCVAR_GET(nx_native_deferred_clears),REXCVAR_GET(nx_native_copy3d),p.color_compression_enabled);
  REXLOG_INFO("[deko-image] stage2: dependency barriers {}, rectangle clears {}; exact masks, bounded regions, materialize before use; full compute/presenter handoffs",
      REXCVAR_GET(nx_native_image_barriers),REXCVAR_GET(nx_native_clear_regions));
  REXLOG_INFO("[deko-geometry] vertex batch v1: enabled {}; <=64 KiB transient payloads, 128 KiB chunks; minimum SSBO offsets and resident cache preserved",
      REXCVAR_GET(nx_native_small_vertex_batch));
  REXLOG_INFO("[deko-upload] index validation reuse v1: enabled {}; exact source compare validates remembered hash; no epoch-only trust",
      REXCVAR_GET(nx_native_index_validation_reuse));
  REXLOG_INFO("[deko-upload] index compact16 v1: enabled {}; raw converted values <=65535; base/clamp unchanged; restart retains Uint32",
      REXCVAR_GET(nx_native_index16)&&REXCVAR_GET(nx_native_index16_compact));
  REXLOG_INFO("[deko-geometry] vertex grid v1: enabled {}; stable 64 KiB cells, conservative proof unchanged",
      REXCVAR_GET(nx_native_vertex_grid_windows));
  REXLOG_INFO("[deko-geometry] dynamic windows v3: {}; 16-byte aligned proven reads for volatile/quarantined ranges; stable windows and exact-hash fallback preserved; epoch reuse {} (16384 entries, work/frame/guest synchronization/guard boundaries)",
      REXCVAR_GET(nx_native_vertex_dynamic_windows),REXCVAR_GET(nx_native_vertex_epoch_reuse));
  REXLOG_INFO("[deko-epochs] split epochs v1: immediate COHER_STATUS_HOST waits end vertex-only reuse; memory/other/blocking waits, writes and interrupts retain global revalidation; frame/work/guard boundaries preserved");
  REXLOG_INFO("[deko-write-range] extent v2: exact 12-byte writes; bounded latest-write history evicts the oldest address instead of clearing all; old leases remain conservative, unchanged overlap and epoch rules");
  REXLOG_INFO("[deko-vertex-probe] v1 enabled {}: <=128 KiB identical payloads, guest->RAM / guest->GPU-upload / RAM->GPU-upload; separate scratch allocation, at most once per 60 frames and 1 second; thread ticks may be unavailable in Eden",
      p.cpu_profile_enabled&&REXCVAR_GET(nx_native_vertex_write_probe));
  REXLOG_INFO("[deko-upload] small index batch v1: enabled {}; <=2048 byte transient payloads in 4096 byte chunks; drain before submission; resident cache unchanged",
      REXCVAR_GET(nx_native_small_index_batch));
  REXLOG_INFO("[deko-upload] constant slots v1: enabled {}, {} KiB protected per slot; fenced same-slot reuse, exact delta pushes; clear coalescing {}",
      REXCVAR_GET(nx_native_constant_slots),p.kConstantSlotBytes>>10,REXCVAR_GET(nx_native_clear_coalesce));
  REXLOG_INFO("[deko-opt] patch=059: grouped constant versions, selective packing, resident revision reuse, texture binding validation reuse, backlog command coalescing");
  p.reported_profile=p.profile;p.reported_texture_profile=p.textures->GetProfile();
  p.reported_fence_ns=p.frame_fence_before=p.core.NsEsperandoValla();
  REXLOG_INFO("[deko-perf] phase profiling v1: CPU wall scopes {}, GPU timestamp sampling {}; per-frame means per guest swap; child timings overlap parents, GPU samples are not utilization",p.cpu_profile_enabled,p.gpu_profile_enabled);
  REXLOG_INFO("[deko-shared] resources v1: geometry={} constants={} textures={}; one 256 MiB upload heap; fence-pinned 64-byte allocations; compatible layouts only",p.shared_geometry_enabled,p.shared_constants_enabled,REXCVAR_GET(nx_native_draw_texture_cache));
  REXLOG_INFO("[deko-cpu] NFSMW audit v1: geometry front tables {}; reflection-only constant snapshots; exact NEON comparisons; texture bucket mix with exact keys; frontbuffer unchanged",REXCVAR_GET(nx_native_geometry_fast_lookup));
  p.validation_cache.SetGeometryEpochChecks(REXCVAR_GET(nx_native_geometry_epoch_checks));
  REXLOG_INFO("[deko-checks] NFSMW schedule v1: texture intervals 1-39 frames, 4KiB samples, 3000 guarded checks, 1/8 full rechecks, 6MiB/frame budget; geometry epoch checks {}",REXCVAR_GET(nx_native_geometry_epoch_checks));
  REXLOG_INFO("[deko-checks] geometry policy v3 on local epoch miss: {}; texture schedule unchanged",
      REXCVAR_GET(nx_native_geometry_epoch_checks)?"experimental epoch checks and volatile bypass":"run044 strict/volatile hashes and immutable-copy reuse; stable sampled cache retained");
  REXLOG_INFO("[deko-deps] draw dependencies v2: bounded flat vertex table, exact indexed texture sets, system-source generations; work/frame/guest/image guards retained");
  REXLOG_INFO("[deko-cpu] constant preparation v2: shared system-source generations and lazy content hashes; unchanged packing order and ordered GPU delta commands");
  REXLOG_INFO("[deko-texture] material cache v2: 128 exact sets, last-set fast path and 4-way indexed lookup; all work/frame/guest/image/resolve checks retained");
  REXLOG_INFO("[deko-upload] geometry pre-list v1: {}; fresh ranges only, <=32752-byte commands, immediate snapshots; adds GPU transfer/barrier work outside main-list timestamp samples (startup A/B)",p.geometry_push_upload);
  REXLOG_INFO("[deko-upload] zero prefix v2: initialize per fenced slot/mapping; reinitialize after protected range grows");
  REXLOG_INFO("[deko-abi] storage descriptor v1: explicit zero tail; full geometry range {} MiB; Eden size/address heuristic compatibility",p.stream_bytes>>20);
  REXLOG_INFO("[deko-command] reuse v1: state_cache={} draw_barriers={}; image dependencies tracked separately; constant gap coalescing 64 bytes",REXCVAR_GET(nx_native_state_cache),REXCVAR_GET(nx_native_draw_barriers));
  REXLOG_INFO("[deko-upload] reuse v3: fence-safe GPU index cache (8 MiB/slot); ordered delta constants; index_cache={} delta_constants={}",REXCVAR_GET(nx_native_index_cache),REXCVAR_GET(nx_native_delta_constants));
  REXLOG_INFO("[deko-upload] reuse v2: pooled metadata; register-generation constants; persistent CPU index conversion (24 MiB, exact source checks)");
  REXLOG_INFO("[deko-validation] Dante cache v3 local fallback: {}; 1/8 rotating geometry samples; first 20000 matching samples guarded, then no recurring full guards; a missed change quarantines only overlapping physical ranges; texture scheduling reported separately; native_validation_cache=false restores full per-use checks",REXCVAR_GET(nx_native_validation_cache));
  REXLOG_INFO("[deko-geometry] persistent cache v1: {}; 16 MiB per upload slot, {} slots; admission after two observations; volatile policy follows geometry epoch setting for 64 frames",p.geometry_cache_enabled,kWorkSlots);
  REXLOG_INFO("[deko-geometry] dynamic upload v3: selective exact windows; rebased fetch addresses; unchanged shader ABI and GPU fence ownership");
  REXLOG_INFO("[deko-upload] reuse v1: O(1) candidate LRU; exact work-local index and constant copies; bulk index uploads");
  return true;
}
void Renderer::Shutdown(){
  if(!impl_||!impl_->textures)return;auto& p=*impl_;p.Flush();p.core.EsperarGpu();
  p.frontbuffer={};p.setup_cached=false;
  p.Begin();for(auto& [k,t]:p.render_targets)p.textures->FreeImage(t->image);p.render_targets.clear();
  for(auto& [k,t]:p.resolves)p.textures->FreeImage(t->image);p.resolves.clear();
  for(auto& [k,t]:p.crops)p.textures->FreeImage(t.image);p.crops.clear();
  p.textures->Shutdown();p.Flush();p.core.EsperarGpu();p.textures.reset();
  p.provider->dispositivo().DestruirBloque(p.constant_memory);p.constant_memory=nullptr;p.constant_memory_base={};
  p.core.Destruir();p.initialized=false;
}
void Renderer::LoadShader(xenos::ShaderType type,const uint32_t* code,uint32_t words,uint32_t) {
  ProfileScope total(stats_.cpu_ns,impl_->cpu_profile_enabled);
  ProfileScope timer(impl_->profile.load_shader_ns,impl_->cpu_profile_enabled);
  // Hash complete microcode even inside a frame; executable guest memory may be reused.
  uint64_t hash=XXH3_64bits_withSeed(code,size_t(words)*4,uint64_t(type));
  auto& item=impl_->shaders[hash];if(!item)item=std::make_unique<SpirvShader>(type,hash,code,words);
  if(type==xenos::ShaderType::kVertex)impl_->active_vs=item.get();else impl_->active_ps=item.get();
}
void Renderer::Draw(const uint32_t* indices,uint32_t words){
  ProfileScope total(stats_.cpu_ns,impl_->cpu_profile_enabled);
  impl_->SyncGuestMemory();
  impl_->queue_work.Changed();
  ProfileScope timer(impl_->profile.draw_ns,impl_->cpu_profile_enabled);impl_->Draw(indices,words);
}
void Renderer::Resolve(){
  ProfileScope total(stats_.cpu_ns,impl_->cpu_profile_enabled);
  impl_->SyncGuestMemory();
  impl_->queue_work.Changed();
  ProfileScope timer(impl_->profile.resolve_ns,impl_->cpu_profile_enabled);impl_->Resolve();
}
void Renderer::Swap(rex::ui::Presenter* presenter,uint32_t front,uint32_t width,uint32_t height){
  {
    ProfileScope total(stats_.cpu_ns,impl_->cpu_profile_enabled);
    impl_->SyncGuestMemory();
    impl_->queue_work.Changed();
    ProfileScope timer(impl_->profile.swap_ns,impl_->cpu_profile_enabled);impl_->Swap(presenter,front,width,height);
  }
  impl_->FinishProfileFrame();
}
void Renderer::Flush(){impl_->Flush();}
void Renderer::KickRecorder(){impl_->Kick();}
void Renderer::InitializeDriverCache(const std::filesystem::path& root,uint32_t title) {
  auto& p=*impl_;
  p.cache_root=root/"fear-deko-dksh-v1";
  p.prepared_cache_root=root/"fear-deko-prepared-v1";
  p.warmup_root=root/"fear-deko-warmup-v1"/fmt::format("{:08X}",title);
  p.WarmupObservedShaders();
}
void Renderer::SetGammaRamp256(const uint32_t* entries){memcpy(impl_->gamma_ramp,entries,sizeof(impl_->gamma_ramp));}
std::string Renderer::TakeTextureUploadReport(){return {};}

bool Renderer::CpuProfilingEnabled() const {return impl_->cpu_profile_enabled;}
void Renderer::InvalidateGuestMemory(GuestMemoryBoundary reason) {
  impl_->guest_boundary_counts[size_t(reason)].fetch_add(1,std::memory_order_relaxed);
  impl_->guest_memory_epoch.fetch_add(1,std::memory_order_release);
}
void Renderer::InvalidateVertexReuse() {
  impl_->vertex_reuse_epoch.fetch_add(1,std::memory_order_release);
}
void Renderer::InvalidateExtentWrite(uint32_t address) {
  ProfileScope timer(impl_->profile.range_write_ns,impl_->cpu_profile_enabled);
  impl_->validation_cache.guest_writes().RecordExtent(address);
}
void Renderer::ReportProfile(uint64_t frames) {
  auto& p=*impl_;frames=std::max(frames,uint64_t(1));
  p.profile.index_validation_candidates=p.index_validation.stats().candidates;
  p.profile.index_validation_hits=p.index_validation.stats().hits;
  p.profile.index_validation_rejected=p.index_validation.stats().rejected;
  p.profile.vertex_fast_hits=p.shared_vertices.fast_hits();p.profile.vertex_table_lookups=p.shared_vertices.table_lookups();
  p.profile.index_fast_hits=p.shared_indices.fast_hits();p.profile.index_table_lookups=p.shared_indices.table_lookups();
  const auto& a=p.profile;const auto& b=p.reported_profile;
  fear_nx::cpu::Report();
  REXLOG_INFO("[deko-front] last interval: {} deferred, {} direct presents, {} materialized, {} whole-clear exchanges, {} superseded; {:.3f} deferred / {:.3f} materialized Mpixels/frame",
      a.front_deferred-b.front_deferred,a.front_direct-b.front_direct,a.front_copies-b.front_copies,
      a.front_exchanges-b.front_exchanges,a.front_replaced-b.front_replaced,
      double(a.front_deferred_pixels-b.front_deferred_pixels)/1e6/frames,
      double(a.front_copy_pixels-b.front_copy_pixels)/1e6/frames);
  const auto ms=[frames](uint64_t current,uint64_t previous){return double(current-previous)/1e6/double(frames);};
  const auto mib=[frames](uint64_t current,uint64_t previous){return double(current-previous)/1048576.0/double(frames);};
  REXLOG_INFO("[deko-cpu] fast tables: vertices {} hits / {} map lookups; indices {} hits / {} map lookups; constant packing source {:.3f} used / {:.3f} bank MiB/frame (coverage, not bytes read)",
      a.vertex_fast_hits-b.vertex_fast_hits,a.vertex_table_lookups-b.vertex_table_lookups,
      a.index_fast_hits-b.index_fast_hits,a.index_table_lookups-b.index_table_lookups,
      mib(a.constant_pack_used_bytes,b.constant_pack_used_bytes),mib(a.constant_pack_bank_bytes,b.constant_pack_bank_bytes));
  const auto& validation=p.validation_cache.stats();
  const auto previous_validation=p.reported_validation_stats;
  const auto& writes=p.validation_cache.guest_writes().stats();
  const auto& old_writes=p.reported_guest_writes;
  REXLOG_INFO("[deko-write-range] interval: {} extent writes ({:.2f}/frame), {:.3f} ms/frame journal; {} lazy queries, {} overlapping leases; {} addresses retained, {} history resets, {} conservative history misses",
      writes.writes-old_writes.writes,double(writes.writes-old_writes.writes)/frames,
      ms(a.range_write_ns,b.range_write_ns),writes.queries-old_writes.queries,writes.overlaps-old_writes.overlaps,
      p.validation_cache.guest_writes().size(),writes.history_resets-old_writes.history_resets,
      writes.history_misses-old_writes.history_misses);
  REXLOG_INFO("[deko-write-range] history v2: {} oldest-address evictions; {} exact recent queries (at most 8 writes); only older discarded history needs a conservative recheck",
      writes.history_evictions-old_writes.history_evictions,writes.recent_queries-old_writes.recent_queries);
  p.reported_guest_writes=writes;
  std::array<uint64_t,size_t(GuestMemoryBoundary::kCount)> boundary_delta{};
  uint64_t global_requests=0;
  for(size_t i=0;i<boundary_delta.size();++i) {
    const uint64_t current=p.guest_boundary_counts[i].load(std::memory_order_relaxed);
    boundary_delta[i]=current-p.reported_guest_boundary_counts[i];
    p.reported_guest_boundary_counts[i]=current;global_requests+=boundary_delta[i];
  }
  const uint64_t ownership=p.vertex_reuse_epoch.load(std::memory_order_acquire);
  const uint64_t local_requests=ownership-p.reported_vertex_reuse_epoch;
  const auto reason=[&](GuestMemoryBoundary kind){return boundary_delta[size_t(kind)];};
  REXLOG_INFO("[deko-epochs] interval: {} vertex-only coherency waits ({:.2f}/frame), {} global requests ({:.2f}/frame); applied local {} / global {}; global reasons write={} interrupt={} memory-wait={} register-wait={} extent={} occlusion={}",
      local_requests,double(local_requests)/frames,global_requests,double(global_requests)/frames,
      p.vertex_reuse_boundaries-p.reported_vertex_reuse_boundaries,p.guest_revalidations-p.reported_guest_revalidations,
      reason(GuestMemoryBoundary::kWrite),reason(GuestMemoryBoundary::kInterrupt),
      reason(GuestMemoryBoundary::kMemoryWait),reason(GuestMemoryBoundary::kRegisterWait),
      reason(GuestMemoryBoundary::kExtentWrite),reason(GuestMemoryBoundary::kOcclusionWrite));
  p.reported_vertex_reuse_epoch=ownership;
  p.reported_vertex_reuse_boundaries=p.vertex_reuse_boundaries;
  p.reported_guest_revalidations=p.guest_revalidations;
  REXLOG_INFO("[deko-validation] last 10 s: {} lookups, {} memo hits, {} full checks, {:.1f} full MiB/frame, {:.2f} sample MiB/frame; {} sample changes, {} guards, {} guard misses, {} invalidations",
      validation.lookups-previous_validation.lookups,validation.hits-previous_validation.hits,
      validation.full_checks-previous_validation.full_checks,mib(validation.full_bytes,previous_validation.full_bytes),
      mib(validation.sample_bytes,previous_validation.sample_bytes),validation.sample_changes-previous_validation.sample_changes,
      validation.guard_checks-previous_validation.guard_checks,validation.guard_misses-previous_validation.guard_misses,
      validation.invalidations-previous_validation.invalidations);
  REXLOG_INFO("[deko-validation] Dante policy: {} sampled checks, {} epoch hits, {} volatile bypasses, {} texture deferrals; {} training checks remain, sampling disabled={}",
      validation.sampled_checks-previous_validation.sampled_checks,validation.epoch_hits-previous_validation.epoch_hits,
      validation.volatile_bypasses-previous_validation.volatile_bypasses,validation.texture_deferred-previous_validation.texture_deferred,
      validation.training_remaining,validation.sampling_disabled);
  REXLOG_INFO("[deko-validation] local fallback: {} quarantined ranges, {} strict lookups, {:.2f} strict MiB/frame; {} quarantine overflows",
      validation.quarantined_ranges,validation.strict_lookups-previous_validation.strict_lookups,
      mib(validation.strict_bytes,previous_validation.strict_bytes),validation.quarantine_overflows-previous_validation.quarantine_overflows);
  REXLOG_INFO("[deko-geometry] dynamic validation: {} exact checks, {:.2f} hashed MiB/frame; {} immutable-copy hits, {:.2f} avoided copy MiB/frame, {:.2f} actual dynamic copy MiB/frame",
      validation.volatile_exact_checks-previous_validation.volatile_exact_checks,
      mib(validation.volatile_full_bytes,previous_validation.volatile_full_bytes),
      a.dynamic_reuse_hits-b.dynamic_reuse_hits,mib(a.dynamic_reuse_bytes,b.dynamic_reuse_bytes),
      mib(a.dynamic_copy_bytes,b.dynamic_copy_bytes));
  REXLOG_INFO("[deko-geometry] fetch windows: {:.2f} declared -> {:.2f} stable window MiB/frame; {} clipped fetches, {} conservative fallbacks",
      mib(a.vertex_declared_bytes,b.vertex_declared_bytes),mib(a.vertex_window_bytes,b.vertex_window_bytes),
      a.vertex_clipped-b.vertex_clipped,a.vertex_range_fallbacks-b.vertex_range_fallbacks);
  REXLOG_INFO("[deko-geometry] vertex grid: {} smaller windows; {:.3f} selected MiB/frame below legacy; {:.3f} copy-miss MiB/frame below legacy (same misses, not net cache savings)",
      a.vertex_grid_reduced-b.vertex_grid_reduced,mib(a.vertex_grid_selected_saved,b.vertex_grid_selected_saved),
      mib(a.vertex_grid_copy_saved,b.vertex_grid_copy_saved));
  REXLOG_INFO("[deko-geometry] work-local copies: {} hits, {} misses; {:.2f} uncopied padding MiB/frame",
      a.vertex_local_hits-b.vertex_local_hits,a.vertex_local_misses-b.vertex_local_misses,
      mib(a.vertex_padding_bytes,b.vertex_padding_bytes));
  p.reported_validation_stats=validation;
  const auto& geometry=p.shared_geometry_enabled?p.shared_vertices.stats():p.vertex_cache.stats();const auto& old_geometry=p.reported_vertex_cache_stats;
  REXLOG_INFO("[deko-geometry] last 10 s: {} persistent hits, {} misses, {:.2f} cached copy MiB/frame, {} evictions, {} capacity misses, {} candidate evictions, {} full-slot retries",
      geometry.hits-old_geometry.hits,geometry.misses-old_geometry.misses,mib(geometry.copied_bytes,old_geometry.copied_bytes),
      geometry.evictions-old_geometry.evictions,geometry.capacity_misses-old_geometry.capacity_misses,
      geometry.candidate_evictions-old_geometry.candidate_evictions,p.transient_work_fallbacks-p.reported_transient_work_fallbacks);
  p.reported_vertex_cache_stats=geometry;p.reported_transient_work_fallbacks=p.transient_work_fallbacks;
  if(p.cpu_profile_enabled) {
    REXLOG_INFO("[deko-perf] wall ms/frame (inclusive): draw {:.3f}, resolve {:.3f}, swap {:.3f}, shader load {:.3f}",
        ms(a.draw_ns,b.draw_ns),ms(a.resolve_ns,b.resolve_ns),ms(a.swap_ns,b.swap_ns),ms(a.load_shader_ns,b.load_shader_ns));
    REXLOG_INFO("[deko-perf] draw phases ms/frame: begin {:.3f}, setup {:.3f}, indices {:.3f}, vertices {:.3f}, constants {:.3f}, textures {:.3f}, commands {:.3f}, mirror clears {:.3f}",
        ms(a.draw_begin_ns,b.draw_begin_ns),ms(a.setup_ns,b.setup_ns),ms(a.index_ns,b.index_ns),ms(a.vertex_ns,b.vertex_ns),
        ms(a.constants_ns,b.constants_ns),ms(a.texture_ns,b.texture_ns),ms(a.record_ns,b.record_ns),ms(a.mirror_ns,b.mirror_ns));
    REXLOG_INFO("[deko-perf] vertex detail: hash {:.3f} ms/frame, {:.1f} MiB/frame, {:.1f} calls/frame; copy {:.3f} ms/frame",
        ms(a.vertex_hash_ns,b.vertex_hash_ns),mib(a.vertex_hash_bytes,b.vertex_hash_bytes),double(a.vertex_hash_calls-b.vertex_hash_calls)/frames,ms(a.vertex_copy_ns,b.vertex_copy_ns));
    REXLOG_INFO("[deko-geometry] vertex batch: {} payloads -> {} writes, {} direct; {:.3f} payload / {:.3f} written / {:.3f} reserved MiB/frame; capture {:.3f} ms/frame (includes mid-batch drains), all drains {:.3f}, pre-submit {:.3f}; total copy work {:.3f}",
        a.vertex_batch_payloads-b.vertex_batch_payloads,a.vertex_batch_writes-b.vertex_batch_writes,a.vertex_direct_writes-b.vertex_direct_writes,
        mib(a.vertex_batch_bytes,b.vertex_batch_bytes),mib(a.vertex_batch_write_bytes,b.vertex_batch_write_bytes),mib(a.vertex_batch_reserved,b.vertex_batch_reserved),
        ms(a.vertex_batch_capture_ns,b.vertex_batch_capture_ns),ms(a.vertex_batch_write_ns,b.vertex_batch_write_ns),ms(a.vertex_batch_submit_ns,b.vertex_batch_submit_ns),
        ms(a.vertex_copy_ns+a.vertex_batch_capture_ns+a.vertex_batch_submit_ns+a.vertex_resident_capture_ns+a.vertex_resident_submit_ns,
           b.vertex_copy_ns+b.vertex_batch_capture_ns+b.vertex_batch_submit_ns+b.vertex_resident_capture_ns+b.vertex_resident_submit_ns));
    REXLOG_INFO("[deko-geometry] resident vertex batch: {} payloads -> {} writes; {:.3f} payload / {:.3f} written MiB/frame; capture {:.3f} ms/frame (includes mid-batch drains), all drains {:.3f}, pre-submit {:.3f}",
        a.vertex_resident_payloads-b.vertex_resident_payloads,a.vertex_resident_writes-b.vertex_resident_writes,
        mib(a.vertex_resident_payload_bytes,b.vertex_resident_payload_bytes),mib(a.vertex_resident_written_bytes,b.vertex_resident_written_bytes),
        ms(a.vertex_resident_capture_ns,b.vertex_resident_capture_ns),ms(a.vertex_resident_drain_ns,b.vertex_resident_drain_ns),ms(a.vertex_resident_submit_ns,b.vertex_resident_submit_ns));
    REXLOG_INFO("[deko-geometry] vertex placement: {} sequential / {} best-fit allocations; {} resident payloads -> {} writes",
        p.shared_vertices.sequential_allocations()-p.reported_vertex_sequential,
        p.shared_vertices.best_fit_allocations()-p.reported_vertex_best_fit,
        a.vertex_resident_payloads-b.vertex_resident_payloads,a.vertex_resident_writes-b.vertex_resident_writes);
    p.reported_vertex_sequential=p.shared_vertices.sequential_allocations();
    p.reported_vertex_best_fit=p.shared_vertices.best_fit_allocations();
    REXLOG_INFO("[deko-geometry] direct vertex writes: resident {:.3f} ms/frame ({} writes, {:.3f} MiB/frame); transient {:.3f} ms/frame ({} writes, {:.3f} MiB/frame); {:.3f} written MiB/frame; {} unaligned sources, {} partial-vector tails",
        ms(a.vertex_direct_resident_ns,b.vertex_direct_resident_ns),a.vertex_direct_resident_writes-b.vertex_direct_resident_writes,mib(a.vertex_direct_resident_bytes,b.vertex_direct_resident_bytes),
        ms(a.vertex_direct_transient_ns,b.vertex_direct_transient_ns),a.vertex_direct_transient_writes-b.vertex_direct_transient_writes,mib(a.vertex_direct_transient_bytes,b.vertex_direct_transient_bytes),
        mib(a.vertex_direct_written_bytes,b.vertex_direct_written_bytes),a.vertex_direct_unaligned_sources-b.vertex_direct_unaligned_sources,a.vertex_direct_tails-b.vertex_direct_tails);
    REXLOG_INFO("[deko-upload] index validation reuse: {} exact-verified hits / {} candidates, {} rejected hints, {} full hashes; probe {:.3f} ms/frame, hash/regular lookup {:.3f}; CPU-cache probe time may overlap",
        a.index_validation_hits-b.index_validation_hits,a.index_validation_candidates-b.index_validation_candidates,a.index_validation_rejected-b.index_validation_rejected,
        a.index_full_hashes-b.index_full_hashes,ms(a.index_validation_probe_ns,b.index_validation_probe_ns),ms(a.index_hash_ns,b.index_hash_ns));
    REXLOG_INFO("[deko-upload] index compact16: {} checked conversions, {} packed, {} retained Uint32; {} expanded-path draws; pack {:.3f} ms/frame (inside conversion)",
        a.index_compact_attempts-b.index_compact_attempts,a.index_compact_accepted-b.index_compact_accepted,a.index_compact_rejected-b.index_compact_rejected,
        a.index_compact_draws-b.index_compact_draws,ms(a.index_compact_ns,b.index_compact_ns));
    REXLOG_INFO("[deko-upload] index reuse: {} hits, {} misses, {} unretained; {:.2f} copied MiB/frame, {:.2f} avoided; exact input lookup {:.3f} ms/frame ({:.2f} MiB), conversion {:.3f}, upload {:.3f}",
        a.index_hits-b.index_hits,a.index_misses-b.index_misses,a.index_uncached-b.index_uncached,
        mib(a.index_copy_bytes,b.index_copy_bytes),mib(a.index_avoided_bytes,b.index_avoided_bytes),
        ms(a.index_hash_ns,b.index_hash_ns),mib(a.index_hash_bytes,b.index_hash_bytes),
        ms(a.index_convert_ns,b.index_convert_ns),ms(a.index_copy_ns,b.index_copy_ns));
    REXLOG_INFO("[deko-upload] small index batch: {} payloads -> {} writes, {} direct writes; {:.3f} payload / {:.3f} written / {:.3f} reserved KiB/frame; capture {:.3f} ms/frame (includes mid-batch drains), all drains {:.3f}, pre-submit drains {:.3f}",
        a.index_batch_payloads-b.index_batch_payloads,a.index_batch_writes-b.index_batch_writes,a.index_direct_writes-b.index_direct_writes,
        mib(a.index_batch_payload_bytes,b.index_batch_payload_bytes)*1024,mib(a.index_batch_write_bytes,b.index_batch_write_bytes)*1024,
        mib(a.index_batch_reserve_bytes,b.index_batch_reserve_bytes)*1024,ms(a.index_batch_capture_ns,b.index_batch_capture_ns),
        ms(a.index_batch_write_ns,b.index_batch_write_ns),ms(a.index_batch_submit_ns,b.index_batch_submit_ns));
    REXLOG_INFO("[deko-upload] index upload total: {:.3f} ms/frame (direct + capture + pre-submit; all-drains counter overlaps these)",
        ms(a.index_copy_ns+a.index_batch_capture_ns+a.index_batch_submit_ns+a.index_resident_capture_ns+a.index_resident_submit_ns,
           b.index_copy_ns+b.index_batch_capture_ns+b.index_batch_submit_ns+b.index_resident_capture_ns+b.index_resident_submit_ns));
    REXLOG_INFO("[deko-upload] resident index batch: {} payloads -> {} writes; {:.3f} payload / {:.3f} written KiB/frame; capture {:.3f} ms/frame (includes mid-batch drains), all drains {:.3f}, pre-submit {:.3f}",
        a.index_resident_payloads-b.index_resident_payloads,a.index_resident_writes-b.index_resident_writes,
        mib(a.index_resident_payload_bytes,b.index_resident_payload_bytes)*1024,mib(a.index_resident_written_bytes,b.index_resident_written_bytes)*1024,
        ms(a.index_resident_capture_ns,b.index_resident_capture_ns),ms(a.index_resident_drain_ns,b.index_resident_drain_ns),ms(a.index_resident_submit_ns,b.index_resident_submit_ns));
    REXLOG_INFO("[deko-upload] direct index writes: resident {:.3f} / transient {:.3f} ms/frame; {:.3f} payload / {:.3f} written KiB/frame; {} unaligned sources, {} partial-vector tails",
        ms(a.index_direct_resident_ns,b.index_direct_resident_ns),ms(a.index_direct_transient_ns,b.index_direct_transient_ns),
        mib(a.index_direct_bytes,b.index_direct_bytes)*1024,mib(a.index_direct_written_bytes,b.index_direct_written_bytes)*1024,
        a.index_direct_unaligned_sources-b.index_direct_unaligned_sources,a.index_direct_tails-b.index_direct_tails);
    const auto& gathered=p.deferred_index_uploads.stats();
    REXLOG_INFO("[deko-upload] index ordered: {} flushes ({} capacity); {} payloads -> {} writes, {} adjacent joins; {} sequential / {} best-fit allocations",
        gathered.flushes-p.reported_deferred_indices.flushes,gathered.capacity_flushes-p.reported_deferred_indices.capacity_flushes,
        gathered.payloads-p.reported_deferred_indices.payloads,gathered.writes-p.reported_deferred_indices.writes,gathered.merged-p.reported_deferred_indices.merged,
        p.shared_indices.sequential_allocations()-p.reported_index_sequential,p.shared_indices.best_fit_allocations()-p.reported_index_best_fit);
    REXLOG_INFO("[deko-upload] index assembly: {} sorted flushes; {:.3f} KiB/frame copied through join scratch",
        gathered.sorted_flushes-p.reported_deferred_indices.sorted_flushes,
        mib(gathered.assembled_bytes,p.reported_deferred_indices.assembled_bytes)*1024);
    p.reported_deferred_indices=gathered;p.reported_index_sequential=p.shared_indices.sequential_allocations();p.reported_index_best_fit=p.shared_indices.best_fit_allocations();
    REXLOG_INFO("[deko-upload] resident index write sizes: <=256 bytes {} writes / {:.3f} KiB/frame / {:.3f} ms/frame; <=4096 bytes {} / {:.3f} / {:.3f}; larger {} / {:.3f} / {:.3f}",
        a.index_write_size_count[0]-b.index_write_size_count[0],mib(a.index_write_size_bytes[0],b.index_write_size_bytes[0])*1024,ms(a.index_write_size_ns[0],b.index_write_size_ns[0]),
        a.index_write_size_count[1]-b.index_write_size_count[1],mib(a.index_write_size_bytes[1],b.index_write_size_bytes[1])*1024,ms(a.index_write_size_ns[1],b.index_write_size_ns[1]),
        a.index_write_size_count[2]-b.index_write_size_count[2],mib(a.index_write_size_bytes[2],b.index_write_size_bytes[2])*1024,ms(a.index_write_size_ns[2],b.index_write_size_ns[2]));
    REXLOG_INFO("[deko-upload] constant reuse: {} hits, {} misses, {} unretained; {:.2f} copied MiB/frame, {:.2f} avoided; exact lookup {:.3f} ms/frame, upload {:.3f}; geometry cache management {:.3f} ms/frame",
        a.constant_hits-b.constant_hits,a.constant_misses-b.constant_misses,a.constant_uncached-b.constant_uncached,
        mib(a.constant_copy_bytes,b.constant_copy_bytes),mib(a.constant_avoided_bytes,b.constant_avoided_bytes),
        ms(a.constant_lookup_ns,b.constant_lookup_ns),ms(a.constant_copy_ns,b.constant_copy_ns),ms(a.vertex_cache_ns,b.vertex_cache_ns));
    REXLOG_INFO("[deko-upload] CPU index conversion cache: {} hits, {} misses, {} bounded resets; lookup {:.3f} ms/frame; {} entries, {:.2f} MiB retained",
        a.index_cpu_hits-b.index_cpu_hits,a.index_cpu_misses-b.index_cpu_misses,a.index_cpu_resets-b.index_cpu_resets,
        ms(a.index_cpu_lookup_ns,b.index_cpu_lookup_ns),p.converted_indices.size(),double(p.converted_indices.shadow_bytes())/1048576.0);
    REXLOG_INFO("[deko-upload] constant preparation: {} generation hits, {} packed rebuilds, {} system snapshot hits; packing {:.3f} ms/frame; upload-cache reset {:.3f} ms/frame",
        a.constant_generation_hits-b.constant_generation_hits,a.constant_packs-b.constant_packs,a.system_pack_reuses-b.system_pack_reuses,
        ms(a.constant_pack_ns,b.constant_pack_ns),ms(a.upload_cache_clear_ns,b.upload_cache_clear_ns));
    REXLOG_INFO("[deko-upload] GPU index residency: {} hits, {} misses; {:.2f} avoided MiB/frame; lookup {:.3f} ms/frame; cumulative evictions {} capacity misses {}",
        a.index_gpu_hits-b.index_gpu_hits,a.index_gpu_misses-b.index_gpu_misses,
        mib(a.index_gpu_avoided_bytes,b.index_gpu_avoided_bytes),ms(a.index_gpu_lookup_ns,b.index_gpu_lookup_ns),
        (p.shared_geometry_enabled?p.shared_indices.stats():p.index_gpu_cache.stats()).evictions,
        (p.shared_geometry_enabled?p.shared_indices.stats():p.index_gpu_cache.stats()).capacity_misses);
    REXLOG_INFO("[deko-upload] constant deltas: {:.3f} initial MiB/frame, {:.3f} pushed MiB/frame, {:.1f} push commands/frame, {} unchanged shadows",
        mib(a.constant_initial_bytes,b.constant_initial_bytes),mib(a.constant_push_bytes,b.constant_push_bytes),
        double(a.constant_push_commands-b.constant_push_commands)/frames,a.constant_delta_unchanged-b.constant_delta_unchanged);
    REXLOG_INFO("[deko-command] state: {} emitted groups, {} skipped groups; draw barriers {} full, {} data invalidations, {} omitted",
        a.state_emitted-b.state_emitted,a.state_skipped-b.state_skipped,a.draw_full-b.draw_full,
        a.draw_data_invalidations-b.draw_data_invalidations,a.draw_no_barrier-b.draw_no_barrier);
    REXLOG_INFO("[deko-shared] constants: initial copy {:.3f} ms/frame, delta scan {:.3f}, push recording {:.3f}; {} layouts, {} shader layouts shared (cumulative)",
        ms(a.constant_initial_ns,b.constant_initial_ns),ms(a.constant_scan_ns,b.constant_scan_ns),ms(a.constant_push_ns,b.constant_push_ns),p.constant_layouts.size(),p.constant_layouts.shared_layouts());
    REXLOG_INFO("[deko-constants] dirty envelopes: {} narrow scans; {:.3f} scanned / {:.3f} skipped MiB/frame; older slot revisions retain full scans",
        a.constant_narrow_scans-b.constant_narrow_scans,mib(a.constant_scan_extent_bytes,b.constant_scan_extent_bytes),mib(a.constant_scan_skipped_bytes,b.constant_scan_skipped_bytes));
    REXLOG_INFO("[deko-shared] texture sets: {} hits, {} misses, {} ineligible; geometry cross-work hits {} cross-slot hits {}, index cross-work hits {} (cumulative)",
        a.texture_set_hits-b.texture_set_hits,a.texture_set_misses-b.texture_set_misses,a.texture_set_ineligible-b.texture_set_ineligible,
        p.shared_vertices.cross_work_hits(),p.shared_vertices.cross_slot_hits(),p.shared_indices.cross_work_hits());
    REXLOG_INFO("[deko-msaa] guest draw layouts: 1x {} 2x {} 4x {} unknown {}; host attachments 1 sample; max target extent {}x{} (session)",
        a.guest_samples[0]-b.guest_samples[0],a.guest_samples[1]-b.guest_samples[1],a.guest_samples[2]-b.guest_samples[2],
        a.guest_samples[3]-b.guest_samples[3],a.max_target_width,a.max_target_height);
    REXLOG_INFO("[deko-constants] isolated ring: {}; transient high-water {} KiB/slot, {} overflow retries (session)",
        p.constant_memory!=nullptr,p.constant_upload_peak>>10,p.constant_upload_overflows);
    const auto& t=p.textures->GetProfile();const auto& old=p.reported_texture_profile;
    REXLOG_INFO("[deko-perf] texture detail: guest cache lookup {:.3f} ms/frame, {:.1f} calls/frame; hash {:.3f} ms/frame, {:.1f} MiB/frame; decode/upload {:.3f} ms/frame, {:.1f} staging MiB/frame",
        ms(t.get_texture_ns,old.get_texture_ns),double(t.get_texture_calls-old.get_texture_calls)/frames,
        ms(t.content_hash_ns,old.content_hash_ns),mib(t.content_hash_bytes,old.content_hash_bytes),
        ms(t.decode_upload_ns,old.decode_upload_ns),mib(t.decode_upload_bytes,old.decode_upload_bytes));
    REXLOG_INFO("[deko-texture] binding memo: {} hits, {} misses; exact fetch/sampler key, same validation epoch, per-image identity/version and guard invalidation",
        t.binding_hits-old.binding_hits,t.binding_misses-old.binding_misses);
    REXLOG_INFO("[deko-texture] scheduled direct reuse: {} bindings; no guest translation/state copy for an already-proven frame/deadline; due/overlap checks unchanged",
        t.scheduled_binding_reuses-old.scheduled_binding_reuses);
    REXLOG_INFO("[deko-perf] resource detail ms/frame: images {:.3f} ({:.1f} calls/frame), descriptors {:.3f} ({:.1f}), samplers {:.3f} ({:.1f}); all callers, nested in phases above",
        ms(t.image_alloc_ns,old.image_alloc_ns),double(t.image_alloc_calls-old.image_alloc_calls)/frames,
        ms(t.descriptor_ns,old.descriptor_ns),double(t.descriptor_calls-old.descriptor_calls)/frames,
        ms(t.sampler_ns,old.sampler_ns),double(t.sampler_calls-old.sampler_calls)/frames);
    REXLOG_INFO("[deko-perf] queue/present detail ms/frame: begin+fence {:.3f}, actual slot fence {:.3f}, cleanup {:.3f}, submit {:.3f}, swap prepare {:.3f}, refresh incl callback {:.3f}; full barriers {:.1f}/frame",
        ms(a.begin_ns,b.begin_ns),ms(p.core.NsEsperandoValla(),p.reported_fence_ns),ms(a.maintenance_ns,b.maintenance_ns),
        ms(a.submit_ns,b.submit_ns),ms(a.swap_prepare_ns,b.swap_prepare_ns),ms(a.refresh_ns,b.refresh_ns),double(a.barriers-b.barriers)/frames);
    REXLOG_INFO("[deko-queue] partial {:.2f} / final {:.2f} sends/frame; {} empty kicks skipped; {} blocked / {} slot probes; blocked {:.3f}, already-ready {:.3f} ms/frame",
        double(a.queue_partial_sends-b.queue_partial_sends)/frames,double(a.queue_final_sends-b.queue_final_sends)/frames,
        a.queue_empty_kicks-b.queue_empty_kicks,a.queue_slot_blocked-b.queue_slot_blocked,a.queue_slot_checks-b.queue_slot_checks,
        ms(a.queue_blocked_ns,b.queue_blocked_ns),ms(a.queue_ready_ns,b.queue_ready_ns));
    const auto blocked=a.queue_slot_blocked-b.queue_slot_blocked,probes=a.queue_slot_checks-b.queue_slot_checks;
    REXLOG_INFO("[deko-queue] slot reuse age {:.3f} ms/probe; blocked slots averaged {:.1f} draws / {:.1f} constant pushes; ring scheduled CPU {:.3f} ms/frame, begin scheduled CPU {:.3f} ({} samples; includes cleanup, Eden uses emulated ticks)",
        probes?double(a.queue_reuse_age_ns-b.queue_reuse_age_ns)/1e6/double(probes):0.,
        blocked?double(a.queue_wait_draws-b.queue_wait_draws)/double(blocked):0.,
        blocked?double(a.queue_wait_pushes-b.queue_wait_pushes)/double(blocked):0.,
        ms(a.ring_cpu_ns,b.ring_cpu_ns),ms(a.queue_begin_cpu_ns,b.queue_begin_cpu_ns),a.queue_begin_cpu_samples-b.queue_begin_cpu_samples);
    p.reported_texture_profile=t;
  } else REXLOG_INFO("[deko-perf] CPU phase profiling disabled (nx.native_cpu_profile=false)");
  const auto samples=p.gpu_profile.gpu_submissions-p.reported_gpu_samples;
  if(p.gpu_profile_enabled) {
    if(samples)REXLOG_INFO("[deko-perf] GPU sampled command-list span: {:.3f} ms/sample, {} completed samples, {} invalid; jittered 1/16 work items, not full-frame GPU time or utilization",
        double(p.gpu_profile.gpu_ns-p.reported_gpu_ns)/1e6/double(samples),samples,p.gpu_profile.rejected_samples-p.reported_gpu_rejected);
    else REXLOG_INFO("[deko-perf] GPU timing: no completed samples yet, {} invalid",p.gpu_profile.rejected_samples-p.reported_gpu_rejected);
    const auto draw_samples=p.gpu_profile.draw_work_samples-p.reported_draw_work_samples;
    REXLOG_INFO("[deko-queue] GPU sample coverage: {:.1f} draws/sample; {} samples with >=64 draws, {:.3f} ms/draw-work sample; spans may include gaps between partial sends",
        samples?double(p.gpu_profile.sampled_draws-p.reported_sample_draws)/double(samples):0.,draw_samples,
        draw_samples?double(p.gpu_profile.draw_work_ns-p.reported_draw_work_ns)/1e6/double(draw_samples):0.);
    p.reported_sample_draws=p.gpu_profile.sampled_draws;p.reported_draw_work_ns=p.gpu_profile.draw_work_ns;p.reported_draw_work_samples=p.gpu_profile.draw_work_samples;
  } else REXLOG_INFO("[deko-perf] GPU timestamp sampling disabled (nx.native_gpu_profile=false); slot fence waits still measured");
  REXLOG_INFO("[deko-image] stage1 counters: index16 {} index32 {}, upload saved {} bytes; clears queued {} materialized {} elided {} executed {}; copies 3D {} ({} pixels, {} compressed targets) transfer {}",
      a.index16_draws-b.index16_draws,a.index32_draws-b.index32_draws,a.index16_saved_bytes-b.index16_saved_bytes,
      a.clears_deferred-b.clears_deferred,a.clears_materialized-b.clears_materialized,a.clears_elided-b.clears_elided,a.clears_executed-b.clears_executed,
      a.copy3d_calls-b.copy3d_calls,a.copy3d_pixels-b.copy3d_pixels,a.copy3d_compressed-b.copy3d_compressed,a.copy_transfer_calls-b.copy_transfer_calls);
  REXLOG_INFO("[deko-image] stage2 counters: fragments {} primitives {} no-op decisions {}; clear rectangles {} masked {} repeated {} capacity drains {}; pixels queued {} executed {}, partial consumes {}",
      a.image_fragments-b.image_fragments,a.image_primitives-b.image_primitives,a.image_barriers_skipped-b.image_barriers_skipped,
      a.clears_rectangles-b.clears_rectangles,a.clears_masked-b.clears_masked,a.clears_repeated-b.clears_repeated,a.clears_capacity_drains-b.clears_capacity_drains,
      a.clear_pixels_queued-b.clear_pixels_queued,a.clear_pixels_executed-b.clear_pixels_executed,a.clear_partial_consumes-b.clear_partial_consumes);
  REXLOG_INFO("[deko-upload] constant slots: {} initialized, {} cross-work reuses ({} unchanged), {} initial CPU bytes avoided, {} reentry push bytes, {} capacity fallback requests",
      a.constant_slot_allocations-b.constant_slot_allocations,a.constant_slot_reuses-b.constant_slot_reuses,a.constant_slot_unchanged-b.constant_slot_unchanged,
      a.constant_slot_initial_avoided-b.constant_slot_initial_avoided,a.constant_slot_reentry_push_bytes-b.constant_slot_reentry_push_bytes,a.constant_slot_fallbacks-b.constant_slot_fallbacks);
  REXLOG_INFO("[deko-image] coalescing: {} regions merged; {} expanded consumes, {} pending pixels materialized early",
      a.clear_regions_merged-b.clear_regions_merged,a.clear_expanded_consumes-b.clear_expanded_consumes,a.clear_expanded_pixels-b.clear_expanded_pixels);
  REXLOG_INFO("[deko-deps] local vertex table: {} first-probe hits, {} other lookups; setup {} hits, {} rebuilds (state {} target {} shader {} signs {}); constants {} register spans skipped / {} checked; texture dependencies {} invalidations; image tracker {} capacity drains",
      p.vertex_copies.fast_hits-p.reported_local_fast,p.vertex_copies.map_lookups-p.reported_local_map,
      a.setup_hits-b.setup_hits,a.setup_misses-b.setup_misses,a.setup_state_misses-b.setup_state_misses,
      a.setup_target_misses-b.setup_target_misses,a.setup_shader_misses-b.setup_shader_misses,a.setup_sign_misses-b.setup_sign_misses,
      a.constant_version_skips-b.constant_version_skips,a.constant_version_checks-b.constant_version_checks,
      a.texture_dependency_misses-b.texture_dependency_misses,p.image_barriers.capacity_drains-p.reported_image_capacity);
  REXLOG_INFO("[deko-constants] patch=059: {} / {} version groups skipped; {} resident packed-revision hits; {} partial packs, {} dense full checks; {:.3f} MiB/frame copied into packed CPU blocks",
      a.constant_group_skips-b.constant_group_skips,a.constant_group_checks-b.constant_group_checks,
      a.constant_revision_hits-b.constant_revision_hits,a.constant_partial_packs-b.constant_partial_packs,
      a.constant_bulk_updates-b.constant_bulk_updates,mib(a.constant_packed_copy_bytes,b.constant_packed_copy_bytes));
  const auto& tc=p.textures->GetCheckStats();const auto& old_tc=p.reported_texture_checks;
  REXLOG_INFO("[deko-texture-check] lookups {} frame hits {} scheduled hits {}; full {} ({:.3f} MiB/frame), samples {} ({:.3f} MiB/frame), sample hits {}; budget deferrals {}; guard checks {} misses {}, training {}/3000, excluded bases {}, sampling disabled={}",
      tc.lookups-old_tc.lookups,tc.frame_hits-old_tc.frame_hits,tc.scheduled_hits-old_tc.scheduled_hits,
      tc.full_checks-old_tc.full_checks,mib(tc.full_bytes,old_tc.full_bytes),tc.sample_checks-old_tc.sample_checks,
      mib(tc.sample_bytes,old_tc.sample_bytes),tc.sample_hits-old_tc.sample_hits,tc.budget_deferrals-old_tc.budget_deferrals,
      tc.guard_checks-old_tc.guard_checks,tc.guard_misses-old_tc.guard_misses,tc.training,tc.excluded_bases,tc.sampling_disabled);
  p.reported_texture_checks=tc;
  REXLOG_INFO("[deko-geometry-check] strict epoch hits {}; volatile unhashed {} calls ({:.3f} MiB/frame), fresh uploads required",
      validation.strict_epoch_hits-previous_validation.strict_epoch_hits,
      validation.volatile_unhashed_calls-previous_validation.volatile_unhashed_calls,
      mib(validation.volatile_unhashed_bytes,previous_validation.volatile_unhashed_bytes));
  const auto& ve=p.vertex_epoch_copies.stats();const auto& old_ve=p.reported_vertex_epoch;
  REXLOG_INFO("[deko-vertex-local] lookups {} hits {} ({:.3f} MiB/frame reused), collisions {} epochs {}; lookup {:.3f} ms/frame; dynamic hits {} ({:.3f} MiB/frame); no hashing on epoch hits",
      ve.lookups-old_ve.lookups,ve.hits-old_ve.hits,mib(ve.bytes,old_ve.bytes),
      ve.collisions-old_ve.collisions,ve.invalidations-old_ve.invalidations,
      ms(a.vertex_epoch_lookup_ns,b.vertex_epoch_lookup_ns),a.vertex_epoch_dynamic_hits-b.vertex_epoch_dynamic_hits,
      mib(a.vertex_epoch_dynamic_bytes,b.vertex_epoch_dynamic_bytes));
  REXLOG_INFO("[deko-vertex-window] exact dynamic selections {}; selected {:.3f} MiB/frame less than stable grid; copied {:.3f} MiB/frame less on actual misses; persistent stable windows retained",
      a.vertex_dynamic_windows-b.vertex_dynamic_windows,mib(a.vertex_dynamic_window_saved,b.vertex_dynamic_window_saved),
      mib(a.vertex_dynamic_copy_saved,b.vertex_dynamic_copy_saved));
  p.reported_vertex_epoch=ve;
  const auto& vp=p.vertex_write_probe.stats();const auto& old_vp=p.reported_vertex_probe;
  const uint64_t probes=vp.samples-old_vp.samples;
  if(probes || vp.allocation_skips!=old_vp.allocation_skips) {
    const auto wall=[&](size_t i){return double(vp.wall_ns[i]-old_vp.wall_ns[i])/1e6/double(std::max(probes,uint64_t(1)));};
    const auto ticks=[&](size_t i){return double(vp.thread_ns[i]-old_vp.thread_ns[i])/1e6/double(std::max(vp.thread_samples[i]-old_vp.thread_samples[i],uint64_t(1)));};
    REXLOG_INFO("[deko-vertex-probe] samples {} mean {:.1f} KiB; wall ms/sample guest->RAM {:.4f}, guest->GPU {:.4f}, RAM->GPU {:.4f}; extra overhead {:.3f} ms/frame (included in vertices, not copy counters), allocation skips {}",
        probes,double(vp.bytes-old_vp.bytes)/1024/double(std::max(probes,uint64_t(1))),wall(0),wall(1),wall(2),
        ms(vp.overhead_ns,old_vp.overhead_ns),vp.allocation_skips-old_vp.allocation_skips);
    REXLOG_INFO("[deko-vertex-probe] thread ms/valid sample guest->RAM {:.4f} (n={}), guest->GPU {:.4f} (n={}), RAM->GPU {:.4f} (n={}); n=0 means unavailable; includes thread-query overhead; GPU labels are CPU upload stores",
        ticks(0),vp.thread_samples[0]-old_vp.thread_samples[0],ticks(1),vp.thread_samples[1]-old_vp.thread_samples[1],
        ticks(2),vp.thread_samples[2]-old_vp.thread_samples[2]);
  }
  p.reported_vertex_probe=vp;
  p.reported_local_fast=p.vertex_copies.fast_hits;p.reported_local_map=p.vertex_copies.map_lookups;
  p.reported_image_capacity=p.image_barriers.capacity_drains;
  REXLOG_INFO("[deko-cpu] constant preparation v2: {} system generation reuses; exact reflected bytes and work ownership preserved",
      a.system_generation_hits-b.system_generation_hits);
  REXLOG_INFO("[deko-cpu] constant hashes: {} computations, {:.3f} MiB/frame; ordered GPU-delta updates use exact shadows without content hashing",
      a.constant_content_hashes-b.constant_content_hashes,mib(a.constant_content_hash_bytes,b.constant_content_hash_bytes));
  const auto& texture_sets=p.draw_textures.stats();
  const auto& old_texture_sets=p.reported_draw_texture_stats;
  REXLOG_INFO("[deko-texture] material cache v2: {} last-set / {} indexed candidates, {} lookups, {} bucket probes, {} plan misses, {} live evictions; dependency checks still apply",
      texture_sets.last_hits-old_texture_sets.last_hits,texture_sets.indexed_hits-old_texture_sets.indexed_hits,
      texture_sets.lookups-old_texture_sets.lookups,texture_sets.probes-old_texture_sets.probes,
      texture_sets.plan_misses-old_texture_sets.plan_misses,texture_sets.evictions-old_texture_sets.evictions);
  p.reported_draw_texture_stats=texture_sets;
  REXLOG_INFO("[deko-upload] geometry pre-list: {} uploads, {} commands, {:.3f} MiB/frame, {} CPU fallbacks; copy timers measure command recording when enabled, compare total frame and fence times",
      a.geometry_push_uploads-b.geometry_push_uploads,a.geometry_push_commands-b.geometry_push_commands,
      mib(a.geometry_push_bytes,b.geometry_push_bytes),a.geometry_push_fallbacks-b.geometry_push_fallbacks);
  REXLOG_INFO("[deko-upload] zero prefix: {} initializations, {} protected reuses, {:.3f} KiB/frame stores avoided",
      a.zero_prefix_writes-b.zero_prefix_writes,a.zero_prefix_reuses-b.zero_prefix_reuses,
      mib(a.zero_prefix_avoided_bytes,b.zero_prefix_avoided_bytes)*1024);
  p.reported_profile=a;p.reported_fence_ns=p.core.NsEsperandoValla();
  p.reported_gpu_ns=p.gpu_profile.gpu_ns;p.reported_gpu_samples=p.gpu_profile.gpu_submissions;p.reported_gpu_rejected=p.gpu_profile.rejected_samples;
}

} // namespace fear_nx::deko
