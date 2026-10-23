// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from Project8Recomp (GPL-3.0), src/game/src/native/; generic Xenos GPU code.
//
// Native renderer for the Switch port. See native_renderer.h for the design.
//
// Parts of this file are ports of ReXGlue's Vulkan backend (src/graphics/vulkan,
// itself from Xenia: Copyright Ben Vanik and the Xenia contributors, BSD-3-Clause,
// see LICENSES/): the system constants (VulkanCommandProcessor::
// UpdateSystemConstantValues), the dynamic state (UpdateDynamicState), the
// shader modifications and pipeline state (VulkanPipelineCache), and the resolve
// rectangle (draw_util::GetResolveInfo). They are what the translated shaders
// expect, so they are kept as close to the original as the native design allows;
// EDRAM storage, fragment shader interlock and tessellation are omitted, with
// explicit alias handling for recognized stencil and zero depth/color clears.

#include "native_renderer.h"
#include "native_resolve_layout.h"
#include "native_stencil_clear.h"
#include "native_depth_color_clear.h"
#include "native_zcull.h"
#include "native_attachment_clear.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <unordered_set>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include <fmt/format.h>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <xxhash.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory/utils.h>
#include <rex/string/buffer.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/pipeline/shader/interpreter.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/vulkan/shader.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/system/xmemory.h>
#include <rex/system/xvideo.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/util.h>

#if defined(__SWITCH__)
extern "C" {
#include <switch/kernel/svc.h>
}
#endif

#include "native_geometry_shaders.h"
#include "native_hash.h"
#include "native_textures.h"

REXCVAR_DECLARE(bool, gpu_specialize_texture_signs);
REXCVAR_DEFINE_BOOL(nx_native_depth24, false, "F.E.A.R. NX",
                    "Store depth in 32 bits per pixel (24-bit depth with stencil, or float "
                    "without) instead of 32-bit float with stencil, which NVIDIA GPUs keep "
                    "in 64");
REXCVAR_DEFINE_BOOL(nx_native_gpu_profile, false, "F.E.A.R. NX",
                    "Log GPU time by render pass and resolve, measured on one frame in 16");
REXCVAR_DEFINE_BOOL(nx_native_record_thread, false, "F.E.A.R. NX",
                    "Make the Vulkan calls of draws on a second thread, so the command ring "
                    "thread prepares the next draw meanwhile");
REXCVAR_DEFINE_BOOL(nx_native_fused_gamma, true, "F.E.A.R. NX",
                    "Apply gamma during final bilinear presentation of 8-bit frontbuffers")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(nx_native_zcull, true, "F.E.A.R. NX",
                    "Allow persistent NVK ZCULL on depth attachments; preserve contents on growth")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(nx_native_shared_resolves, true, "F.E.A.R. NX",
                    "Write validated tiled resolves directly into one shared destination resource")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(nx_native_precise_resolve_barriers, true, "F.E.A.R. NX",
                    "Avoid the broad pre-transfer barrier when a resolve uses compute")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(nx_native_full_clear_load_op, true, "F.E.A.R. NX",
                    "Clear fully covered render areas at attachment load without loading old contents")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(nx_native_msaa, true, "F.E.A.R. NX",
                    "Render with the multisampling the game asks for; off draws every target "
                    "with one sample, which costs the GPU much less memory bandwidth");
REXCVAR_DEFINE_BOOL(nx_native_async_pipelines, true, "F.E.A.R. NX",
                    "Native renderer: compile new pipelines on a worker thread and skip the "
                    "draws that need them until they are ready, instead of stalling the frame");
REXCVAR_DEFINE_BOOL(nx_native_trace_output, false, "F.E.A.R. NX",
                    "Log bounded unique native renderer swap, viewport, and Xenos resolve state for display diagnostics");

namespace fear_nx::native {
namespace shaders {
#include "shaders/vulkan_spirv/apply_gamma_table_cs.h"
#include "shaders/depth_resolve_cs.h"
#include "shaders/depth_to_float_cs.h"
#include "shaders/copy_color_cs.h"
}  // namespace shaders

namespace {

namespace xenos = rex::graphics::xenos;
namespace reg = rex::graphics::reg;
namespace draw_util = rex::graphics::draw_util;
using rex::graphics::RegisterFile;
using rex::graphics::Shader;
using rex::graphics::SpirvShader;
using rex::graphics::SpirvShaderTranslator;
using rex::graphics::vulkan::VulkanShader;
using rex::ui::vulkan::VulkanDevice;

// Four: with three, a single frame of heavy GPU work stalled the ring thread
// at once (30-49 ms waits for a free slot in frames that then dropped).
constexpr uint32_t kSubmissionSlots = 4;
// Vertex, index, constant and texture upload data of the submissions in flight.
constexpr VkDeviceSize kStreamSize = 160u << 20;
// Room at the end of the stream for the fixed ranges of constant bindings.
constexpr VkDeviceSize kStreamGuard = 16u << 10;
// Past the end of the stream, never allocated from but inside the range the
// shaders' vertex fetches may address: a fetch past its copied data (a guest
// index out of range) reads garbage there instead of faulting the GPU's MMU,
// which loses the device for good.
constexpr VkDeviceSize kStreamSlack = 16u << 20;
// Sizes of the constant buffer bindings. The float ones are bound with their
// maximum size and filled up to the shader's count.
constexpr VkDeviceSize kFloatConstantsBytes = 256 * 4 * sizeof(float);
constexpr VkDeviceSize kBoolLoopConstantsBytes = sizeof(uint32_t) * (8 + 32);
constexpr VkDeviceSize kFetchConstantsBytes = sizeof(uint32_t) * 6 * 32;

VkFormat ColorFormatToVk(xenos::ColorRenderTargetFormat format) {
  using F = xenos::ColorRenderTargetFormat;
  switch (format) {
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_GAMMA:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case F::k_2_10_10_10:
    case F::k_2_10_10_10_AS_10_10_10_10:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case F::k_2_10_10_10_FLOAT:
    case F::k_2_10_10_10_FLOAT_AS_16_16_16_16:
    case F::k_16_16_16_16_FLOAT:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case F::k_16_16:
      return VK_FORMAT_R16G16_SNORM;
    case F::k_16_16_16_16:
      return VK_FORMAT_R16G16B16A16_SNORM;
    case F::k_16_16_FLOAT:
      return VK_FORMAT_R16G16_SFLOAT;
    case F::k_32_FLOAT:
      return VK_FORMAT_R32_SFLOAT;
    case F::k_32_32_FLOAT:
      return VK_FORMAT_R32G32_SFLOAT;
    default:
      return VK_FORMAT_R8G8B8A8_UNORM;
  }
}

bool IsColorFormat64bpp(xenos::ColorRenderTargetFormat format) {
  using F = xenos::ColorRenderTargetFormat;
  return format == F::k_16_16_16_16 || format == F::k_16_16_16_16_FLOAT ||
         format == F::k_32_32_FLOAT;
}

// Depth buffers are D32_SFLOAT_S8_UINT unless nx_native_depth24 stores the
// game's unorm 24-bit ones as D24_UNORM_S8_UINT and its float 24-bit ones as
// D32_SFLOAT (see DepthFormatFor). A resolve turns depth into the R32_SFLOAT
// the translated shaders sample k_24_8 as, and into the guest's memory words
// (see Resolve).
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;

VkBlendFactor BlendFactorToVk(xenos::BlendFactor factor) {
  switch (uint32_t(factor)) {
    case 0:
      return VK_BLEND_FACTOR_ZERO;
    case 1:
      return VK_BLEND_FACTOR_ONE;
    case 4:
      return VK_BLEND_FACTOR_SRC_COLOR;
    case 5:
      return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 6:
      return VK_BLEND_FACTOR_SRC_ALPHA;
    case 7:
      return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8:
      return VK_BLEND_FACTOR_DST_COLOR;
    case 9:
      return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10:
      return VK_BLEND_FACTOR_DST_ALPHA;
    case 11:
      return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 12:
      return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 13:
      return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 14:
      return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 15:
      return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case 16:
      return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default:
      return VK_BLEND_FACTOR_ZERO;
  }
}

VkBlendOp BlendOpToVk(xenos::BlendOp op) {
  switch (uint32_t(op)) {
    case 1:
      return VK_BLEND_OP_SUBTRACT;
    case 2:
      return VK_BLEND_OP_MIN;
    case 3:
      return VK_BLEND_OP_MAX;
    case 4:
      return VK_BLEND_OP_REVERSE_SUBTRACT;
    default:
      return VK_BLEND_OP_ADD;
  }
}

struct RenderTargetKey {
  uint32_t base_tiles;
  uint32_t pitch;
  uint32_t format;
  uint32_t msaa;
  // Depth aliases retain their guest layout even with host MSAA disabled.
  // Color identity stays unchanged; its last written guest layout is tracked below.
  uint32_t guest_msaa;
  uint32_t is_depth;
  bool operator==(const RenderTargetKey& o) const { return !std::memcmp(this, &o, sizeof(o)); }
};
struct RenderTargetKeyHasher {
  size_t operator()(const RenderTargetKey& k) const { return size_t(XXH3_64bits(&k, sizeof(k))); }
};

struct RenderTarget {
  RenderTargetKey key;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  // Depth only, for the compute pass that resolves depth.
  VkImageView sample_view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t last_used_frame = 0;
  // Eligible for NVK's persistent ZCULL allocation. Transfer destinations are
  // excluded because transfer writes cannot maintain the depth hierarchy.
  bool zcull_compatible = false;
  // The last actual color write determines its guest EDRAM sample layout.
  uint32_t last_draw_guest_msaa = UINT32_MAX;
};

// The attachments of a rendering scope: color 0-3 and depth.
struct PassTargets {
  RenderTarget* color[4] = {};
  RenderTarget* depth = nullptr;
  bool operator==(const PassTargets& o) const {
    return !std::memcmp(this, &o, sizeof(o));
  }
  bool Empty() const { return !color[0] && !color[1] && !color[2] && !color[3] && !depth; }
};

// Everything a graphics pipeline depends on, hashed as plain memory.
struct PipelineKey {
  uint64_t vs_hash;
  uint64_t vs_modification;
  uint64_t ps_hash;
  uint64_t ps_modification;
  uint32_t color_formats[4];
  uint32_t depth_format;
  uint32_t samples;
  uint32_t topology;
  uint32_t geometry_shader;
  uint32_t primitive_restart;
  uint32_t cull_mode;
  uint32_t front_face_clockwise;
  uint32_t polygon_line;
  uint32_t depth_clamp;
  uint32_t depth_test;
  uint32_t depth_write;
  uint32_t depth_compare;
  uint32_t stencil_test;
  uint32_t stencil_front[4];
  uint32_t stencil_back[4];
  uint32_t blend[4];
  uint32_t write_masks;
  uint32_t texture_signs[8];
  uint64_t layout;
};

struct PipelineLayout {
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout textures_vertex = VK_NULL_HANDLE;
  VkDescriptorSetLayout textures_pixel = VK_NULL_HANDLE;
};

struct VertexRangeKey {
  uint32_t address;
  uint32_t size;
  uint64_t hash;
  bool operator==(const VertexRangeKey& o) const {
    return address == o.address && size == o.size && hash == o.hash;
  }
};
struct VertexRangeKeyHasher {
  size_t operator()(const VertexRangeKey& k) const { return size_t(k.hash ^ k.address); }
};

}  // namespace

struct Renderer::Impl {
  Renderer* owner = nullptr;
  rex::ui::vulkan::VulkanProvider* provider = nullptr;
  const VulkanDevice* device = nullptr;
  VkDevice vk = VK_NULL_HANDLE;
  rex::memory::Memory* memory = nullptr;
  RegisterFile* regs = nullptr;

  // Shaders.
  std::unique_ptr<SpirvShaderTranslator> translator;
  rex::string::StringBuffer disasm;
  std::unordered_map<uint64_t, std::unique_ptr<VulkanShader>> shaders;
  struct KnownShader {
    VulkanShader* shader;
    uint64_t sample;  // of all of the code
    uint64_t verified_frame;
    uint64_t head;  // of the first 16 words
  };
  std::unordered_map<uint64_t, KnownShader> shaders_by_location;
  VulkanShader* active_vs = nullptr;
  VulkanShader* active_ps = nullptr;
  bool specialize_signs = false;

  // Layouts.
  VkDescriptorSetLayout layout_empty = VK_NULL_HANDLE;
  VkDescriptorSetLayout layout_shared = VK_NULL_HANDLE;
  VkDescriptorSetLayout layout_constants = VK_NULL_HANDLE;
  uint32_t shared_binding_count = 1;
  std::unordered_map<uint64_t, VkDescriptorSetLayout> texture_set_layouts;
  std::unordered_map<uint64_t, PipelineLayout> pipeline_layouts;

  // The stream: one host-visible buffer the shaders read vertices from (as
  // the translator's "shared memory"), with the indices, the constants and the
  // texture staging data in it too.
  VkBuffer stream_buffer = VK_NULL_HANDLE;
  VkDeviceMemory stream_memory = VK_NULL_HANDLE;
  uint8_t* stream_mapping = nullptr;
  uint64_t stream_head = 0;  // total bytes ever allocated
  static constexpr uint64_t kNoStreamPosition = UINT64_MAX;
  // Stream data referenced by the draw currently being prepared. The renderer
  // may submit and rotate a submission before that draw is enqueued, so its
  // ranges must survive independently of any submission's stream_oldest.
  bool draw_stream_active = false;
  bool draw_stream_failed = false;
  bool draw_stream_committed = false;
  uint64_t draw_stream_oldest = kNoStreamPosition;
  uint64_t draw_stream_submission_oldest = kNoStreamPosition;
  VkDescriptorPool static_pool = VK_NULL_HANDLE;
  VkDescriptorSet set_shared = VK_NULL_HANDLE;
  VkDescriptorSet set_constants = VK_NULL_HANDLE;
  VkDeviceSize ubo_alignment = 256;

  // Submissions.
  struct Submission {
    VkCommandPool pool = VK_NULL_HANDLE;
    // The main command buffer's own: the setup one is recorded on the ring
    // thread while the recording thread records this one.
    VkCommandPool main_pool = VK_NULL_HANDLE;
    VkCommandBuffer setup = VK_NULL_HANDLE;
    VkCommandBuffer main = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false;
    uint64_t number = 0;
    uint64_t frame = 0;
    // Oldest stream position the submission reads.
    uint64_t stream_oldest = 0;
    std::vector<VkDescriptorPool> descriptor_pools;
    size_t descriptor_pool_used = 0;
    bool timed = false;
  };
  VkQueryPool timestamps = VK_NULL_HANDLE;
  float timestamp_period = 1.0f;
  Submission submissions[kSubmissionSlots];
  uint64_t submission_number = 0;  // the one being recorded, if recording
  bool recording = false;
  uint64_t frame = 1;
  uint64_t completed_frame = 0;

  // Opt-in, bounded output diagnostics. These sets only grow during a trace run.
  struct TraceResolveRect {
    uint32_t x, y, width, height, allocation_width, allocation_height;
  };
  std::set<std::array<uint32_t, 20>> trace_swap_geometries;
  std::set<std::array<uint32_t, 15>> trace_draw_geometries;
  std::set<std::array<uint32_t, 12>> trace_stencil_states;
  std::set<std::array<uint32_t, 8>> trace_stencil_clear_aliases;
  std::set<std::array<uint32_t, 8>> trace_depth_color_clears;
  std::set<std::array<uint32_t, 26>> trace_resolve_states;
  std::set<std::array<uint32_t, 26>> trace_depth_resolve_states;
  std::set<std::array<uint32_t, 12>> trace_shared_resolve_states;
  std::set<std::array<uint32_t, 14>> trace_shared_fallback_states;
  std::set<std::array<uint32_t, 50>> trace_attachment_transitions;
  std::unordered_map<uint32_t, TraceResolveRect> trace_resolve_rects;
  // Whether render targets get the game's sample count (nx_native_msaa).
  // The targets are sized in guest pixels whatever the sample count, so one
  // sample changes only the images, the pipelines and the resolves.
  bool multisample = true;
  // D24_UNORM_S8_UINT for the game's kD24S8 buffers (nx_native_depth24).
  bool depth24 = false;
  // Float24 guest surfaces also have an S8 aspect. Keep it even with compact
  // D24 storage enabled; dropping it would silently disable their stencil tests.
  VkFormat DepthFormatFor(uint32_t guest_format) const {
    if (!depth24) return kDepthFormat;
    return guest_format == uint32_t(xenos::DepthRenderTargetFormat::kD24S8)
               ? VK_FORMAT_D24_UNORM_S8_UINT
               : kDepthFormat;
  }
  static bool HasStencil(VkFormat format) { return format != VK_FORMAT_D32_SFLOAT; }
  VkDescriptorSetLayout depth_convert_set_layout = VK_NULL_HANDLE;
  VkPipelineLayout depth_convert_layout = VK_NULL_HANDLE;
  VkPipeline depth_convert_pipeline = VK_NULL_HANDLE;
  VkDescriptorPool depth_convert_pool = VK_NULL_HANDLE;
  VkDescriptorSet depth_convert_set = VK_NULL_HANDLE;
  // Depth straight from the depth buffer into the R32_SFLOAT image, in one
  // compute pass (depth_to_float.cs.glsl).
  VkDescriptorSetLayout depth_copy_set_layout = VK_NULL_HANDLE;
  VkPipelineLayout depth_copy_layout = VK_NULL_HANDLE;
  VkPipeline depth_copy_pipeline = VK_NULL_HANDLE;
  // Color resolves by compute (copy_color.cs.glsl), same set layout.
  VkPipelineLayout copy_color_layout = VK_NULL_HANDLE;
  VkPipeline copy_color_pipeline = VK_NULL_HANDLE;
  bool ComputeColorCopy(VkCommandBuffer c, VkImageView source, VkImageView dest, int32_t sx,
                        int32_t sy, int32_t dx, int32_t dy, uint32_t width, uint32_t height);
  bool CreateDepthConversion();

  // GPU time by render pass and resolve (nx_native_gpu_profile): on one
  // frame in 16, a timestamp is written where each pass or resolve begins,
  // and the time to the next one is charged to it.
  static constexpr uint32_t kProfileQueries = 512;
  VkQueryPool profile_pool = VK_NULL_HANDLE;
  bool profile_frame = false;
  struct ProfileSegment {
    uint64_t key;
    uint32_t draws;
  };
  std::vector<ProfileSegment> profile_segments[kSubmissionSlots];
  bool profile_closed[kSubmissionSlots] = {};
  struct ProfileTotal {
    double ns = 0;
    uint64_t count = 0;
    uint64_t draws = 0;
  };
  std::unordered_map<uint64_t, ProfileTotal> profile_totals;
  uint32_t profiled_frames = 0;
  double profiled_ns = 0;
  // Kinds, in the key's top 4 bits.
  enum : uint64_t { kProfileOther = 0, kProfilePass = 1, kProfileResolve = 2 };
  static uint64_t ProfileTargetBits(const RenderTarget* rt) {
    if (!rt) return 0xFFFFFF;
    return (uint64_t(rt->key.base_tiles & 0xFFF) << 12) | (uint64_t(rt->key.pitch & 0x7FF) << 1) |
           (rt->key.format & 1);
  }
  void ProfileMark(uint64_t key) {
    if (!profile_frame || !profile_pool || !recording) return;
    uint32_t slot = uint32_t(submission_number % kSubmissionSlots);
    auto& segments = profile_segments[slot];
    // The last query is kept for the closing timestamp.
    if (segments.size() + 1 >= kProfileQueries) return;
    dfn().vkCmdWriteTimestamp(cmd(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, profile_pool,
                              slot * kProfileQueries + uint32_t(segments.size()));
    segments.push_back({key, 0});
  }
  void ProfileDraw() {
    if (!profile_frame) return;
    auto& segments = profile_segments[submission_number % kSubmissionSlots];
    if (!segments.empty()) ++segments.back().draws;
  }
  void ProfileCollect(uint32_t slot);
  struct TextureTimer {
    uint64_t& total;
    uint64_t start;
    explicit TextureTimer(uint64_t& t) : total(t), start(NowNs()) {}
    ~TextureTimer() { total += NowNs() - start; }
  };
  void ProfileReport();
  uint32_t HostMsaa(xenos::MsaaSamples guest) const {
    return multisample ? uint32_t(guest) : 0;
  }
  bool null_images_initialized = false;

  // Render targets.
  std::unordered_map<RenderTargetKey, std::unique_ptr<RenderTarget>, RenderTargetKeyHasher>
      render_targets;
  bool in_pass = false;
  PassTargets pass;
  VkExtent2D pass_extent = {};

  // Pipelines.
  std::unordered_map<uint64_t, VkPipeline> pipelines;
  std::unordered_map<uint32_t, VkShaderModule> geometry_shaders;
  VkPipelineCache driver_cache = VK_NULL_HANDLE;
  std::filesystem::path driver_cache_path;
  uint64_t pipelines_at_last_save = 0;
  std::chrono::steady_clock::time_point last_pipeline_time;
  size_t driver_cache_saved_size = 0;
  std::atomic<bool> driver_cache_writing{false};
  std::thread driver_cache_writer;

  // Frame time tracking.
  std::chrono::steady_clock::time_point last_swap_time;
  RendererStats at_last_swap;

  // What the main command buffer has bound.
  VkPipeline bound_pipeline = VK_NULL_HANDLE;
  VkPipelineLayout bound_layout = VK_NULL_HANDLE;
  bool bound_static_sets = false;
  VkViewport bound_viewport = {};
  VkRect2D bound_scissor = {};
  float bound_depth_bias[2] = {NAN, NAN};
  bool bound_depth_bias_exact = false;
  float bound_blend_constants[4] = {NAN, NAN, NAN, NAN};
  uint32_t bound_stencil[6] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u};

  // Vertex and index data already in the stream: stream position by the
  // range and its content.
  struct RangeCopy {
    VertexRangeKey key = {};
    uint64_t position = 0;
  };
  FlatMap64<RangeCopy> vertex_ranges;
  static uint64_t RangeSlot(const VertexRangeKey& k) {
    uint64_t h = k.hash ^ ((uint64_t(k.address) << 32) | k.size);
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 31;
    return h == ~uint64_t(0) ? 0 : h;
  }
  // The stream position of a copy of this range, or UINT64_MAX.
  uint64_t FindRange(const VertexRangeKey& k) {
    RangeCopy* c = vertex_ranges.find(RangeSlot(k));
    return c && c->key == k ? c->position : UINT64_MAX;
  }
  void AddRange(const VertexRangeKey& k, uint64_t position) {
    vertex_ranges[RangeSlot(k)] = {k, position};
  }

  TextureCache textures;
  UploadTarget upload;

  // Texture descriptor sets, by content: the game uses the same few hundred
  // combinations of textures and samplers every frame, and allocating and
  // writing a set per draw (then freeing them one by one on pool reset) was
  // an eighth of the renderer's time.
  std::unordered_map<uint64_t, VkDescriptorSet> texture_set_cache;
  std::vector<VkDescriptorPool> set_pools;
  size_t set_pool_used = 0;
  struct RetiredPools {
    uint64_t frame;
    std::vector<VkDescriptorPool> pools;
  };
  std::vector<RetiredPools> retired_set_pools;
  uint64_t texture_views_epoch = 0;

  // Vertex buffer content hashes, computed once per frame for each range: many
  // draws share one buffer.
  struct VertexHash {
    uint64_t frame = UINT64_MAX;
    uint64_t hash = 0;
    // Last real content hash, and how many checks in a row found it changed.
    uint64_t content = 0;
    uint32_t changes = 0;
    ChangeDetector detector;
  };
  // This frame's hash of a vertex or index range. Data the game rewrites
  // every frame (skinned meshes, particles) was hashed in full every frame
  // only to be copied anyway, as the hash never matched: once a range has
  // changed on 4 checks in a row it gets a hash unique to the frame without
  // reading it (still copied once per frame, shared by its draws), and is
  // checked for real every 16 frames to notice it settling.
  uint64_t RangeHash(VertexHash& known, const uint8_t* data, uint32_t bytes, uint32_t address) {
    if (known.frame == frame) return known.hash;
    known.frame = frame;
    if (known.changes >= 4 && (frame & 15)) {
      known.hash = (frame + 1) * 0xD1B54A32D192ED03ull ^ (uint64_t(address) << 20) ^ bytes;
      return known.hash;
    }
    uint64_t content = known.detector.Check(data, bytes, frame, address >> 4, vertex_hash_bytes);
    known.changes = content != known.content ? std::min(known.changes + 1, 8u) : 0;
    known.content = content;
    known.hash = content;
    return content;
  }
  uint64_t vertex_hash_bytes = 0;
  FlatMap64<VertexHash> vertex_hashes;

  // Swap.
  VkDescriptorSetLayout swap_layout_ramp = VK_NULL_HANDLE;
  VkDescriptorSetLayout swap_layout_source = VK_NULL_HANDLE;
  VkDescriptorSetLayout swap_layout_dest = VK_NULL_HANDLE;
  VkPipelineLayout swap_pipeline_layout = VK_NULL_HANDLE;
  VkPipeline swap_pipeline = VK_NULL_HANDLE;
  VkBuffer ramp_buffer = VK_NULL_HANDLE;
  VkDeviceMemory ramp_memory = VK_NULL_HANDLE;
  uint8_t* ramp_mapping = nullptr;
  VkBufferView ramp_views[kSubmissionSlots] = {};
  VkDescriptorPool swap_pool = VK_NULL_HANDLE;
  VkDescriptorSet swap_sets[kSubmissionSlots][3] = {};
  uint32_t gamma_ramp[256] = {};

  SpirvShaderTranslator::SystemConstants system_constants = {};

  struct DrawSetup {
    bool valid = false;
    uint64_t state_generation = 0;
    uint64_t fetch_generation = 0;
    uint64_t rt_generation = 0;
    VulkanShader* active_vs = nullptr;
    VulkanShader* active_ps = nullptr;
    uint32_t initiator_state = 0;
    uint32_t skip_reason = UINT32_MAX;
    VulkanShader* vs = nullptr;
    VulkanShader* ps = nullptr;
    bool primitive_polygonal = false;
    reg::RB_DEPTHCONTROL depth_control = {};
    bool convert_fan = false, convert_loop = false, strip = false, reset_enabled = false;
    draw_util::Scissor scissor = {};
    draw_util::ViewportInfo viewport = {};
    PassTargets targets;
    const PipelineLayout* layout = nullptr;
    uint32_t counts[4] = {};
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkViewport vp = {};
    float bias_constant = 0.0f, bias_slope = 0.0f;
    bool bias_exact_unorm = false;
    float blend_constants[4] = {};
    uint32_t stencil[6] = {};
  };
  DrawSetup setup;
  std::unordered_map<uint64_t, DrawSetup> setup_cache;
  uint64_t setup_cache_rt_generation = 0;
  // Counts setup rebuilds, so derived caches can tell a setup apart.
  uint64_t setup_generation = 0;
  struct SystemInputs {
    uint64_t setup_generation = ~0ull;
    uint64_t fetch_generation = 0;
    uint64_t index_generation = 0;
    uint32_t used_mask = 0;
  };
  SystemInputs system_inputs;
  struct RetiredRenderTarget {
    uint64_t frame;
    VkImage image;
    VkImageView view;
    VkImageView sample_view;
    VkDeviceMemory memory;
  };
  std::vector<RetiredRenderTarget> retired_render_targets;
  // Staging for depth resolves (depth to buffer to color image).
  static constexpr VkDeviceSize kDepthScratchSize = 16u << 20;
  VkBuffer depth_scratch = VK_NULL_HANDLE;
  VkDeviceMemory depth_scratch_memory = VK_NULL_HANDLE;
  // Bumped whenever a render target is created, as setups hold pointers.
  uint64_t rt_generation = 1;
  bool BuildDrawSetup(uint32_t initiator_value);

  struct TextureBindings {
    bool valid = false;
    uint64_t frame = 0;
    uint64_t submission = 0;
    uint64_t views_epoch = 0;
    const VulkanShader* vs = nullptr;
    const VulkanShader* ps = nullptr;
    const PipelineLayout* layout = nullptr;
    uint32_t used_mask = 0;
    uint32_t slots = 0;
    VkDescriptorSet sets[2] = {};
    // Reusable in later frames through this one: no texture behind it is due
    // a content check before then. The textures are marked used on reuse.
    uint64_t valid_until = 0;
    uint64_t set_generation = 0;
    uint64_t answers_epoch = 0;
    // The views the sets hold, to check them alive when views_epoch moved.
    uint32_t view_count = 0;
    VkImageView views[24] = {};
    uint64_t view_ids[24] = {};
    // Keep every uploaded texture alive for the actual draw submission, including
    // bindings beyond the small view-validation cache above.
    std::vector<void*> textures;
  };
  FlatMap64<TextureBindings> tb_cache;

  // What each fetch slot last resolved to, this frame and submission: most
  // binding rebuilds change one or two slots of several.
  struct SlotViewEntry {
    uint64_t frame = 0;
    uint64_t submission = 0;
    uint64_t views_epoch = 0;
    // Resolve answers can switch to a tiled composite while old views stay alive.
    uint64_t answers_epoch = 0;
    uint32_t words[6] = {};
    uint32_t variant = 0;
    VkImageView view = VK_NULL_HANDLE;
    void* texture = nullptr;
    uint64_t valid_until = 0;
  };
  SlotViewEntry slot_views[32][8];
  // Set by SlotView: the texture behind its answer and how long it holds.
  void* slot_texture = nullptr;
  uint64_t slot_valid_until = 0;
  struct SlotSamplerEntry {
    bool valid = false;
    uint32_t words[6] = {};
    uint32_t binding_bits = 0;
    VkSampler sampler = VK_NULL_HANDLE;
  };
  SlotSamplerEntry slot_samplers[32][4];

  VkImageView SlotView(uint32_t slot, xenos::FetchOpDimension dimension, bool is_signed) {
    const uint32_t* words = &(*regs)[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 +
                                     slot * 6];
    uint32_t variant = uint32_t(dimension) | (uint32_t(is_signed) << 2);
    SlotViewEntry& e = slot_views[slot][variant & 7];
    if (e.frame == frame && e.submission == submission_number &&
        e.views_epoch == textures.views_epoch() && e.answers_epoch == textures.answers_epoch() &&
        e.variant == variant &&
        !std::memcmp(e.words, words, sizeof(e.words))) {
      slot_texture = e.texture;
      slot_valid_until = e.valid_until;
      return e.view;
    }
    VkImageView view =
        textures.GetView(regs->GetTextureFetch(slot), dimension, is_signed, upload);
    e.texture = slot_texture = textures.last_texture();
    e.valid_until = slot_valid_until = textures.last_valid_until();
    e.frame = frame;
    e.submission = submission_number;
    e.views_epoch = textures.views_epoch();
    e.answers_epoch = textures.answers_epoch();
    e.variant = variant;
    std::memcpy(e.words, words, sizeof(e.words));
    e.view = view;
    return view;
  }

  VkSampler SlotSampler(const SpirvShader::SamplerBinding& binding) {
    uint32_t slot = binding.fetch_constant;
    const uint32_t* words = &(*regs)[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 +
                                     slot * 6];
    uint32_t bits = uint32_t(binding.mag_filter) | (uint32_t(binding.min_filter) << 2) |
                    (uint32_t(binding.mip_filter) << 4) |
                    (uint32_t(binding.aniso_filter) << 6);
    SlotSamplerEntry* entries = slot_samplers[slot];
    for (uint32_t i = 0; i < 4; ++i) {
      SlotSamplerEntry& e = entries[i];
      if (e.valid && e.binding_bits == bits && !std::memcmp(e.words, words, sizeof(e.words))) {
        return e.sampler;
      }
    }
    VkSampler sampler = textures.GetSampler(regs->GetTextureFetch(slot), binding);
    // Samplers are never destroyed, so these stay valid across frames.
    SlotSamplerEntry& e = entries[(bits ^ words[0]) & 3];
    e.valid = true;
    e.binding_bits = bits;
    std::memcpy(e.words, words, sizeof(e.words));
    e.sampler = sampler;
    return sampler;
  }
  uint64_t tb_cache_frame = 0;

  // The fetch constants as the shaders get them: the guest's, with vertex
  // buffer addresses pointing into the stream.
  std::array<uint32_t, 6 * 32> fetch_words = {};
  std::array<uint64_t, 96> fetch_resolved_frame = {};
  std::array<uint64_t, 96> fetch_resolved_position = {};
  std::array<uint8_t, 96> draw_stream_fetch_slots = {};
  uint64_t fetch_words_frame = 0;
  uint64_t fetch_words_submission = 0;
  uint32_t last_fetch_words[6 * 32] = {};

  uint32_t bound_offsets[SpirvShaderTranslator::kConstantBufferCount] = {};
  bool bound_offsets_valid = false;
  VkDescriptorSet bound_texture_sets[2] = {};
  VkDeviceSize bound_index_offset = 0;
  VkIndexType bound_index_type = VK_INDEX_TYPE_UINT16;
  bool bound_index_valid = false;

  // The last upload of each constant buffer, reused by the next draws while
  // the registers behind it are unchanged (the ring counts register writes)
  // and the stream still holds it (same submission).
  struct ReusedUpload {
    uint64_t submission = 0;
    uint64_t generation = 0;
    const void* shader = nullptr;
    uint64_t frame = 0;
    VkDeviceSize offset = 0;
    uint64_t stream_position = kNoStreamPosition;
  };
  ReusedUpload last_fetch, last_vs_float, last_ps_float, last_bool_loop;
  SpirvShaderTranslator::SystemConstants last_system_constants = {};
  ReusedUpload last_system;
  bool Reusable(const ReusedUpload& u, uint64_t generation, const void* shader,
                bool per_frame = false) const {
    return u.submission == submission_number && u.generation == generation &&
           u.shader == shader && (!per_frame || u.frame == frame);
  }

  const VulkanDevice::Functions& dfn() const { return device->functions(); }
  // Not in ReXGlue's device function table.
  PFN_vkCmdClearDepthStencilImage vkCmdClearDepthStencilImage = nullptr;
  PFN_vkCmdResolveImage vkCmdResolveImage = nullptr;
  PFN_vkDeviceWaitIdle vkDeviceWaitIdle = nullptr;
  Submission& current() { return submissions[submission_number % kSubmissionSlots]; }
  // Waits for queued draws first: the recording thread and this one never
  // record into the command buffer at the same time.
  VkCommandBuffer cmd() {
    DrainRecorder();
    return current().main;
  }

  // --- Recording thread ------------------------------------------------------------
  // A draw's Vulkan calls (binds, dynamic state, the draw), decided on the
  // ring thread and made on the recording thread (nx_native_record_thread)
  // or straight away. In the busiest areas the driver's share of a draw was
  // about a quarter of the ring thread's time; on another core it overlaps
  // with the ring preparing the next draw. Everything else recorded into the
  // main command buffer goes through cmd(), which waits for the queue to
  // empty, and the texture uploads go into the setup command buffer, which
  // has a pool of its own so the two threads never share one.
  struct DrawCommand {
    VkCommandBuffer commands;
    uint32_t flags;
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkDescriptorSet set_shared;
    VkDescriptorSet set_constants;
    uint32_t offsets[SpirvShaderTranslator::kConstantBufferCount];
    VkDescriptorSet texture_sets[2];
    VkViewport viewport;
    VkRect2D scissor;
    float bias_constant, bias_slope;
    bool bias_exact_unorm;
    float blend[4];
    uint32_t stencil[6];
    VkBuffer index_buffer;
    VkDeviceSize index_offset;
    VkIndexType index_type;
    uint32_t count;
  };
  enum : uint32_t {
    kDrawPipeline = 1u << 0,
    kDrawSharedSet = 1u << 1,
    kDrawConstants = 1u << 2,
    kDrawTexturesVertex = 1u << 3,
    kDrawTexturesPixel = 1u << 4,
    kDrawViewport = 1u << 5,
    kDrawScissor = 1u << 6,
    kDrawDepthBias = 1u << 7,
    kDrawBlend = 1u << 8,
    kDrawStencilCompare = 1u << 9,
    kDrawStencilWrite = 1u << 10,
    kDrawStencilReference = 1u << 11,
    kDrawIndexBuffer = 1u << 12,
    kDrawIndexed = 1u << 13,
  };
  void ExecuteDrawCommand(const DrawCommand& d) const {
    VkCommandBuffer c = d.commands;
    const auto& f = device->functions();
    if (d.flags & kDrawPipeline) {
      f.vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipeline);
    }
    if (d.flags & kDrawSharedSet) {
      f.vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout,
                                SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram, 1,
                                &d.set_shared, 0, nullptr);
    }
    if (d.flags & kDrawConstants) {
      f.vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout,
                                SpirvShaderTranslator::kDescriptorSetConstants, 1,
                                &d.set_constants, SpirvShaderTranslator::kConstantBufferCount,
                                d.offsets);
    }
    if (d.flags & kDrawTexturesVertex) {
      f.vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout,
                                SpirvShaderTranslator::kDescriptorSetTexturesVertex, 1,
                                &d.texture_sets[0], 0, nullptr);
    }
    if (d.flags & kDrawTexturesPixel) {
      f.vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout,
                                SpirvShaderTranslator::kDescriptorSetTexturesPixel, 1,
                                &d.texture_sets[1], 0, nullptr);
    }
    if (d.flags & kDrawViewport) f.vkCmdSetViewport(c, 0, 1, &d.viewport);
    if (d.flags & kDrawScissor) f.vkCmdSetScissor(c, 0, 1, &d.scissor);
    if (d.flags & kDrawDepthBias) {
      if (d.bias_exact_unorm) {
        VkDepthBiasRepresentationInfoEXT representation{};
        representation.sType = VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT;
        representation.depthBiasRepresentation =
            VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORMAT_EXT;
        representation.depthBiasExact = VK_TRUE;
        VkDepthBiasInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_DEPTH_BIAS_INFO_EXT;
        info.pNext = &representation;
        info.depthBiasConstantFactor = d.bias_constant;
        info.depthBiasSlopeFactor = d.bias_slope;
        f.vkCmdSetDepthBias2EXT(c, &info);
      } else {
        f.vkCmdSetDepthBias(c, d.bias_constant, 0.0f, d.bias_slope);
      }
    }
    if (d.flags & kDrawBlend) f.vkCmdSetBlendConstants(c, d.blend);
    if (d.flags & kDrawStencilCompare) {
      f.vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_FRONT_BIT, d.stencil[0]);
      f.vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_BACK_BIT, d.stencil[1]);
    }
    if (d.flags & kDrawStencilWrite) {
      f.vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_BIT, d.stencil[2]);
      f.vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_BACK_BIT, d.stencil[3]);
    }
    if (d.flags & kDrawStencilReference) {
      f.vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_BIT, d.stencil[4]);
      f.vkCmdSetStencilReference(c, VK_STENCIL_FACE_BACK_BIT, d.stencil[5]);
    }
    if (d.flags & kDrawIndexBuffer) {
      f.vkCmdBindIndexBuffer(c, d.index_buffer, d.index_offset, d.index_type);
    }
    if (d.flags & kDrawIndexed) {
      f.vkCmdDrawIndexed(c, d.count, 1, 0, 0, 0);
    } else {
      f.vkCmdDraw(c, d.count, 1, 0, 0);
    }
  }
  static constexpr uint32_t kRecordQueueSize = 4096;
  std::unique_ptr<DrawCommand[]> record_queue;
  std::atomic<uint32_t> record_head{0};
  std::atomic<uint32_t> record_tail{0};
  std::atomic<bool> record_stop{false};
  std::atomic<bool> record_sleeping{false};
  std::mutex record_mutex;
  std::condition_variable record_cv;
  std::thread record_thread;
  bool record_threaded = false;

  void WakeRecorder() {
    if (record_sleeping.load(std::memory_order_seq_cst)) {
      std::lock_guard<std::mutex> lock(record_mutex);
      record_cv.notify_one();
    }
  }

  void RecordThread() {
#if defined(__SWITCH__)
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2C);
    // Off the ring thread's core.
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, 1, 0x3);
#endif
    uint32_t idle = 0;
    while (true) {
      uint32_t tail = record_tail.load(std::memory_order_relaxed);
      if (tail != record_head.load(std::memory_order_acquire)) {
        ExecuteDrawCommand(record_queue[tail % kRecordQueueSize]);
        record_tail.store(tail + 1, std::memory_order_release);
        idle = 0;
        continue;
      }
      if (record_stop.load(std::memory_order_acquire)) return;
      // Briefly: spinning longer took cores from the game's own threads.
      if (++idle < 16) {
        std::this_thread::yield();
        continue;
      }
      std::unique_lock<std::mutex> lock(record_mutex);
      record_sleeping.store(true, std::memory_order_seq_cst);
      if (record_head.load(std::memory_order_seq_cst) == tail &&
          !record_stop.load(std::memory_order_acquire)) {
        record_cv.wait_for(lock, std::chrono::milliseconds(2));
      }
      record_sleeping.store(false, std::memory_order_relaxed);
      idle = 0;
    }
  }

  void SubmitDraw(const DrawCommand& d) {
    if (!record_threaded) {
      ExecuteDrawCommand(d);
      return;
    }
    uint32_t head = record_head.load(std::memory_order_relaxed);
    while (head - record_tail.load(std::memory_order_acquire) >= kRecordQueueSize) {
      WakeRecorder();
      std::this_thread::yield();
    }
    record_queue[head % kRecordQueueSize] = d;
    record_head.store(head + 1, std::memory_order_seq_cst);
    // In batches: the recording thread kept up draw by draw, slept between
    // them, and waking it for every draw (a lock and a system call) showed as
    // 6% of this thread in lock contention. It is also woken when this
    // thread is about to wait for the game (KickRecorder), and anything
    // recording into the command buffer drains the queue first.
    if (head + 1 - record_tail.load(std::memory_order_relaxed) >= kRecordBatch) WakeRecorder();
  }
  static constexpr uint32_t kRecordBatch = 32;

  void KickRecorder() {
    if (record_threaded &&
        record_tail.load(std::memory_order_acquire) != record_head.load(std::memory_order_relaxed)) {
      WakeRecorder();
    }
  }

  void DrainRecorder() {
    if (!record_threaded) return;
    uint32_t head = record_head.load(std::memory_order_relaxed);
    if (record_tail.load(std::memory_order_acquire) == head) return;
    uint64_t start = NowNs();
    uint32_t spins = 0;
    while (record_tail.load(std::memory_order_acquire) != head) {
      WakeRecorder();
      if (++spins > 32) std::this_thread::yield();
    }
    frame_drain_wait_ns += NowNs() - start;
  }
  RendererStats& stats() { return owner->stats_; }

  // --- Setup ---------------------------------------------------------------------

  bool Initialize();
  void Shutdown();
  bool CreateSwapResources();

  // --- Submissions and the stream ----------------------------------------------

  // A frame is complete when every submission up to it is: a frame can be
  // split over several submissions (when the stream fills up), and counting it
  // complete when its first one finished freed textures and staging memory the
  // rest still used, an MMU fault that lost the GPU. The frame being recorded
  // is never complete.
  void UpdateCompletedFrame() {
    uint64_t oldest = frame;
    for (const Submission& other : submissions) {
      if (other.pending) oldest = std::min(oldest, other.frame);
    }
    if (oldest > 0) {
      completed_frame = std::max(completed_frame, oldest - 1);
    }
  }

  void Completed(Submission& s) {
    s.pending = false;
    UpdateCompletedFrame();
    ProfileCollect(uint32_t(&s - submissions));
    if (s.timed && timestamps) {
      uint32_t slot = uint32_t(&s - submissions);
      uint64_t ticks[2] = {};
      if (dfn().vkGetQueryPoolResults(vk, timestamps, slot * 2, 2, sizeof(ticks), ticks,
                                      sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
          ticks[1] > ticks[0]) {
        stats().gpu_ns += uint64_t(double(ticks[1] - ticks[0]) * timestamp_period);
        ++stats().gpu_submissions;
      }
      s.timed = false;
    }
  }

  // This frame's waits and texture time (RendererStats::frame_*).
  uint64_t frame_upload_wait_ns = 0, frame_fence_wait_ns = 0, frame_drain_wait_ns = 0,
           frame_texture_ns = 0, frame_draw_count = 0, frame_stream_wait_ns = 0;
  static uint64_t NowNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
  }

  void WaitSubmission(Submission& s) {
    if (s.pending) {
      uint64_t start = NowNs();
      dfn().vkWaitForFences(vk, 1, &s.fence, VK_TRUE, UINT64_MAX);
      frame_fence_wait_ns += NowNs() - start;
      Completed(s);
    }
  }

  void PollCompleted() {
    for (Submission& s : submissions) {
      if (s.pending && dfn().vkGetFenceStatus(vk, s.fence) == VK_SUCCESS) {
        Completed(s);
      }
    }
    UpdateCompletedFrame();
  }

  void BeginRecording() {
    if (recording) {
      return;
    }
    ++submission_number;
    Submission& s = current();
    WaitSubmission(s);
    dfn().vkResetFences(vk, 1, &s.fence);
    dfn().vkResetCommandPool(vk, s.pool, 0);
    dfn().vkResetCommandPool(vk, s.main_pool, 0);
    for (VkDescriptorPool pool : s.descriptor_pools) {
      dfn().vkResetDescriptorPool(vk, pool, 0);
    }
    s.descriptor_pool_used = 0;
    s.number = submission_number;
    s.frame = frame;
    s.stream_oldest = stream_head;
    if (draw_stream_active) {
      draw_stream_submission_oldest = s.stream_oldest;
      if (draw_stream_oldest != kNoStreamPosition) {
        s.stream_oldest = std::min(s.stream_oldest, draw_stream_oldest);
      }
    }
    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dfn().vkBeginCommandBuffer(s.setup, &begin);
    dfn().vkBeginCommandBuffer(s.main, &begin);
    if (timestamps) {
      uint32_t slot = uint32_t(submission_number % kSubmissionSlots);
      dfn().vkCmdResetQueryPool(s.setup, timestamps, slot * 2, 2);
      dfn().vkCmdWriteTimestamp(s.setup, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, slot * 2);
      s.timed = true;
    }
    if (!null_images_initialized) {
      textures.InitializeNullImages(s.setup);
      null_images_initialized = true;
    }
    {
      uint32_t slot = uint32_t(submission_number % kSubmissionSlots);
      profile_segments[slot].clear();
      profile_closed[slot] = false;
      if (profile_frame && profile_pool) {
        dfn().vkCmdResetQueryPool(s.setup, profile_pool, slot * kProfileQueries,
                                  kProfileQueries);
      }
    }
    // Everything the setup command buffer uploads is visible to the draws.
    VkMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                            VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    dfn().vkCmdPipelineBarrier(s.main, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0,
                               nullptr);
    recording = true;
    bound_pipeline = VK_NULL_HANDLE;
    bound_layout = VK_NULL_HANDLE;
    bound_static_sets = false;
    std::memset(&bound_viewport, 0, sizeof(bound_viewport));
    std::memset(&bound_scissor, 0, sizeof(bound_scissor));
    bound_depth_bias[0] = bound_depth_bias[1] = NAN;
    bound_depth_bias_exact = false;
    bound_offsets_valid = false;
    bound_texture_sets[0] = bound_texture_sets[1] = VK_NULL_HANDLE;
    bound_index_valid = false;
    for (float& f : bound_blend_constants) f = NAN;
    for (uint32_t& v : bound_stencil) v = ~0u;
  }

  void Submit() {
    if (!recording) {
      return;
    }
    DrainRecorder();
    EndPass();
    // The setup command buffer copies staging data the upload worker fills.
    {
      uint64_t start = NowNs();
      textures.WaitForUploads();
      frame_upload_wait_ns += NowNs() - start;
    }
    // Keep pending render/resolve dependencies across submissions. Queue order
    // alone does not make writes visible to later commands.
    Submission& s = current();
    {
      uint32_t slot = uint32_t(submission_number % kSubmissionSlots);
      if (profile_pool && !profile_segments[slot].empty()) {
        dfn().vkCmdWriteTimestamp(s.main, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, profile_pool,
                                  slot * kProfileQueries +
                                      uint32_t(profile_segments[slot].size()));
        profile_closed[slot] = true;
      }
    }
    if (s.timed) {
      dfn().vkCmdWriteTimestamp(s.main, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps,
                                uint32_t(submission_number % kSubmissionSlots) * 2 + 1);
    }
    dfn().vkEndCommandBuffer(s.setup);
    dfn().vkEndCommandBuffer(s.main);
    VkCommandBuffer buffers[] = {s.setup, s.main};
    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 2;
    submit.pCommandBuffers = buffers;
    {
      auto queue = device->AcquireQueue(device->queue_family_graphics_compute(), 0);
      if (dfn().vkQueueSubmit(queue.queue(), 1, &submit, s.fence) != VK_SUCCESS) {
        REXLOG_ERROR("[native] vkQueueSubmit failed");
      }
    }
    s.pending = true;
    recording = false;
    ++stats().submissions;
  }

  // Stream memory for this submission; stalls on older submissions if the
  // ring would run over data they still read.
  // The start of the stream's next lap. Not rex::align: that rounds with a
  // mask, right only for powers of two, and the stream is 160 MB. Wrapping
  // with it put allocations at arbitrary places, far enough ahead that the
  // stream looked full and the renderer waited for the GPU to finish all its
  // work (30-45 ms, every wrap), or across the end of the buffer.
  static uint64_t NextStreamLap(uint64_t position) {
    return (position / kStreamSize + 1) * kStreamSize;
  }

  struct DrawStreamScope {
    explicit DrawStreamScope(Impl& renderer) : renderer(renderer) {
      renderer.draw_stream_active = true;
      renderer.draw_stream_failed = false;
      renderer.draw_stream_committed = false;
      renderer.draw_stream_oldest = kNoStreamPosition;
      renderer.draw_stream_submission_oldest = renderer.current().stream_oldest;
      renderer.draw_stream_fetch_slots.fill(0);
    }
    ~DrawStreamScope() {
      if (!renderer.draw_stream_committed && renderer.recording &&
          renderer.draw_stream_submission_oldest != kNoStreamPosition) {
        // A skipped draw must not keep reused ranges pinned. Allocations made
        // by the draw are still covered by the submission's original oldest.
        renderer.current().stream_oldest = renderer.draw_stream_submission_oldest;
      }
      if (!renderer.draw_stream_committed) {
        for (size_t i = 0; i < renderer.draw_stream_fetch_slots.size(); ++i) {
          if (renderer.draw_stream_fetch_slots[i] &&
              renderer.fetch_resolved_frame[i] == renderer.frame) {
            renderer.fetch_resolved_frame[i] = 0;
          }
        }
      }
      if (renderer.draw_stream_failed) {
        // A staging allocation rejected by the draw pin may have cached a
        // null-texture answer. Retry it in the next draw, including the memo,
        // slot-view and binding caches which all follow answers_epoch.
        renderer.textures.InvalidateAnswers();
      }
      renderer.draw_stream_active = false;
      renderer.draw_stream_failed = false;
      renderer.draw_stream_committed = false;
      renderer.draw_stream_oldest = kNoStreamPosition;
      renderer.draw_stream_submission_oldest = kNoStreamPosition;
      renderer.draw_stream_fetch_slots.fill(0);
    }
    DrawStreamScope(const DrawStreamScope&) = delete;
    DrawStreamScope& operator=(const DrawStreamScope&) = delete;
    Impl& renderer;
  };

  void PinDrawStreamPosition(uint64_t position) {
    if (position == kNoStreamPosition) return;
    if (draw_stream_active) {
      draw_stream_oldest = std::min(draw_stream_oldest, position);
      current().stream_oldest = std::min(current().stream_oldest, position);
    } else {
      current().stream_oldest = std::min(current().stream_oldest, position);
    }
  }

  uint8_t* Allocate(VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& offset_out,
                    bool pin_for_draw = true) {
    if (draw_stream_active && draw_stream_failed) {
      return nullptr;
    }
    if (size > kStreamSize / 4) {
      return nullptr;
    }
    BeginRecording();
    // Allocations stay kStreamGuard short of the end, so a constant binding's
    // fixed range starting at any allocation stays inside the buffer.
    uint64_t start = rex::align(stream_head, uint64_t(alignment));
    if ((start % kStreamSize) + size > kStreamSize - kStreamGuard) {
      start = NextStreamLap(start);
    }
    uint64_t end = start + size;
    if (draw_stream_active && draw_stream_oldest != kNoStreamPosition &&
        end - draw_stream_oldest > kStreamSize) {
      draw_stream_failed = true;
      return nullptr;
    }
    // The current submission itself must not wrap onto what it still reads.
    const uint64_t fence_before = frame_fence_wait_ns;
    if (end - current().stream_oldest > kStreamSize) {
      Submit();
      WaitSubmission(submissions[submission_number % kSubmissionSlots]);
      BeginRecording();
      start = rex::align(stream_head, uint64_t(alignment));
      if ((start % kStreamSize) + size > kStreamSize - kStreamGuard) {
        start = NextStreamLap(start);
      }
      end = start + size;
      if (draw_stream_active) {
        // BeginRecording restored the unfinished draw's pin.
        if (draw_stream_oldest != kNoStreamPosition &&
            end - draw_stream_oldest > kStreamSize) {
          frame_stream_wait_ns += frame_fence_wait_ns - fence_before;
          draw_stream_failed = true;
          return nullptr;
        }
      }
    }
    for (Submission& s : submissions) {
      if (&s != &current() && s.pending && end - s.stream_oldest > kStreamSize) {
        WaitSubmission(s);
      }
    }
    frame_stream_wait_ns += frame_fence_wait_ns - fence_before;
    stream_head = end;
    offset_out = VkDeviceSize(start % kStreamSize);
    if (draw_stream_active && pin_for_draw) {
      PinDrawStreamPosition(start);
    }
    return stream_mapping + offset_out;
  }

  VkDescriptorSet AllocatePersistentSet(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    while (true) {
      if (set_pool_used >= set_pools.size()) {
        VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16384},
            {VK_DESCRIPTOR_TYPE_SAMPLER, 16384},
        };
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = 4096;
        pool_info.poolSizeCount = 2;
        pool_info.pPoolSizes = sizes;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (dfn().vkCreateDescriptorPool(vk, &pool_info, nullptr, &pool) != VK_SUCCESS) {
          return VK_NULL_HANDLE;
        }
        set_pools.push_back(pool);
      }
      info.descriptorPool = set_pools[set_pool_used];
      if (dfn().vkAllocateDescriptorSets(vk, &info, &set) == VK_SUCCESS) {
        return set;
      }
      ++set_pool_used;
    }
  }

  // Forgets every cached set; the pools are freed once the GPU is done with
  // the frames that may use them.
  // Bumped when the cached texture sets are dropped: their pools are freed
  // once the GPU is done, so bindings kept from earlier frames must not name
  // them any more.
  uint64_t set_cache_generation = 1;
  void FlushSetCache() {
    ++set_cache_generation;
    texture_set_cache.clear();
    if (!set_pools.empty()) {
      retired_set_pools.push_back({frame, std::move(set_pools)});
      set_pools.clear();
    }
    set_pool_used = 0;
  }

  void FreeRetiredSetPools() {
    for (auto it = retired_set_pools.begin(); it != retired_set_pools.end();) {
      if (it->frame <= completed_frame) {
        for (VkDescriptorPool pool : it->pools) {
          dfn().vkDestroyDescriptorPool(vk, pool, nullptr);
        }
        it = retired_set_pools.erase(it);
      } else {
        ++it;
      }
    }
  }

  VkDescriptorSet AllocateSet(VkDescriptorSetLayout layout) {
    Submission& s = current();
    VkDescriptorSetAllocateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    while (true) {
      if (s.descriptor_pool_used >= s.descriptor_pools.size()) {
        VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16384},
            {VK_DESCRIPTOR_TYPE_SAMPLER, 16384},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256},
        };
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = 4096;
        pool_info.poolSizeCount = 3;
        pool_info.pPoolSizes = sizes;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (dfn().vkCreateDescriptorPool(vk, &pool_info, nullptr, &pool) != VK_SUCCESS) {
          return VK_NULL_HANDLE;
        }
        s.descriptor_pools.push_back(pool);
      }
      info.descriptorPool = s.descriptor_pools[s.descriptor_pool_used];
      if (dfn().vkAllocateDescriptorSets(vk, &info, &set) == VK_SUCCESS) {
        return set;
      }
      ++s.descriptor_pool_used;
    }
  }

  // --- Shaders, layouts and pipelines ----------------------------------------------

  VkDescriptorSetLayout GetTextureSetLayout(bool vertex, uint32_t textures, uint32_t samplers);
  const PipelineLayout* GetPipelineLayout(uint32_t tex_vs, uint32_t samp_vs, uint32_t tex_ps,
                                          uint32_t samp_ps);
  bool EnsureTranslated(VulkanShader::VulkanTranslation* translation, bool* pending_out);
  void DrainTranslations();
  void TranslationThread();
  std::unique_ptr<SpirvShaderTranslator> worker_translator;
  // 1 = being translated on the worker, 2 = done.
  std::unordered_map<const void*, uint8_t> translation_state;
  std::thread translation_thread;
  std::mutex translation_mutex;
  std::condition_variable translation_cv;
  std::deque<VulkanShader::VulkanTranslation*> translation_jobs;
  std::vector<VulkanShader::VulkanTranslation*> translations_done;
  std::atomic<uint32_t> translations_done_count{0};
  bool translation_stop = false;
  struct PipelineJob {
    uint64_t hash = 0;
    PipelineKey key = {};
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule pixel = VK_NULL_HANDLE;
    VkShaderModule geometry = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
  };
  VkPipeline GetPipeline(const PipelineKey& key, VulkanShader::VulkanTranslation* vs,
                         VulkanShader::VulkanTranslation* ps, VkShaderModule geometry,
                         VkPipelineLayout layout, bool* pending_out);
  VkPipeline CreatePipeline(const PipelineJob& job);
  void DrainCompletedPipelines();
  void PipelineThread();
  std::unordered_set<uint64_t> pending_pipelines;
  std::thread pipeline_thread;
  std::mutex pipeline_mutex;
  std::condition_variable pipeline_cv;
  std::deque<PipelineJob> pipeline_jobs;
  std::vector<std::pair<uint64_t, VkPipeline>> pipelines_completed;
  std::atomic<uint32_t> pipelines_completed_count{0};
  bool pipeline_stop = false;

  // --- Render targets ------------------------------------------------------------

  RenderTarget* GetRenderTarget(const RenderTargetKey& key, uint32_t min_height,
                               bool discard_for_zcull = false);
  void EndPass() {
    if (in_pass) {
      dfn().vkCmdEndRendering(cmd());
      in_pass = false;
      // Dynamic rendering has no implicit memory dependency between scopes.
      // Defer the barrier so adjacent resolves can still share one.
      resolve_barrier_pending = true;
    }
  }
  bool BeginPass(const PassTargets& targets, const VkClearValue* color0_clear = nullptr,
                 const VkClearValue* depth_clear = nullptr);

  // --- Draws ---------------------------------------------------------------------

  void Draw(const uint32_t* immediate_indices, uint32_t immediate_index_words);
  bool GetFastClearRectangle(StencilClearRect& rect, bool require_zero_depth);
  void MirrorStencilClear();
  void MirrorZeroDepthColorClear();
  void UpdateSystemConstants(bool primitive_polygonal, const draw_util::ViewportInfo& viewport,
                             uint32_t used_texture_mask, uint32_t vertex_index_endian_flags,
                             const VulkanShader* vs, const VulkanShader* ps);
  void Resolve();
  bool resolve_barrier_pending = false;
  void FlushResolveBarrier();
  void Swap(rex::ui::Presenter* presenter, uint32_t frontbuffer, uint32_t width, uint32_t height);
  void SaveDriverCache();
};

// --- Setup -------------------------------------------------------------------------

bool Renderer::Impl::Initialize() {
  device = provider->vulkan_device();
  vk = device->device();
  auto get_proc = device->vulkan_instance()->functions().vkGetDeviceProcAddr;
  vkCmdClearDepthStencilImage = reinterpret_cast<PFN_vkCmdClearDepthStencilImage>(
      get_proc(vk, "vkCmdClearDepthStencilImage"));
  vkCmdResolveImage =
      reinterpret_cast<PFN_vkCmdResolveImage>(get_proc(vk, "vkCmdResolveImage"));
  vkDeviceWaitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(get_proc(vk, "vkDeviceWaitIdle"));
  if (!vkCmdClearDepthStencilImage || !vkCmdResolveImage || !vkDeviceWaitIdle) {
    REXLOG_ERROR("[native] Missing Vulkan device functions");
    return false;
  }
  const auto& properties = device->properties();
  if (!properties.dynamicRendering) {
    REXLOG_ERROR("[native] The native renderer needs dynamic rendering");
    return false;
  }
  specialize_signs = REXCVAR_GET(gpu_specialize_texture_signs);
  multisample = REXCVAR_GET(nx_native_msaa);
  REXLOG_INFO("[native] MSAA: {}",
              multisample ? "honoring guest sample count" : "disabled (single-sample host targets)");
  ubo_alignment = std::max<VkDeviceSize>(properties.minUniformBufferOffsetAlignment, 16);

  translator = std::make_unique<SpirvShaderTranslator>(SpirvShaderTranslator::Features(device),
                                                       true, true, false);

  VkShaderStageFlags guest_stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                                    (properties.geometryShader ? VK_SHADER_STAGE_GEOMETRY_BIT : 0);

  VkDescriptorSetLayoutCreateInfo layout_info = {};
  layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  if (dfn().vkCreateDescriptorSetLayout(vk, &layout_info, nullptr, &layout_empty) != VK_SUCCESS) {
    return false;
  }
  // Set 0: the translator's shared memory, here the stream. With a small
  // maxStorageBufferRange the translator splits it into several bindings.
  shared_binding_count = 1u << SpirvShaderTranslator::GetSharedMemoryStorageBufferCountLog2(
                             properties.maxStorageBufferRange);
  VkDescriptorSetLayoutBinding shared_binding = {};
  shared_binding.binding = 0;
  shared_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  shared_binding.descriptorCount = shared_binding_count;
  shared_binding.stageFlags = guest_stages;
  layout_info.bindingCount = 1;
  layout_info.pBindings = &shared_binding;
  if (dfn().vkCreateDescriptorSetLayout(vk, &layout_info, nullptr, &layout_shared) != VK_SUCCESS) {
    return false;
  }
  // Set 1: the five constant buffers, dynamic so one set serves every draw.
  VkDescriptorSetLayoutBinding constant_bindings[SpirvShaderTranslator::kConstantBufferCount] = {};
  for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
    constant_bindings[i].binding = i;
    constant_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    constant_bindings[i].descriptorCount = 1;
    constant_bindings[i].stageFlags = guest_stages;
  }
  constant_bindings[SpirvShaderTranslator::kConstantBufferFloatVertex].stageFlags =
      VK_SHADER_STAGE_VERTEX_BIT;
  constant_bindings[SpirvShaderTranslator::kConstantBufferFloatPixel].stageFlags =
      VK_SHADER_STAGE_FRAGMENT_BIT;
  layout_info.bindingCount = SpirvShaderTranslator::kConstantBufferCount;
  layout_info.pBindings = constant_bindings;
  if (dfn().vkCreateDescriptorSetLayout(vk, &layout_info, nullptr, &layout_constants) !=
      VK_SUCCESS) {
    return false;
  }

  // The stream.
  VkDeviceSize stream_allocation = 0;
  uint32_t stream_memory_type = 0;
  if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
          device, kStreamSize + kStreamSlack,
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
          rex::ui::vulkan::util::MemoryPurpose::kUpload, stream_buffer, stream_memory,
          &stream_memory_type, &stream_allocation)) {
    REXLOG_ERROR("[native] Could not create the {} MB stream buffer", kStreamSize >> 20);
    return false;
  }
  if (!(device->memory_types().host_coherent & (1u << stream_memory_type))) {
    REXLOG_WARN("[native] The stream memory is not host-coherent");
  }
  void* mapping = nullptr;
  if (dfn().vkMapMemory(vk, stream_memory, 0, VK_WHOLE_SIZE, 0, &mapping) != VK_SUCCESS) {
    return false;
  }
  stream_mapping = static_cast<uint8_t*>(mapping);

  VkDescriptorPoolSize static_sizes[] = {
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, SpirvShaderTranslator::kConstantBufferCount},
  };
  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = 2;
  pool_info.poolSizeCount = 2;
  pool_info.pPoolSizes = static_sizes;
  if (dfn().vkCreateDescriptorPool(vk, &pool_info, nullptr, &static_pool) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorSetLayout static_layouts[] = {layout_shared, layout_constants};
  VkDescriptorSet static_sets[2];
  VkDescriptorSetAllocateInfo allocate = {};
  allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocate.descriptorPool = static_pool;
  allocate.descriptorSetCount = 2;
  allocate.pSetLayouts = static_layouts;
  if (dfn().vkAllocateDescriptorSets(vk, &allocate, static_sets) != VK_SUCCESS) {
    return false;
  }
  set_shared = static_sets[0];
  set_constants = static_sets[1];
  // With several shared memory bindings each covers a slice of guest memory;
  // the stream is far smaller than the first slice, so all of them can point at
  // the whole stream.
  std::vector<VkDescriptorBufferInfo> shared_infos(shared_binding_count);
  VkDeviceSize slice = VkDeviceSize(512u << 20) / shared_binding_count;
  for (uint32_t i = 0; i < shared_binding_count; ++i) {
    // Binding i serves the addresses from i * slice, so it starts there in
    // the stream (clamped for the slices past the end of the stream, which
    // are never addressed).
    VkDeviceSize offset = std::min(VkDeviceSize(i) * slice, kStreamSize - 256);
    offset &= ~(properties.minStorageBufferOffsetAlignment - 1);
    shared_infos[i].buffer = stream_buffer;
    shared_infos[i].offset = offset;
    shared_infos[i].range = kStreamSize + kStreamSlack - offset;
  }
  VkDescriptorBufferInfo constant_infos[SpirvShaderTranslator::kConstantBufferCount];
  VkDeviceSize constant_ranges[] = {sizeof(SpirvShaderTranslator::SystemConstants),
                                    kFloatConstantsBytes, kFloatConstantsBytes,
                                    kBoolLoopConstantsBytes, kFetchConstantsBytes};
  for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
    constant_infos[i].buffer = stream_buffer;
    constant_infos[i].offset = 0;
    constant_infos[i].range = constant_ranges[i];
  }
  VkWriteDescriptorSet writes[2] = {};
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = set_shared;
  writes[0].descriptorCount = shared_binding_count;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[0].pBufferInfo = shared_infos.data();
  writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[1].dstSet = set_constants;
  writes[1].descriptorCount = SpirvShaderTranslator::kConstantBufferCount;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
  writes[1].pBufferInfo = constant_infos;
  dfn().vkUpdateDescriptorSets(vk, 2, writes, 0, nullptr);

  // Submissions.
  for (Submission& s : submissions) {
    VkCommandPoolCreateInfo command_pool = {};
    command_pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    command_pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    command_pool.queueFamilyIndex = device->queue_family_graphics_compute();
    if (dfn().vkCreateCommandPool(vk, &command_pool, nullptr, &s.pool) != VK_SUCCESS) {
      return false;
    }
    if (dfn().vkCreateCommandPool(vk, &command_pool, nullptr, &s.main_pool) != VK_SUCCESS) {
      return false;
    }
    VkCommandBufferAllocateInfo buffers = {};
    buffers.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    buffers.commandPool = s.pool;
    buffers.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    buffers.commandBufferCount = 1;
    if (dfn().vkAllocateCommandBuffers(vk, &buffers, &s.setup) != VK_SUCCESS) {
      return false;
    }
    buffers.commandPool = s.main_pool;
    if (dfn().vkAllocateCommandBuffers(vk, &buffers, &s.main) != VK_SUCCESS) {
      return false;
    }
    VkFenceCreateInfo fence = {};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (dfn().vkCreateFence(vk, &fence, nullptr, &s.fence) != VK_SUCCESS) {
      return false;
    }
  }

  VkQueryPoolCreateInfo query_info = {};
  query_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
  query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
  query_info.queryCount = kSubmissionSlots * 2;
  if (properties.timestampPeriod > 0.0f &&
      dfn().vkCreateQueryPool(vk, &query_info, nullptr, &timestamps) == VK_SUCCESS) {
    timestamp_period = properties.timestampPeriod;
  } else {
    timestamps = VK_NULL_HANDLE;
  }

  VkPipelineCacheCreateInfo cache_info = {};
  cache_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  dfn().vkCreatePipelineCache(vk, &cache_info, nullptr, &driver_cache);

  if (!textures.Initialize(device, memory)) {
    return false;
  }
  upload.buffer = stream_buffer;
  upload.resolve_commands = [this]() {
    EndPass();
    // The first on-demand crop can be created when no render pass is active.
    // Its transfer fallback must still be visible before the next draw/present.
    resolve_barrier_pending = true;
    return cmd();
  };
  REXLOG_INFO("[native] Resolve resource mode: {}",
              REXCVAR_GET(nx_native_shared_resolves) ? "shared-direct-v1" : "legacy-composite");
  REXLOG_INFO("[native] ZCULL depth allocation: {}",
              REXCVAR_GET(nx_native_zcull) ? "persistent-eligible (growth uses copy fallback)"
                                         : "legacy-transfer");
  upload.allocate = [this](size_t size, size_t alignment, VkDeviceSize& offset) {
    // Upload staging is consumed by the setup copy, not by the draw itself.
    // The allocator still checks it against active draw pins, while the
    // submission's own oldest position protects it until uploads complete.
    uint8_t* result = Allocate(size, alignment, offset, false);
    // An allocation may have started a new submission.
    upload.setup_commands = current().setup;
    upload.submission = submission_number;
    return result;
  };
  if (!CreateSwapResources()) {
    return false;
  }
  pipeline_thread = std::thread([this]() { PipelineThread(); });
  if (REXCVAR_GET(nx_native_record_thread)) {
    record_queue = std::make_unique<DrawCommand[]>(kRecordQueueSize);
    record_thread = std::thread([this]() { RecordThread(); });
    record_threaded = true;
  }
  worker_translator = std::make_unique<SpirvShaderTranslator>(
      SpirvShaderTranslator::Features(device), true, true, false);
  translation_thread = std::thread([this]() { TranslationThread(); });
  if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
          device, kDepthScratchSize,
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, depth_scratch,
          depth_scratch_memory)) {
    depth_scratch = VK_NULL_HANDLE;
    REXLOG_WARN("[native] No scratch buffer for depth resolves");
  }
  if (depth_scratch && !CreateDepthConversion()) {
    REXLOG_WARN("[native] No depth resolve pipeline; depth resolves are not done");
    if (depth_convert_pipeline) dfn().vkDestroyPipeline(vk, depth_convert_pipeline, nullptr);
    depth_convert_pipeline = VK_NULL_HANDLE;
  }
  if (copy_color_pipeline) {
    textures.SetComputeCopy([this](VkCommandBuffer c, VkImageView source, VkImageView dest,
                                   int32_t sx, int32_t sy, int32_t dx, int32_t dy, uint32_t width,
                                   uint32_t height) {
      return ComputeColorCopy(c, source, dest, sx, sy, dx, dy, width, height);
    });
  }
  if (REXCVAR_GET(nx_native_depth24)) {
    VkFormatProperties format_properties = {};
    device->vulkan_instance()->functions().vkGetPhysicalDeviceFormatProperties(
        device->physical_device(), VK_FORMAT_D24_UNORM_S8_UINT, &format_properties);
    if ((format_properties.optimalTilingFeatures &
         (VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) ==
            (VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) &&
        depth_scratch && depth_convert_pipeline) {
      depth24 = true;
    } else {
      REXLOG_WARN("[native] 24-bit depth buffers unavailable; using 32-bit float");
    }
  }
  if (REXCVAR_GET(nx_native_trace_output)) {
    REXLOG_INFO(
        "[native][trace] depth capabilities: compact_depth={} separateStencilMaskRef={} "
        "depthClamp={} guest_D24S8_host={} guest_D24FS8_host={}",
        depth24, properties.separateStencilMaskRef, properties.depthClamp,
        uint32_t(DepthFormatFor(uint32_t(xenos::DepthRenderTargetFormat::kD24S8))),
        uint32_t(DepthFormatFor(uint32_t(xenos::DepthRenderTargetFormat::kD24FS8))));
  }
  REXLOG_INFO("[native] D24S8 depth bias: {}",
      depth24 && properties.depthBiasControl && properties.depthBiasExact &&
              dfn().vkCmdSetDepthBias2EXT
          ? "exact D24 UNORM quantum (VK_EXT_depth_bias_control)"
          : "legacy format-dependent offset (exact D24 bias unavailable)");
  if (REXCVAR_GET(nx_native_gpu_profile) && timestamps) {
    VkQueryPoolCreateInfo profile_info = {};
    profile_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    profile_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    profile_info.queryCount = kSubmissionSlots * kProfileQueries;
    if (dfn().vkCreateQueryPool(vk, &profile_info, nullptr, &profile_pool) != VK_SUCCESS) {
      profile_pool = VK_NULL_HANDLE;
    }
  }
  // The default linear 256-entry gamma ramp (CommandProcessor::Initialize).
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t value = i * 0x3FF / 0xFF;
    gamma_ramp[i] = value | (value << 10) | (value << 20);
  }
  REXLOG_INFO("[native] Renderer ready: {} MB stream, {} shared memory binding(s)",
              kStreamSize >> 20, shared_binding_count);
  return true;
}

bool Renderer::Impl::CreateDepthConversion() {
  VkDescriptorSetLayoutBinding binding = {};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo layout_info = {};
  layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layout_info.bindingCount = 1;
  layout_info.pBindings = &binding;
  if (dfn().vkCreateDescriptorSetLayout(vk, &layout_info, nullptr, &depth_convert_set_layout) !=
      VK_SUCCESS) {
    return false;
  }
  VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t) * 5};
  VkPipelineLayoutCreateInfo pipeline_layout = {};
  pipeline_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout.setLayoutCount = 1;
  pipeline_layout.pSetLayouts = &depth_convert_set_layout;
  pipeline_layout.pushConstantRangeCount = 1;
  pipeline_layout.pPushConstantRanges = &push;
  if (dfn().vkCreatePipelineLayout(vk, &pipeline_layout, nullptr, &depth_convert_layout) !=
      VK_SUCCESS) {
    return false;
  }
  depth_convert_pipeline = rex::ui::vulkan::util::CreateComputePipeline(
      device, depth_convert_layout, shaders::kDepthResolveCs, sizeof(shaders::kDepthResolveCs));
  if (!depth_convert_pipeline) {
    return false;
  }
  VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &size;
  if (dfn().vkCreateDescriptorPool(vk, &pool_info, nullptr, &depth_convert_pool) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorSetAllocateInfo allocate = {};
  allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocate.descriptorPool = depth_convert_pool;
  allocate.descriptorSetCount = 1;
  allocate.pSetLayouts = &depth_convert_set_layout;
  if (dfn().vkAllocateDescriptorSets(vk, &allocate, &depth_convert_set) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorBufferInfo buffer = {depth_scratch, 0, kDepthScratchSize};
  VkWriteDescriptorSet write = {};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = depth_convert_set;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write.pBufferInfo = &buffer;
  dfn().vkUpdateDescriptorSets(vk, 1, &write, 0, nullptr);

  VkDescriptorSetLayoutBinding copy_bindings[2] = {};
  copy_bindings[0].binding = 0;
  copy_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  copy_bindings[0].descriptorCount = 1;
  copy_bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  copy_bindings[1] = copy_bindings[0];
  copy_bindings[1].binding = 1;
  copy_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  layout_info.bindingCount = 2;
  layout_info.pBindings = copy_bindings;
  VkPushConstantRange copy_push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t) * 6};
  pipeline_layout.pSetLayouts = &depth_copy_set_layout;
  pipeline_layout.pPushConstantRanges = &copy_push;
  if (dfn().vkCreateDescriptorSetLayout(vk, &layout_info, nullptr, &depth_copy_set_layout) ==
          VK_SUCCESS &&
      dfn().vkCreatePipelineLayout(vk, &pipeline_layout, nullptr, &depth_copy_layout) ==
          VK_SUCCESS) {
    depth_copy_pipeline = rex::ui::vulkan::util::CreateComputePipeline(
        device, depth_copy_layout, shaders::kDepthToFloatCs, sizeof(shaders::kDepthToFloatCs));
  }
  if (!depth_copy_pipeline) {
    REXLOG_WARN("[native] No direct depth resolve; resolving depth through a buffer");
  }

  VkPhysicalDeviceFeatures features = {};
  device->vulkan_instance()->functions().vkGetPhysicalDeviceFeatures(device->physical_device(),
                                                                     &features);
  if (depth_copy_set_layout && features.shaderStorageImageWriteWithoutFormat) {
    VkPushConstantRange color_push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(int32_t) * 6};
    pipeline_layout.pSetLayouts = &depth_copy_set_layout;
    pipeline_layout.pPushConstantRanges = &color_push;
    if (dfn().vkCreatePipelineLayout(vk, &pipeline_layout, nullptr, &copy_color_layout) ==
        VK_SUCCESS) {
      copy_color_pipeline = rex::ui::vulkan::util::CreateComputePipeline(
          device, copy_color_layout, shaders::kCopyColorCs, sizeof(shaders::kCopyColorCs));
    }
  }
  if (!copy_color_pipeline) {
    REXLOG_WARN("[native] No compute color copy; resolving color with vkCmdCopyImage");
  }
  return true;
}

bool Renderer::Impl::ComputeColorCopy(VkCommandBuffer c, VkImageView source, VkImageView dest,
                                      int32_t sx, int32_t sy, int32_t dx, int32_t dy,
                                      uint32_t width, uint32_t height) {
  if (!copy_color_pipeline || !width || !height) return false;
  VkDescriptorSet set = AllocateSet(depth_copy_set_layout);
  if (!set) return false;
  VkDescriptorImageInfo images[2] = {{VK_NULL_HANDLE, source, VK_IMAGE_LAYOUT_GENERAL},
                                     {VK_NULL_HANDLE, dest, VK_IMAGE_LAYOUT_GENERAL}};
  VkWriteDescriptorSet writes[2] = {};
  for (uint32_t i = 0; i < 2; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType =
        i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[i].pImageInfo = &images[i];
  }
  dfn().vkUpdateDescriptorSets(vk, 2, writes, 0, nullptr);
  // Earlier writes of the source (rendering, copies, compute) before the
  // reads, and earlier reads of the destination before the writes.
  VkMemoryBarrier before = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                                VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
  dfn().vkCmdPipelineBarrier(
      c,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
          VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
  int32_t params[6] = {sx, sy, dx, dy, int32_t(width), int32_t(height)};
  dfn().vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, copy_color_pipeline);
  dfn().vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, copy_color_layout, 0, 1, &set,
                                0, nullptr);
  dfn().vkCmdPushConstants(c, copy_color_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params),
                           params);
  dfn().vkCmdDispatch(c, (width + 7) / 8, (height + 7) / 8, 1);
  VkMemoryBarrier after = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                           VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                           VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                               VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
  dfn().vkCmdPipelineBarrier(
      c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
          VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      0, 1, &after, 0, nullptr, 0, nullptr);
  return true;
}

void Renderer::Impl::ProfileCollect(uint32_t slot) {
  auto& segments = profile_segments[slot];
  if (!profile_pool || !profile_closed[slot] || segments.empty()) {
    segments.clear();
    return;
  }
  profile_closed[slot] = false;
  std::vector<uint64_t> ticks(segments.size() + 1);
  if (dfn().vkGetQueryPoolResults(vk, profile_pool, slot * kProfileQueries, uint32_t(ticks.size()),
                                  ticks.size() * sizeof(uint64_t), ticks.data(), sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
    for (size_t i = 0; i < segments.size(); ++i) {
      if (ticks[i + 1] < ticks[i]) continue;
      double ns = double(ticks[i + 1] - ticks[i]) * timestamp_period;
      ProfileTotal& total = profile_totals[segments[i].key];
      total.ns += ns;
      ++total.count;
      total.draws += segments[i].draws;
      profiled_ns += ns;
    }
  }
  segments.clear();
}

void Renderer::Impl::ProfileReport() {
  if (!profile_pool || !profile_frame) return;
  // One profiled frame in 16; report about every 10 seconds at 30 fps.
  if (++profiled_frames < 20) return;
  std::vector<std::pair<uint64_t, ProfileTotal>> sorted(profile_totals.begin(),
                                                        profile_totals.end());
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
  double frames = double(profiled_frames);
  std::string line = fmt::format("[native] GPU by pass, ms per frame (sampled, as the driver "
                                 "reports it): total {:.1f}",
                                 profiled_ns / 1e6 / frames);
  auto target = [](uint64_t bits) -> std::string {
    if (bits == 0xFFFFFF) return "-";
    return fmt::format("t{}p{}{}", (bits >> 12) & 0xFFF, (bits >> 1) & 0x7FF,
                       (bits & 1) ? "f" : "");
  };
  for (size_t i = 0; i < sorted.size() && i < 14; ++i) {
    uint64_t key = sorted[i].first;
    const ProfileTotal& t = sorted[i].second;
    std::string what;
    switch (key >> 60) {
      case kProfilePass:
        what = fmt::format("pass c={} d={}", target((key >> 24) & 0xFFFFFF), target(key & 0xFFFFFF));
        break;
      case kProfileResolve:
        what = fmt::format("resolve{} p{} {}x{}", ((key >> 56) & 1) ? " depth" : "",
                           (key >> 32) & 0x7FF, (key >> 16) & 0xFFFF, key & 0xFFFF);
        break;
      default:
        what = "other";
        break;
    }
    line += fmt::format(" | {}: {:.2f} ({:.1f}x, {:.0f} draws)", what, t.ns / 1e6 / frames,
                        double(t.count) / frames, double(t.draws) / frames);
  }
  REXLOG_INFO("{}", line);
  profile_totals.clear();
  profiled_frames = 0;
  profiled_ns = 0;
}

bool Renderer::Impl::CreateSwapResources() {
  // The same layout as VulkanCommandProcessor's apply-gamma compute pipeline:
  // set 0 the ramp (uniform texel buffer), set 1 the source (sampled image),
  // set 2 the destination (storage image), and the size as a push constant.
  VkDescriptorSetLayoutBinding binding = {};
  binding.binding = 0;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo layout_info = {};
  layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layout_info.bindingCount = 1;
  layout_info.pBindings = &binding;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
  if (dfn().vkCreateDescriptorSetLayout(vk, &layout_info, nullptr, &swap_layout_ramp) !=
      VK_SUCCESS) {
    return false;
  }
  binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  if (dfn().vkCreateDescriptorSetLayout(vk, &layout_info, nullptr, &swap_layout_source) !=
      VK_SUCCESS) {
    return false;
  }
  binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  if (dfn().vkCreateDescriptorSetLayout(vk, &layout_info, nullptr, &swap_layout_dest) !=
      VK_SUCCESS) {
    return false;
  }
  VkDescriptorSetLayout set_layouts[] = {swap_layout_ramp, swap_layout_source, swap_layout_dest};
  VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t) * 2};
  VkPipelineLayoutCreateInfo pipeline_layout = {};
  pipeline_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout.setLayoutCount = 3;
  pipeline_layout.pSetLayouts = set_layouts;
  pipeline_layout.pushConstantRangeCount = 1;
  pipeline_layout.pPushConstantRanges = &push;
  if (dfn().vkCreatePipelineLayout(vk, &pipeline_layout, nullptr, &swap_pipeline_layout) !=
      VK_SUCCESS) {
    return false;
  }
  swap_pipeline = rex::ui::vulkan::util::CreateComputePipeline(
      device, swap_pipeline_layout, shaders::apply_gamma_table_cs,
      sizeof(shaders::apply_gamma_table_cs));
  if (swap_pipeline == VK_NULL_HANDLE) {
    REXLOG_ERROR("[native] Could not create the swap pipeline");
    return false;
  }

  VkDeviceSize ramp_size = 256 * sizeof(uint32_t) * kSubmissionSlots;
  if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
          device, ramp_size,
          VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
          rex::ui::vulkan::util::MemoryPurpose::kUpload, ramp_buffer, ramp_memory)) {
    return false;
  }
  void* mapping = nullptr;
  dfn().vkMapMemory(vk, ramp_memory, 0, VK_WHOLE_SIZE, 0, &mapping);
  ramp_mapping = static_cast<uint8_t*>(mapping);

  VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, kSubmissionSlots},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSubmissionSlots},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSubmissionSlots},
  };
  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = 3 * kSubmissionSlots;
  pool_info.poolSizeCount = 3;
  pool_info.pPoolSizes = sizes;
  if (dfn().vkCreateDescriptorPool(vk, &pool_info, nullptr, &swap_pool) != VK_SUCCESS) {
    return false;
  }
  for (uint32_t i = 0; i < kSubmissionSlots; ++i) {
    VkDescriptorSetAllocateInfo allocate = {};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = swap_pool;
    allocate.descriptorSetCount = 3;
    allocate.pSetLayouts = set_layouts;
    if (dfn().vkAllocateDescriptorSets(vk, &allocate, swap_sets[i]) != VK_SUCCESS) {
      return false;
    }
    VkBufferViewCreateInfo view = {};
    view.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
    view.buffer = ramp_buffer;
    view.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    view.offset = 256 * sizeof(uint32_t) * i;
    view.range = 256 * sizeof(uint32_t);
    if (dfn().vkCreateBufferView(vk, &view, nullptr, &ramp_views[i]) != VK_SUCCESS) {
      return false;
    }
    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = swap_sets[i][0];
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    write.pTexelBufferView = &ramp_views[i];
    dfn().vkUpdateDescriptorSets(vk, 1, &write, 0, nullptr);
  }
  return true;
}

void Renderer::Impl::Shutdown() {
  if (!device) {
    return;
  }
  if (record_thread.joinable()) {
    DrainRecorder();
    record_stop.store(true, std::memory_order_release);
    {
      std::lock_guard<std::mutex> lock(record_mutex);
      record_cv.notify_all();
    }
    record_thread.join();
    record_threaded = false;
  }
  if (driver_cache_writer.joinable()) {
    driver_cache_writer.join();
  }
  if (translation_thread.joinable()) {
    {
      std::lock_guard<std::mutex> lock(translation_mutex);
      translation_stop = true;
    }
    translation_cv.notify_all();
    translation_thread.join();
  }
  if (pipeline_thread.joinable()) {
    {
      std::lock_guard<std::mutex> lock(pipeline_mutex);
      pipeline_stop = true;
    }
    pipeline_cv.notify_all();
    pipeline_thread.join();
  }
  DrainCompletedPipelines();
  Submit();
  for (Submission& s : submissions) {
    WaitSubmission(s);
  }
  vkDeviceWaitIdle(vk);
  textures.Shutdown();
  for (auto& [key, rt] : render_targets) {
    if (rt->sample_view) dfn().vkDestroyImageView(vk, rt->sample_view, nullptr);
    if (rt->view) dfn().vkDestroyImageView(vk, rt->view, nullptr);
    if (rt->image) dfn().vkDestroyImage(vk, rt->image, nullptr);
    if (rt->memory) dfn().vkFreeMemory(vk, rt->memory, nullptr);
  }
  render_targets.clear();
  for (auto& retired : retired_render_targets) {
    if (retired.sample_view) dfn().vkDestroyImageView(vk, retired.sample_view, nullptr);
    dfn().vkDestroyImageView(vk, retired.view, nullptr);
    dfn().vkDestroyImage(vk, retired.image, nullptr);
    dfn().vkFreeMemory(vk, retired.memory, nullptr);
  }
  retired_render_targets.clear();
  if (depth_convert_pipeline) dfn().vkDestroyPipeline(vk, depth_convert_pipeline, nullptr);
  if (depth_convert_layout) dfn().vkDestroyPipelineLayout(vk, depth_convert_layout, nullptr);
  if (depth_copy_pipeline) dfn().vkDestroyPipeline(vk, depth_copy_pipeline, nullptr);
  if (copy_color_pipeline) dfn().vkDestroyPipeline(vk, copy_color_pipeline, nullptr);
  if (copy_color_layout) dfn().vkDestroyPipelineLayout(vk, copy_color_layout, nullptr);
  if (depth_copy_layout) dfn().vkDestroyPipelineLayout(vk, depth_copy_layout, nullptr);
  if (depth_copy_set_layout) {
    dfn().vkDestroyDescriptorSetLayout(vk, depth_copy_set_layout, nullptr);
  }
  if (depth_convert_pool) dfn().vkDestroyDescriptorPool(vk, depth_convert_pool, nullptr);
  if (depth_convert_set_layout) {
    dfn().vkDestroyDescriptorSetLayout(vk, depth_convert_set_layout, nullptr);
  }
  if (profile_pool) dfn().vkDestroyQueryPool(vk, profile_pool, nullptr);
  if (depth_scratch) dfn().vkDestroyBuffer(vk, depth_scratch, nullptr);
  if (depth_scratch_memory) dfn().vkFreeMemory(vk, depth_scratch_memory, nullptr);
  for (auto& [key, pipeline] : pipelines) {
    if (pipeline) dfn().vkDestroyPipeline(vk, pipeline, nullptr);
  }
  pipelines.clear();
  for (auto& [key, module] : geometry_shaders) {
    if (module) dfn().vkDestroyShaderModule(vk, module, nullptr);
  }
  geometry_shaders.clear();
  for (auto& [key, layout] : pipeline_layouts) {
    dfn().vkDestroyPipelineLayout(vk, layout.layout, nullptr);
  }
  pipeline_layouts.clear();
  for (auto& [key, layout] : texture_set_layouts) {
    if (layout != layout_empty) dfn().vkDestroyDescriptorSetLayout(vk, layout, nullptr);
  }
  texture_set_layouts.clear();
  shaders.clear();
  FlushSetCache();
  for (auto& retired : retired_set_pools) {
    for (VkDescriptorPool pool : retired.pools) dfn().vkDestroyDescriptorPool(vk, pool, nullptr);
  }
  retired_set_pools.clear();
  for (Submission& s : submissions) {
    for (VkDescriptorPool pool : s.descriptor_pools) {
      dfn().vkDestroyDescriptorPool(vk, pool, nullptr);
    }
    s.descriptor_pools.clear();
    if (s.fence) dfn().vkDestroyFence(vk, s.fence, nullptr);
    if (s.pool) dfn().vkDestroyCommandPool(vk, s.pool, nullptr);
    if (s.main_pool) dfn().vkDestroyCommandPool(vk, s.main_pool, nullptr);
    s = Submission();
  }
  for (VkBufferView view : ramp_views) {
    if (view) dfn().vkDestroyBufferView(vk, view, nullptr);
  }
  if (swap_pool) dfn().vkDestroyDescriptorPool(vk, swap_pool, nullptr);
  if (swap_pipeline) dfn().vkDestroyPipeline(vk, swap_pipeline, nullptr);
  if (swap_pipeline_layout) dfn().vkDestroyPipelineLayout(vk, swap_pipeline_layout, nullptr);
  for (VkDescriptorSetLayout l : {swap_layout_ramp, swap_layout_source, swap_layout_dest,
                                  layout_empty, layout_shared, layout_constants}) {
    if (l) dfn().vkDestroyDescriptorSetLayout(vk, l, nullptr);
  }
  if (ramp_buffer) dfn().vkDestroyBuffer(vk, ramp_buffer, nullptr);
  if (ramp_memory) dfn().vkFreeMemory(vk, ramp_memory, nullptr);
  if (static_pool) dfn().vkDestroyDescriptorPool(vk, static_pool, nullptr);
  if (stream_buffer) dfn().vkDestroyBuffer(vk, stream_buffer, nullptr);
  if (stream_memory) dfn().vkFreeMemory(vk, stream_memory, nullptr);
  if (driver_cache) dfn().vkDestroyPipelineCache(vk, driver_cache, nullptr);
  if (timestamps) dfn().vkDestroyQueryPool(vk, timestamps, nullptr);
  device = nullptr;
}

// --- Layouts and pipelines -----------------------------------------------------------

VkDescriptorSetLayout Renderer::Impl::GetTextureSetLayout(bool vertex, uint32_t texture_count,
                                                          uint32_t sampler_count) {
  if (!texture_count && !sampler_count) {
    return layout_empty;
  }
  uint64_t key = (uint64_t(vertex) << 40) | (uint64_t(texture_count) << 20) | sampler_count;
  auto it = texture_set_layouts.find(key);
  if (it != texture_set_layouts.end()) {
    return it->second;
  }
  std::vector<VkDescriptorSetLayoutBinding> bindings;
  VkShaderStageFlags stages = vertex ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
  for (uint32_t i = 0; i < texture_count; ++i) {
    bindings.push_back({i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, stages, nullptr});
  }
  for (uint32_t i = 0; i < sampler_count; ++i) {
    bindings.push_back({texture_count + i, VK_DESCRIPTOR_TYPE_SAMPLER, 1, stages, nullptr});
  }
  VkDescriptorSetLayoutCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  info.bindingCount = uint32_t(bindings.size());
  info.pBindings = bindings.data();
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  if (dfn().vkCreateDescriptorSetLayout(vk, &info, nullptr, &layout) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  texture_set_layouts.emplace(key, layout);
  return layout;
}

const PipelineLayout* Renderer::Impl::GetPipelineLayout(uint32_t tex_vs, uint32_t samp_vs,
                                                        uint32_t tex_ps, uint32_t samp_ps) {
  uint64_t key = (uint64_t(tex_vs) << 48) | (uint64_t(samp_vs) << 32) | (uint64_t(tex_ps) << 16) |
                 samp_ps;
  auto it = pipeline_layouts.find(key);
  if (it != pipeline_layouts.end()) {
    return &it->second;
  }
  PipelineLayout layout;
  layout.textures_vertex = GetTextureSetLayout(true, tex_vs, samp_vs);
  layout.textures_pixel = GetTextureSetLayout(false, tex_ps, samp_ps);
  if (!layout.textures_vertex || !layout.textures_pixel) {
    return nullptr;
  }
  VkDescriptorSetLayout sets[SpirvShaderTranslator::kDescriptorSetCount];
  sets[SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] = layout_shared;
  sets[SpirvShaderTranslator::kDescriptorSetConstants] = layout_constants;
  sets[SpirvShaderTranslator::kDescriptorSetTexturesVertex] = layout.textures_vertex;
  sets[SpirvShaderTranslator::kDescriptorSetTexturesPixel] = layout.textures_pixel;
  VkPipelineLayoutCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  info.setLayoutCount = SpirvShaderTranslator::kDescriptorSetCount;
  info.pSetLayouts = sets;
  if (dfn().vkCreatePipelineLayout(vk, &info, nullptr, &layout.layout) != VK_SUCCESS) {
    return nullptr;
  }
  return &pipeline_layouts.emplace(key, layout).first->second;
}

// Translation runs on one worker thread with a translator of its own. Being one
// thread matters: the first completed translation of a shader fills the
// shader's binding lists, and every later one leaves them alone, so a
// translation reported done means its shader's bindings are final.
bool Renderer::Impl::EnsureTranslated(VulkanShader::VulkanTranslation* translation,
                                      bool* pending_out) {
  *pending_out = false;
  DrainTranslations();
  auto state = translation_state.find(translation);
  if (state == translation_state.end()) {
    if (!translation->shader().is_ucode_analyzed()) {
      translation->shader().AnalyzeUcode(disasm);
    }
    if (!REXCVAR_GET(nx_native_async_pipelines) || !translation_thread.joinable()) {
      bool ok = translator->TranslateAnalyzedShader(*translation);
      if (ok) translation->GetOrCreateShaderModule();
      ++stats().shaders_translated;
      translation_state[translation] = 2;
      if (!ok) {
        REXLOG_ERROR("[native] Shader {:016X} did not translate",
                     translation->shader().ucode_data_hash());
        return false;
      }
    } else {
      translation_state[translation] = 1;
      {
        std::lock_guard<std::mutex> lock(translation_mutex);
        translation_jobs.push_back(translation);
      }
      translation_cv.notify_one();
      *pending_out = true;
      return false;
    }
  } else if (state->second == 1) {
    *pending_out = true;
    return false;
  }
  return translation->is_valid() && translation->shader_module() != VK_NULL_HANDLE;
}

void Renderer::Impl::DrainTranslations() {
  if (!translations_done_count.load(std::memory_order_acquire)) {
    return;
  }
  std::vector<VulkanShader::VulkanTranslation*> done;
  {
    std::lock_guard<std::mutex> lock(translation_mutex);
    done.swap(translations_done);
    translations_done_count.store(0, std::memory_order_release);
  }
  for (auto* t : done) {
    translation_state[t] = 2;
    ++stats().shaders_translated;
  }
}

void Renderer::Impl::TranslationThread() {
#if defined(__SWITCH__)
  svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3D);
#endif
  while (true) {
    VulkanShader::VulkanTranslation* translation;
    {
      std::unique_lock<std::mutex> lock(translation_mutex);
      translation_cv.wait(lock, [&] { return translation_stop || !translation_jobs.empty(); });
      if (translation_stop) return;
      translation = translation_jobs.front();
      translation_jobs.pop_front();
    }
    if (worker_translator->TranslateAnalyzedShader(*translation)) {
      translation->GetOrCreateShaderModule();
    } else {
      REXLOG_ERROR("[native] Shader {:016X} did not translate",
                   translation->shader().ucode_data_hash());
    }
    std::lock_guard<std::mutex> lock(translation_mutex);
    translations_done.push_back(translation);
    translations_done_count.fetch_add(1, std::memory_order_release);
  }
}

VkPipeline Renderer::Impl::GetPipeline(const PipelineKey& key,
                                       VulkanShader::VulkanTranslation* vs,
                                       VulkanShader::VulkanTranslation* ps,
                                       VkShaderModule geometry, VkPipelineLayout layout,
                                       bool* pending_out) {
  *pending_out = false;
  DrainCompletedPipelines();
  uint64_t hash = XXH3_64bits(&key, sizeof(key));
  auto it = pipelines.find(hash);
  if (it != pipelines.end()) {
    return it->second;
  }
  if (pending_pipelines.count(hash)) {
    *pending_out = true;
    return VK_NULL_HANDLE;
  }
  PipelineJob job;
  job.hash = hash;
  job.key = key;
  job.vertex = vs->shader_module();
  job.pixel = ps ? ps->shader_module() : VK_NULL_HANDLE;
  job.geometry = geometry;
  job.layout = layout;
  if (REXCVAR_GET(nx_native_async_pipelines) && pipeline_thread.joinable()) {
    // Compiled on the worker. Until it is ready the draws that need it are
    // skipped, a frame or two of missing objects instead of a frozen frame.
    pending_pipelines.insert(hash);
    {
      std::lock_guard<std::mutex> lock(pipeline_mutex);
      pipeline_jobs.push_back(job);
    }
    pipeline_cv.notify_one();
    *pending_out = true;
    return VK_NULL_HANDLE;
  }
  VkPipeline pipeline = CreatePipeline(job);
  if (pipeline) {
    ++stats().pipelines_created;
    last_pipeline_time = std::chrono::steady_clock::now();
  }
  pipelines.emplace(hash, pipeline);
  return pipeline;
}

void Renderer::Impl::DrainCompletedPipelines() {
  if (!pipelines_completed_count.load(std::memory_order_acquire)) {
    return;
  }
  std::vector<std::pair<uint64_t, VkPipeline>> done;
  {
    std::lock_guard<std::mutex> lock(pipeline_mutex);
    done.swap(pipelines_completed);
    pipelines_completed_count.store(0, std::memory_order_release);
  }
  for (auto& [hash, pipeline] : done) {
    pending_pipelines.erase(hash);
    pipelines.emplace(hash, pipeline);
    if (pipeline) {
      ++stats().pipelines_created;
      last_pipeline_time = std::chrono::steady_clock::now();
    }
  }
}

void Renderer::Impl::PipelineThread() {
#if defined(__SWITCH__)
  // Below the game's threads and the ring: compiling is never urgent enough
  // to take a core from them.
  svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3F);
#endif
  while (true) {
    PipelineJob job;
    {
      std::unique_lock<std::mutex> lock(pipeline_mutex);
      pipeline_cv.wait(lock, [&] { return pipeline_stop || !pipeline_jobs.empty(); });
      if (pipeline_stop) {
        return;
      }
      job = pipeline_jobs.front();
      pipeline_jobs.pop_front();
    }
    VkPipeline pipeline = CreatePipeline(job);
    std::lock_guard<std::mutex> lock(pipeline_mutex);
    pipelines_completed.emplace_back(job.hash, pipeline);
    pipelines_completed_count.fetch_add(1, std::memory_order_release);
  }
}

// Builds the pipeline for a key. Only reads immutable state, so it runs on the
// pipeline worker as well as on the ring thread.
VkPipeline Renderer::Impl::CreatePipeline(const PipelineJob& job) {
  const PipelineKey& key = job.key;
  VkShaderModule geometry = job.geometry;
  VkPipelineLayout layout = job.layout;

  VkSpecializationMapEntry sign_entries[8];
  for (uint32_t i = 0; i < 8; ++i) {
    sign_entries[i] = {i, uint32_t(sizeof(uint32_t) * i), sizeof(uint32_t)};
  }
  VkSpecializationInfo signs = {8, sign_entries, sizeof(key.texture_signs), key.texture_signs};
  const VkSpecializationInfo* specialization = specialize_signs ? &signs : nullptr;

  VkPipelineShaderStageCreateInfo stages[3] = {};
  uint32_t stage_count = 0;
  stages[stage_count++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                           nullptr,
                           0,
                           VK_SHADER_STAGE_VERTEX_BIT,
                           job.vertex,
                           "main",
                           specialization};
  if (geometry) {
    stages[stage_count++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                             nullptr,
                             0,
                             VK_SHADER_STAGE_GEOMETRY_BIT,
                             geometry,
                             "main",
                             nullptr};
  }
  if (job.pixel) {
    stages[stage_count++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                             nullptr,
                             0,
                             VK_SHADER_STAGE_FRAGMENT_BIT,
                             job.pixel,
                             "main",
                             specialization};
  }

  VkPipelineVertexInputStateCreateInfo vertex_input = {};
  vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
  input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly.topology = VkPrimitiveTopology(key.topology);
  input_assembly.primitiveRestartEnable = key.primitive_restart ? VK_TRUE : VK_FALSE;
  VkPipelineViewportStateCreateInfo viewport = {};
  viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster = {};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.depthClampEnable = key.depth_clamp ? VK_TRUE : VK_FALSE;
  raster.polygonMode = key.polygon_line ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
  raster.cullMode = key.cull_mode;
  raster.frontFace =
      key.front_face_clockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.depthBiasEnable = key.depth_format ? VK_TRUE : VK_FALSE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample = {};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VkSampleCountFlagBits(key.samples);
  VkPipelineDepthStencilStateCreateInfo depth = {};
  depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depth.depthTestEnable = key.depth_test ? VK_TRUE : VK_FALSE;
  depth.depthWriteEnable = key.depth_write ? VK_TRUE : VK_FALSE;
  depth.depthCompareOp = VkCompareOp(key.depth_compare);
  depth.stencilTestEnable =
      key.stencil_test && HasStencil(VkFormat(key.depth_format)) ? VK_TRUE : VK_FALSE;
  depth.front.failOp = VkStencilOp(key.stencil_front[0]);
  depth.front.passOp = VkStencilOp(key.stencil_front[1]);
  depth.front.depthFailOp = VkStencilOp(key.stencil_front[2]);
  depth.front.compareOp = VkCompareOp(key.stencil_front[3]);
  depth.back.failOp = VkStencilOp(key.stencil_back[0]);
  depth.back.passOp = VkStencilOp(key.stencil_back[1]);
  depth.back.depthFailOp = VkStencilOp(key.stencil_back[2]);
  depth.back.compareOp = VkCompareOp(key.stencil_back[3]);
  VkPipelineColorBlendAttachmentState attachments[4] = {};
  uint32_t attachment_count = 0;
  VkFormat color_formats[4] = {};
  for (uint32_t i = 0; i < 4; ++i) {
    color_formats[i] = VkFormat(key.color_formats[i]);
    if (key.color_formats[i]) {
      attachment_count = i + 1;
    }
    VkPipelineColorBlendAttachmentState& a = attachments[i];
    uint32_t blend = key.blend[i];
    a.colorWriteMask = (key.write_masks >> (4 * i)) & 0xF;
    if (blend != 0x00010001) {
      reg::RB_BLENDCONTROL control;
      control.value = blend;
      a.blendEnable = VK_TRUE;
      a.srcColorBlendFactor = BlendFactorToVk(control.color_srcblend);
      a.dstColorBlendFactor = BlendFactorToVk(control.color_destblend);
      a.colorBlendOp = BlendOpToVk(control.color_comb_fcn);
      a.srcAlphaBlendFactor = BlendFactorToVk(control.alpha_srcblend);
      a.dstAlphaBlendFactor = BlendFactorToVk(control.alpha_destblend);
      a.alphaBlendOp = BlendOpToVk(control.alpha_comb_fcn);
    }
  }
  VkPipelineColorBlendStateCreateInfo blend = {};
  blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  blend.attachmentCount = attachment_count;
  blend.pAttachments = attachments;
  VkDynamicState dynamic_states[] = {
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
      VK_DYNAMIC_STATE_DEPTH_BIAS,
      VK_DYNAMIC_STATE_BLEND_CONSTANTS,
      VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
      VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
      VK_DYNAMIC_STATE_STENCIL_REFERENCE,
  };
  VkPipelineDynamicStateCreateInfo dynamic = {};
  dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic.dynamicStateCount = uint32_t(std::size(dynamic_states));
  dynamic.pDynamicStates = dynamic_states;
  VkPipelineRenderingCreateInfo rendering = {};
  rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  rendering.colorAttachmentCount = attachment_count;
  rendering.pColorAttachmentFormats = color_formats;
  rendering.depthAttachmentFormat = VkFormat(key.depth_format);
  // A depth-only image has no stencil aspect, even when its depth format is valid.
  rendering.stencilAttachmentFormat = HasStencil(VkFormat(key.depth_format))
                                          ? VkFormat(key.depth_format)
                                          : VK_FORMAT_UNDEFINED;

  VkGraphicsPipelineCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  info.pNext = &rendering;
  info.stageCount = stage_count;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &input_assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = layout;
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (dfn().vkCreateGraphicsPipelines(vk, driver_cache, 1, &info, nullptr, &pipeline) !=
      VK_SUCCESS) {
    REXLOG_ERROR("[native] Pipeline creation failed (VS {:016X}, PS {:016X})", key.vs_hash,
                 key.ps_hash);
    pipeline = VK_NULL_HANDLE;
  }
  return pipeline;
}

// --- Render targets ------------------------------------------------------------------

RenderTarget* Renderer::Impl::GetRenderTarget(const RenderTargetKey& requested_key,
                                            uint32_t min_height, bool discard_for_zcull) {
  RenderTargetKey key = requested_key;
  if (!key.is_depth) {
    // These guest aliases use identical host storage. Keep their contents in
    // one image; shader gamma/sign behavior still comes from the raw registers.
    auto format = xenos::GetStorageColorFormat(xenos::ColorRenderTargetFormat(key.format));
    if (format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
      format = xenos::ColorRenderTargetFormat::k_8_8_8_8;
    }
    key.format = uint32_t(format);
  }
  min_height = std::clamp(min_height, 16u, 2048u);
  const bool want_zcull = key.is_depth && REXCVAR_GET(nx_native_zcull);
  auto it = render_targets.find(key);
  RenderTarget* old = nullptr;
  bool replace_for_zcull = false;
  if (it != render_targets.end()) {
    old = it->second.get();
    old->last_used_frame = frame;
    // The caller has proved that the next guest clear overwrites every depth
    // and stencil sample, including padded rows. Growth must still preserve
    // the old contents via the transfer fallback.
    replace_for_zcull = discard_for_zcull && want_zcull && !old->zcull_compatible &&
                        old->height >= min_height;
    if (old->height >= min_height && !replace_for_zcull) {
      return old;
    }
  }
  auto rt = std::make_unique<RenderTarget>();
  rt->key = key;
  rt->width = std::max(key.pitch, 1u);
  rt->height = replace_for_zcull ? old->height : rex::align(min_height, 16u);
  rt->samples = VkSampleCountFlagBits(1u << key.msaa);
  rt->format = key.is_depth ? DepthFormatFor(key.format)
                            : ColorFormatToVk(xenos::ColorRenderTargetFormat(key.format));
  rt->last_used_frame = frame;
  const bool preserve_old = old && !replace_for_zcull;
  rt->zcull_compatible = want_zcull && !preserve_old;
  VkImageCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = rt->format;
  info.extent = {rt->width, rt->height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = rt->samples;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = (key.is_depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT
                             : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (!rt->zcull_compatible) {
    info.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  }
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkDeviceSize allocation_size = 0;
  if (!rex::ui::vulkan::util::CreateDedicatedAllocationImage(
          device, info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, rt->image,
          rt->memory, nullptr, &allocation_size)) {
    REXLOG_ERROR("[native] Could not create a {}x{} render target", rt->width, rt->height);
    return old;
  }
  VkImageAspectFlags aspect =
      key.is_depth ? (HasStencil(rt->format)
                          ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                          : VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT))
                   : VK_IMAGE_ASPECT_COLOR_BIT;
  VkImageViewCreateInfo view = {};
  view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view.image = rt->image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = rt->format;
  view.subresourceRange = rex::ui::vulkan::util::InitializeSubresourceRange(aspect);
  dfn().vkCreateImageView(vk, &view, nullptr, &rt->view);
  if (key.is_depth) {
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    dfn().vkCreateImageView(vk, &view, nullptr, &rt->sample_view);
    view.subresourceRange.aspectMask = aspect;
  }

  // Render targets live in GENERAL. A new one starts cleared, or with the
  // content of the smaller one it replaces.
  BeginRecording();
  EndPass();
  VkCommandBuffer c = cmd();
  VkImageMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcAccessMask = 0;
  const VkPipelineStageFlags init_stages =
      rt->zcull_compatible ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                           : VK_PIPELINE_STAGE_TRANSFER_BIT;
  const VkAccessFlags init_access = rt->zcull_compatible
                                       ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                                       : VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = init_access;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = rt->image;
  barrier.subresourceRange = view.subresourceRange;
  dfn().vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, init_stages,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
  if (rt->zcull_compatible) {
    // Attachment writes maintain persistent ZCULL metadata; a transfer clear
    // would require TRANSFER_DST and prevent its allocation in NVK.
    VkRenderingAttachmentInfo depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = rt->view;
    depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo rendering = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {rt->width, rt->height};
    rendering.layerCount = 1;
    rendering.pDepthAttachment = &depth;
    rendering.pStencilAttachment = HasStencil(rt->format) ? &depth : nullptr;
    dfn().vkCmdBeginRendering(c, &rendering);
    dfn().vkCmdEndRendering(c);
  } else if (key.is_depth) {
    VkClearDepthStencilValue clear = {1.0f, 0};
    vkCmdClearDepthStencilImage(c, rt->image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1,
                                      &view.subresourceRange);
  } else {
    VkClearColorValue clear = {};
    dfn().vkCmdClearColorImage(c, rt->image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1,
                               &view.subresourceRange);
  }
  if (preserve_old) {
    VkImageCopy copy = {};
    copy.srcSubresource = {aspect, 0, 0, 1};
    copy.dstSubresource = copy.srcSubresource;
    copy.extent = {std::min(old->width, rt->width), old->height, 1};
    VkMemoryBarrier memory_barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                      VK_ACCESS_TRANSFER_WRITE_BIT |
                                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                      VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    dfn().vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &memory_barrier, 0, nullptr,
                               0, nullptr);
    dfn().vkCmdCopyImage(c, old->image, VK_IMAGE_LAYOUT_GENERAL, rt->image,
                         VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
  }
  if (old) {
    // Destroyed once this frame is done on the GPU (waiting for idle here
    // was a visible hitch).
    retired_render_targets.push_back({frame, old->image, old->view, old->sample_view, old->memory});
  }
  VkMemoryBarrier after = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                           init_access,
                           VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
  dfn().vkCmdPipelineBarrier(cmd(), init_stages,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             0, 1, &after, 0, nullptr, 0, nullptr);
  RenderTarget* result = rt.get();
  render_targets[key] = std::move(rt);
  ++stats().render_targets_created;
  ++rt_generation;
  REXLOG_INFO("[native] Render target {}x{} {} format {} at tile {}, {}x host MSAA, "
              "depth_guest_samples={}, zcull_eligible={}, allocation_bytes={}",
              result->width, result->height, key.is_depth ? "depth" : "color", key.format,
              key.base_tiles, 1u << key.msaa, key.is_depth ? 1u << key.guest_msaa : 0,
              result->zcull_compatible, allocation_size);
  return result;
}

bool Renderer::Impl::BeginPass(const PassTargets& targets, const VkClearValue* color0_clear,
                               const VkClearValue* depth_clear) {
  // A load clear needs a new rendering scope even when attachments match.
  if (in_pass && (color0_clear || depth_clear)) EndPass();
  if (in_pass) {
    // Track whether the previous superset check would have reused an
    // incompatible dynamic-rendering scope.
    bool compatible = true;
    for (uint32_t i = 0; i < 4; ++i) {
      if (targets.color[i] && targets.color[i] != pass.color[i]) compatible = false;
    }
    if (targets.depth && targets.depth != pass.depth) compatible = false;
    if (compatible && !(targets == pass) && REXCVAR_GET(nx_native_trace_output) &&
        trace_attachment_transitions.size() < 8) {
      std::array<uint32_t, 50> trace_key{};
      size_t trace_offset = 0;
      auto append_target_key = [&trace_key, &trace_offset](const RenderTarget* target) {
        if (target) {
          trace_key[trace_offset++] = target->key.base_tiles;
          trace_key[trace_offset++] = target->key.pitch;
          trace_key[trace_offset++] = target->key.format;
          trace_key[trace_offset++] = target->key.msaa;
          trace_key[trace_offset++] = target->key.is_depth;
        } else {
          for (uint32_t i = 0; i < 5; ++i) trace_key[trace_offset++] = UINT32_MAX;
        }
      };
      for (RenderTarget* color : pass.color) append_target_key(color);
      append_target_key(pass.depth);
      for (RenderTarget* color : targets.color) append_target_key(color);
      append_target_key(targets.depth);
      if (trace_attachment_transitions.insert(trace_key).second) {
        auto base_tiles = [](const RenderTarget* target) {
          return target ? target->key.base_tiles : UINT32_MAX;
        };
        REXLOG_INFO(
            "[native][trace] split dynamic-rendering scope for exact attachment match: "
            "old_color_bases=[{:X},{:X},{:X},{:X}] old_depth={:X} -> "
            "new_color_bases=[{:X},{:X},{:X},{:X}] new_depth={:X}",
            base_tiles(pass.color[0]), base_tiles(pass.color[1]), base_tiles(pass.color[2]),
            base_tiles(pass.color[3]), base_tiles(pass.depth), base_tiles(targets.color[0]),
            base_tiles(targets.color[1]), base_tiles(targets.color[2]),
            base_tiles(targets.color[3]), base_tiles(targets.depth));
      }
    }
    // Pipelines must match the complete active attachment set, including its
    // gaps and the depth/stencil attachment.
    if (targets == pass) {
      return true;
    }
    EndPass();
  }
  FlushResolveBarrier();
  ProfileMark((kProfilePass << 60) | (ProfileTargetBits(targets.color[0]) << 24) |
              ProfileTargetBits(targets.depth));
  pass = targets;
  VkRenderingAttachmentInfo colors[4] = {};
  uint32_t color_count = 0;
  uint32_t width = UINT32_MAX, height = UINT32_MAX;
  for (uint32_t i = 0; i < 4; ++i) {
    colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    if (RenderTarget* rt = targets.color[i]) {
      colors[i].imageView = rt->view;
      colors[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      colors[i].loadOp = i == 0 && color0_clear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                               : VK_ATTACHMENT_LOAD_OP_LOAD;
      if (i == 0 && color0_clear) colors[i].clearValue = *color0_clear;
      colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      color_count = i + 1;
      width = std::min(width, rt->width);
      height = std::min(height, rt->height);
    }
  }
  VkRenderingAttachmentInfo depth = {};
  depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  if (targets.depth) {
    depth.imageView = targets.depth->view;
    depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    depth.loadOp = depth_clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    if (depth_clear) depth.clearValue = *depth_clear;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    width = std::min(width, targets.depth->width);
    height = std::min(height, targets.depth->height);
  }
  if (width == UINT32_MAX) {
    return false;
  }
  VkRenderingInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  info.renderArea.extent = {width, height};
  info.layerCount = 1;
  info.colorAttachmentCount = color_count;
  info.pColorAttachments = colors;
  info.pDepthAttachment = targets.depth ? &depth : nullptr;
  info.pStencilAttachment =
      targets.depth && HasStencil(targets.depth->format) ? &depth : nullptr;
  dfn().vkCmdBeginRendering(cmd(), &info);
  in_pass = true;
  pass_extent = {width, height};
  ++stats().render_passes;
  return true;
}

// --- System constants ----------------------------------------------------------------

void Renderer::Impl::UpdateSystemConstants(bool primitive_polygonal,
                                           const draw_util::ViewportInfo& viewport,
                                           uint32_t used_texture_mask, uint32_t,
                                           const VulkanShader* vs, const VulkanShader* ps) {
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
        1.0f / float(std::max(viewport.xy_extent[1], 1u));
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

// --- Draws -------------------------------------------------------------------------------

// The render state of a draw, rebuilt only when a register that can change it
// is written (Renderer::state_generation_), the shaders or the primitive type
// change, or a render target is created. Consecutive draws almost always share
// it, and rebuilding it per draw was most of the renderer's CPU time.
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
  const auto& properties = device->properties();
  auto fail = [&](uint32_t reason) {
    st.skip_reason = reason;
    return false;
  };

  xenos::EdramMode edram_mode = r.Get<reg::RB_MODECONTROL>().edram_mode;
  auto surface_info = r.Get<reg::RB_SURFACE_INFO>();
  if (!surface_info.surface_pitch) return fail(0);
  VulkanShader* vs = active_vs;
  if (!vs) return fail(1);
  if (!vs->is_ucode_analyzed()) vs->AnalyzeUcode(disasm);
  if (vs->memexport_eM_written()) return fail(2);
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(r);
  if (!draw_util::IsRasterizationPotentiallyDone(r, primitive_polygonal)) return fail(3);
  VulkanShader* ps = nullptr;
  if (edram_mode == xenos::EdramMode::kColorDepth && active_ps) {
    ps = active_ps;
    if (!ps->is_ucode_analyzed()) ps->AnalyzeUcode(disasm);
    if (ps->memexport_eM_written()) return fail(2);
    if (!draw_util::IsPixelShaderNeededWithRasterization(*ps, r)) ps = nullptr;
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
      static_cast<VulkanShader::VulkanTranslation*>(vs->GetOrCreateTranslation(vs_mod.value));
  auto* ps_translation =
      ps ? static_cast<VulkanShader::VulkanTranslation*>(ps->GetOrCreateTranslation(ps_mod.value))
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
  }
  // Creating a render target above bumped rt_generation; this setup is built
  // against the new ones.
  st.rt_generation = rt_generation;
  if (targets.Empty()) return fail(7);

  // Layout.
  st.vs = vs;
  st.ps = ps;
  const VulkanShader* stage_shaders[2] = {vs, ps};
  for (uint32_t stage = 0; stage < 2; ++stage) {
    const VulkanShader* shader = stage_shaders[stage];
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
    for (const VulkanShader* shader : stage_shaders) {
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
  VkShaderModule geometry_module = VK_NULL_HANDLE;
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
      it = geometry_shaders.emplace(gs_key.key, CreateGeometryShader(device, gs_key)).first;
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
  // D3D9 specifies a normalized float bias. Core Vulkan's D24 quantum may
  // legally be twice one ULP: the viewmodel's -0.5 bias then becomes -1.0,
  // clamping its surfaces to zero and destroying their depth ordering.
  // Exact FORMAT representation fixes that quantum for a real D24 host target.
  // Float host targets / guest float24 keep the existing exponent-based path.
  st.bias_exact_unorm = guest_depth_unorm && targets.depth &&
      targets.depth->format == VK_FORMAT_D24_UNORM_S8_UINT &&
      properties.depthBiasControl && properties.depthBiasExact && dfn().vkCmdSetDepthBias2EXT;
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

void Renderer::Impl::Draw(const uint32_t* immediate_indices, uint32_t immediate_index_words) {
  const RegisterFile& r = *regs;
  // The reasons, in order: 0 no surface pitch, 1 no vertex shader, 2 memory
  // export, 3 no rasterization, 4 primitive type, 5 no geometry shaders,
  // 6 translation, 7 no render target, 8 vertex/index data, 9 stream full,
  // 10 layout, 11 descriptors, 12 geometry shader, 13 pipeline, 14 pass,
  // 15 immediate indices.
  auto skip = [&](uint32_t reason) {
    ++stats().draws_skipped;
    ++stats().skipped_by_reason[reason & 15];
  };

  auto initiator = r.Get<reg::VGT_DRAW_INITIATOR>();
  uint32_t guest_count = initiator.num_indices;
  if (!guest_count) {
    return;
  }
  // The render state: the same as the previous draw (nothing changed), or
  // one seen before (looked up by the hash of the state registers, the
  // shaders and the primitive setup), or built now.
  if (!(setup.valid && setup.state_generation == owner->state_generation_ &&
        setup.active_vs == active_vs && setup.active_ps == active_ps &&
        setup.initiator_state == (initiator.value & 0xFFFF) &&
        setup.rt_generation == rt_generation &&
        (!specialize_signs || setup.fetch_generation == owner->fetch_generation_))) {
    if (setup_cache_rt_generation != rt_generation) {
      // Setups hold render target pointers.
      setup_cache.clear();
      setup_cache_rt_generation = rt_generation;
    }
    uint64_t key = owner->state_hash_ ^
                   Renderer::Mix(0x10000, uint32_t(reinterpret_cast<uintptr_t>(active_vs))) ^
                   Renderer::Mix(0x10001, uint32_t(reinterpret_cast<uintptr_t>(active_vs) >> 32)) ^
                   Renderer::Mix(0x10002, uint32_t(reinterpret_cast<uintptr_t>(active_ps))) ^
                   Renderer::Mix(0x10003, uint32_t(reinterpret_cast<uintptr_t>(active_ps) >> 32)) ^
                   Renderer::Mix(0x10004, (initiator.value & 0xFFFF) | 0x80000000u);
    if (specialize_signs) {
      // The analysis lists texture-fetch instructions, not unique slots. XORing
      // the same slot twice would erase its signs from this key and could reuse
      // a pipeline specialized for a previous texture's signedness.
      uint32_t signed_slots = 0;
      for (VulkanShader* shader : {active_vs, active_ps}) {
        if (!shader || !shader->is_ucode_analyzed()) continue;
        for (const auto& binding : shader->texture_bindings()) {
          signed_slots |= uint32_t(1) << binding.fetch_constant;
        }
      }
      uint32_t slot;
      while (rex::bit_scan_forward(signed_slots, &slot)) {
        signed_slots &= ~(uint32_t(1) << slot);
        key ^= Renderer::Mix(
            0x10010 + slot,
            0x100u | rex::graphics::texture_util::SwizzleSigns(r.GetTextureFetch(slot)));
      }
    }
    auto found = setup_cache.find(key);
    if (found != setup_cache.end() && found->second.active_vs == active_vs &&
        found->second.active_ps == active_ps &&
        found->second.initiator_state == (initiator.value & 0xFFFF)) {
      setup = found->second;
      setup.state_generation = owner->state_generation_;
      setup.fetch_generation = owner->fetch_generation_;
      ++setup_generation;
    } else {
      ++stats().setups_built;
      ++setup_generation;
      BuildDrawSetup(initiator.value);
      if (setup.valid) {
        if (setup_cache.size() >= 4096) setup_cache.clear();
        setup_cache[key] = setup;
      }
    }
  }
  DrawSetup& st = setup;
  if (st.skip_reason != UINT32_MAX) {
    return skip(st.skip_reason);
  }
  VulkanShader* vs = st.vs;
  VulkanShader* ps = st.ps;
  const PassTargets& targets = st.targets;
  const PipelineLayout* layout = st.layout;

  // Indices.
  BeginRecording();
  DrawStreamScope draw_stream_scope(*this);
  auto dma_size = r.Get<reg::VGT_DMA_SIZE>();
  bool indexed = initiator.source_select != xenos::SourceSelect::kAutoIndex;
  bool index32 = initiator.index_size == xenos::IndexFormat::kInt32;
  bool reset_enabled = st.reset_enabled;
  bool convert_fan = st.convert_fan, convert_loop = st.convert_loop;
  uint32_t reset_index = r.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
  const uint8_t* guest_indices = nullptr;
  xenos::Endian index_endian = dma_size.swap_mode;
  uint32_t dma_base = 0;
  if (indexed) {
    if (initiator.source_select == xenos::SourceSelect::kDMA) {
      dma_base = r[rex::graphics::XE_GPU_REG_VGT_DMA_BASE] & 0x1FFFFFFF;
      uint32_t bytes = guest_count * (index32 ? 4 : 2);
      if (uint64_t(dma_base) + bytes > 0x20000000u) {
        return skip(8);
      }
      guest_indices = memory->TranslatePhysical<const uint8_t*>(dma_base);
    } else if (immediate_indices) {
      guest_indices = reinterpret_cast<const uint8_t*>(immediate_indices);
      index_endian = xenos::Endian::kNone;
      guest_count = std::min(guest_count, immediate_index_words * (index32 ? 1u : 2u));
    } else {
      return skip(15);
    }
  }
  auto read_index = [&](uint32_t i) -> uint32_t {
    if (!indexed) {
      return i;
    }
    if (index32) {
      uint32_t v;
      std::memcpy(&v, guest_indices + size_t(i) * 4, 4);
      return xenos::GpuSwap(v, index_endian);
    }
    uint16_t v;
    std::memcpy(&v, guest_indices + size_t(i) * 2, 2);
    return xenos::GpuSwap(v, index_endian);
  };
  uint32_t draw_count = guest_count;
  bool host_indexed = indexed || convert_fan || convert_loop;
  VkIndexType index_type = index32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
  VkDeviceSize index_offset = 0;
  if (host_indexed) {
    if (convert_fan) {
      if (guest_count < 3) return;
      draw_count = (guest_count - 2) * 3;
    } else if (convert_loop) {
      if (guest_count < 2) return;
      draw_count = guest_count + 1;
    }
    if (!indexed) {
      index_type = VK_INDEX_TYPE_UINT32;
    }
    uint32_t index_size = index_type == VK_INDEX_TYPE_UINT32 ? 4 : 2;
    // Index buffers in guest memory are usually static: reuse the converted
    // copy while the guest data is the same (checked once per frame).
    bool reused = false;
    VertexRangeKey index_key = {};
    if (dma_base && !convert_fan && !convert_loop) {
      uint32_t bytes = guest_count * (index32 ? 4 : 2);
      VertexHash& known = vertex_hashes[(uint64_t(dma_base) << 32) | bytes];
      RangeHash(known, guest_indices, bytes, dma_base);
      // The conversion also depends on the byte order and the reset index.
      index_key = {dma_base, bytes | 0x80000000u,
                   known.hash ^ (uint64_t(index_endian) << 60) ^
                       (reset_enabled ? (uint64_t(reset_index) << 8) | 1 : 0)};
      uint64_t found = FindRange(index_key);
      if (found != UINT64_MAX && stream_head - found < kStreamSize / 2) {
        index_offset = VkDeviceSize(found % kStreamSize);
        PinDrawStreamPosition(found);
        reused = true;
      }
    }
    if (!reused) {
      uint8_t* out = Allocate(VkDeviceSize(draw_count) * index_size, 4, index_offset);
      if (!out) {
        return skip(9);
      }
      if (index_key.address) {
        AddRange(index_key, stream_head - VkDeviceSize(draw_count) * index_size);
      }
      uint32_t host_reset = index_size == 4 ? 0xFFFFFFFFu : 0xFFFFu;
      auto put = [&](uint32_t i, uint32_t v) {
        if (reset_enabled && v == reset_index) {
          v = host_reset;
        }
        if (index_size == 4) {
          std::memcpy(out + size_t(i) * 4, &v, 4);
        } else {
          uint16_t h = uint16_t(v);
          std::memcpy(out + size_t(i) * 2, &h, 2);
        }
      };
      if (convert_fan) {
        uint32_t first = read_index(0);
        for (uint32_t t = 0; t + 2 < guest_count; ++t) {
          put(t * 3 + 0, first);
          put(t * 3 + 1, read_index(t + 1));
          put(t * 3 + 2, read_index(t + 2));
        }
      } else if (convert_loop) {
        for (uint32_t i = 0; i < guest_count; ++i) put(i, read_index(i));
        put(guest_count, read_index(0));
      } else if (!reset_enabled && index_endian == xenos::Endian::kNone) {
        std::memcpy(out, guest_indices, size_t(draw_count) * index_size);
      } else if (!reset_enabled && index_size == 2 && index_endian == xenos::Endian::k8in16) {
        rex::memory::copy_and_swap_16_unaligned(out, guest_indices, draw_count);
      } else if (!reset_enabled && index_size == 4 && index_endian == xenos::Endian::k8in32) {
        rex::memory::copy_and_swap_32_unaligned(out, guest_indices, draw_count);
      } else {
        for (uint32_t i = 0; i < draw_count; ++i) put(i, read_index(i));
      }
    }
  }

  // Fetch constants: the vertex buffers the shader reads are pointed at their
  // copies in the stream. Only slots written since they were last resolved,
  // or not yet resolved this frame, are looked at again.
  const Shader::ConstantRegisterMap& vs_constants = vs->constant_register_map();
  if (fetch_words_frame != frame || fetch_words_submission != submission_number) {
    // New frame: vertex data is checked for changes again. New submission:
    // copies in older submissions may be reused, but the bookkeeping is
    // simpler starting over.
    fetch_resolved_frame.fill(0);
    fetch_words_frame = frame;
    fetch_words_submission = submission_number;
  }
  uint32_t dirty_slots = owner->vertex_fetch_dirty_;
  owner->vertex_fetch_dirty_ = 0;
  if (dirty_slots) {
    for (uint32_t i = 0; i < 96; ++i) {
      if (dirty_slots & (1u << (i / 3))) fetch_resolved_frame[i] = 0;
    }
    // Texture words are copied as they are; vertex words get rewritten below.
    for (uint32_t slot = 0; slot < 32; ++slot) {
      if (dirty_slots & (1u << slot)) {
        std::memcpy(&fetch_words[slot * 6],
                    &r[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 6],
                    6 * sizeof(uint32_t));
      }
    }
  }
  for (uint32_t word = 0; word < 3; ++word) {
    uint32_t bits = vs_constants.vertex_fetch_bitmap[word];
    uint32_t bit;
    while (rex::bit_scan_forward(bits, &bit)) {
      bits &= ~(1u << bit);
      uint32_t fetch_index = word * 32 + bit;
      draw_stream_fetch_slots[fetch_index] = 1;
      if (fetch_resolved_frame[fetch_index] == frame) {
        // The shader words still point at this stream copy even when the
        // per-frame fetch resolution is a fast hit.
        PinDrawStreamPosition(fetch_resolved_position[fetch_index]);
        continue;
      }
      uint32_t guest_dword0 =
          r[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_index * 2];
      uint32_t dword1 =
          r[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_index * 2 + 1];
      uint32_t& dword0 = fetch_words[fetch_index * 2];
      dword0 = guest_dword0;
      uint32_t address = guest_dword0 & 0x1FFFFFFC;
      uint32_t size = (dword1 >> 2) & 0xFFFFFF;  // in words
      uint32_t bytes = std::min(size * 4, 16u << 20);
      if (!bytes || uint64_t(address) + bytes > 0x20000000u) {
        return skip(8);
      }
      const uint8_t* source = memory->TranslatePhysical<const uint8_t*>(address);
      VertexHash& known = vertex_hashes[(uint64_t(address) << 32) | bytes];
      RangeHash(known, source, bytes, address);
      VertexRangeKey range_key = {address, bytes, known.hash};
      uint64_t found = FindRange(range_key);
      VkDeviceSize range_offset;
      uint64_t range_position;
      if (found != UINT64_MAX && stream_head - found < kStreamSize / 2) {
        range_offset = VkDeviceSize(found % kStreamSize);
        range_position = found;
        PinDrawStreamPosition(range_position);
      } else {
        uint8_t* out = Allocate(bytes, 16, range_offset);
        if (!out) return skip(9);
        std::memcpy(out, source, bytes);
        stats().vertex_bytes_copied += bytes;
        range_position = stream_head - bytes;
        AddRange(range_key, range_position);
      }
      dword0 = (guest_dword0 & 0x3) | uint32_t(range_offset);
      fetch_resolved_position[fetch_index] = range_position;
      fetch_resolved_frame[fetch_index] = frame;
    }
  }
  if (vertex_ranges.size() > 65536) {
    vertex_ranges.clear();
  }
  if (vertex_hashes.size() > 65536) {
    vertex_hashes.clear();
  }
  VkDeviceSize fetch_offset;
  if (last_fetch.submission == submission_number &&
      !std::memcmp(last_fetch_words, fetch_words.data(), sizeof(last_fetch_words))) {
    fetch_offset = last_fetch.offset;
    PinDrawStreamPosition(last_fetch.stream_position);
  } else {
    uint8_t* fetch_out = Allocate(kFetchConstantsBytes, ubo_alignment, fetch_offset);
    if (!fetch_out) return skip(9);
    std::memcpy(fetch_out, fetch_words.data(), kFetchConstantsBytes);
    std::memcpy(last_fetch_words, fetch_words.data(), sizeof(last_fetch_words));
    last_fetch = {submission_number, 0, nullptr, frame, fetch_offset,
                  stream_head - kFetchConstantsBytes};
  }

  auto write_float_constants = [&](const Shader::ConstantRegisterMap& map, uint32_t base_register,
                                   VkDeviceSize& offset) -> bool {
    // Only the constants the shader reads: the binding's fixed range reads on
    // into whatever follows (never used), which the stream's guard keeps
    // inside the buffer. Allocating the full 4 KB per draw filled the stream
    // and made it wait for the GPU.
    VkDeviceSize bytes = VkDeviceSize(std::max(map.float_count, 1u)) * 4 * sizeof(float);
    uint8_t* out = Allocate(bytes, ubo_alignment, offset);
    if (!out) return false;
    for (uint32_t i = 0; i < 4; ++i) {
      uint64_t bits = map.float_bitmap[i];
      uint32_t bit;
      while (rex::bit_scan_forward(bits, &bit)) {
        bits &= ~(uint64_t(1) << bit);
        std::memcpy(out, &r[base_register + (i << 8) + (bit << 2)], 4 * sizeof(float));
        out += 4 * sizeof(float);
      }
    }
    return true;
  };
  VkDeviceSize vs_float_offset, ps_float_offset, bool_loop_offset, system_offset;
  const VkDeviceSize vs_float_bytes =
      VkDeviceSize(std::max(vs_constants.float_count, 1u)) * 4 * sizeof(float);
  if (Reusable(last_vs_float, owner->vs_float_generation_, vs)) {
    vs_float_offset = last_vs_float.offset;
    PinDrawStreamPosition(last_vs_float.stream_position);
  } else {
    if (!write_float_constants(vs_constants, rex::graphics::XE_GPU_REG_SHADER_CONSTANT_000_X,
                               vs_float_offset)) {
      return skip(9);
    }
    last_vs_float = {submission_number, owner->vs_float_generation_, vs, frame, vs_float_offset,
                     stream_head - vs_float_bytes};
  }
  if (ps) {
    const VkDeviceSize ps_float_bytes =
        VkDeviceSize(std::max(ps->constant_register_map().float_count, 1u)) * 4 * sizeof(float);
    if (Reusable(last_ps_float, owner->ps_float_generation_, ps)) {
      ps_float_offset = last_ps_float.offset;
      PinDrawStreamPosition(last_ps_float.stream_position);
    } else {
      if (!write_float_constants(ps->constant_register_map(),
                                 rex::graphics::XE_GPU_REG_SHADER_CONSTANT_256_X,
                                 ps_float_offset)) {
        return skip(9);
      }
      last_ps_float = {submission_number, owner->ps_float_generation_, ps, frame,
                       ps_float_offset, stream_head - ps_float_bytes};
    }
  } else {
    ps_float_offset = vs_float_offset;
  }
  if (Reusable(last_bool_loop, owner->bool_loop_generation_, nullptr)) {
    bool_loop_offset = last_bool_loop.offset;
    PinDrawStreamPosition(last_bool_loop.stream_position);
  } else {
    uint8_t* bool_loop = Allocate(kBoolLoopConstantsBytes, ubo_alignment, bool_loop_offset);
    if (!bool_loop) return skip(9);
    std::memcpy(bool_loop, &r[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
                kBoolLoopConstantsBytes);
    last_bool_loop = {submission_number, owner->bool_loop_generation_, nullptr, frame,
                      bool_loop_offset, stream_head - kBoolLoopConstantsBytes};
  }

  // Textures and samplers, reused while no fetch slot they come from was
  // written, within one frame (content changes are looked for once a frame)
  // and one submission (uploads go in its setup command buffer).
  upload.setup_commands = current().setup;
  upload.submission = submission_number;
  const VulkanShader* stage_shaders[2] = {vs, ps};
  // Keyed by the shaders and the fetch constants of the slots they read,
  // valid for a frame: materials repeat across the draws of a frame, and a hit
  // skips every texture and sampler lookup.
  uint32_t slot_mask = 0;
  for (const VulkanShader* shader : stage_shaders) {
    if (!shader) continue;
    for (const auto& b : shader->GetTextureBindingsAfterTranslation()) {
      slot_mask |= 1u << b.fetch_constant;
    }
    for (const auto& b : shader->GetSamplerBindingsAfterTranslation()) {
      slot_mask |= 1u << b.fetch_constant;
    }
  }
  uint64_t tb_key = XXH3_64bits_withSeed(&layout, sizeof(layout),
                                         uint64_t(reinterpret_cast<uintptr_t>(vs)) ^
                                             (uint64_t(reinterpret_cast<uintptr_t>(ps)) << 1));
  {
    uint32_t words[6 * 32];
    uint32_t count = 0;
    uint32_t remaining = slot_mask;
    uint32_t slot;
    while (rex::bit_scan_forward(remaining, &slot)) {
      remaining &= ~(1u << slot);
      std::memcpy(&words[count],
                  &r[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 6],
                  6 * sizeof(uint32_t));
      count += 6;
    }
    tb_key = XXH3_64bits_withSeed(words, count * sizeof(uint32_t), tb_key ^ slot_mask);
  }
  if (tb_cache_frame != frame) {
    if (tb_cache.size() > 8192) tb_cache.clear();
    tb_cache_frame = frame;
  }
  TextureBindings& tb = tb_cache[tb_key];
  // Any view destroyed anywhere moves views_epoch, which while a level
  // streams in is most frames: instead of rebuilding everything then, the
  // bindings check that their own views are still there.
  auto views_alive = [&]() {
    if (tb.views_epoch == textures.views_epoch()) return true;
    if (tb.view_count > 24) return false;
    for (uint32_t i = 0; i < tb.view_count; ++i) {
      if (!textures.ViewAlive(tb.views[i], tb.view_ids[i])) return false;
    }
    tb.views_epoch = textures.views_epoch();
    return true;
  };
  bool tb_hit = tb.valid && tb.vs == vs && tb.ps == ps && tb.layout == layout &&
                tb.set_generation == set_cache_generation &&
                tb.answers_epoch == textures.answers_epoch() && views_alive() &&
                ((tb.frame == frame && tb.submission == submission_number) ||
                 tb.valid_until >= frame);
  if (tb_hit && (tb.frame != frame || tb.submission != submission_number)) {
    for (void* texture : tb.textures) {
      textures.Touch(texture, submission_number);
    }
    tb.frame = frame;
    tb.submission = submission_number;
  }
  if (!tb_hit) {
    TextureTimer texture_timer(frame_texture_ns);
    ++stats().texture_bindings_built;
    tb = TextureBindings();
    tb.valid_until = UINT64_MAX;
    for (uint32_t stage = 0; stage < 2; ++stage) {
      const VulkanShader* shader = stage_shaders[stage];
      if (!shader || (!st.counts[stage * 2] && !st.counts[stage * 2 + 1])) continue;
      const auto& texture_bindings = shader->GetTextureBindingsAfterTranslation();
      const auto& sampler_bindings = shader->GetSamplerBindingsAfterTranslation();
      VkDescriptorImageInfo infos[64];
      uint32_t info_count = 0;
      for (const auto& binding : texture_bindings) {
        if (info_count >= 64) break;
        tb.used_mask |= 1u << binding.fetch_constant;
        VkDescriptorImageInfo& info = infos[info_count++];
        info.sampler = VK_NULL_HANDLE;
        info.imageView = SlotView(binding.fetch_constant, binding.dimension,
                                  bool(binding.is_signed));
        info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        tb.valid_until = std::min(tb.valid_until, slot_valid_until);
        if (tb.view_count < 24) {
          tb.views[tb.view_count] = info.imageView;
          tb.view_ids[tb.view_count] = textures.ViewId(info.imageView);
        }
        ++tb.view_count;
        if (slot_texture &&
            std::find(tb.textures.begin(), tb.textures.end(), slot_texture) == tb.textures.end()) {
          tb.textures.push_back(slot_texture);
        }
      }
      for (const auto& binding : sampler_bindings) {
        if (info_count >= 64) break;
        tb.slots |= 1u << binding.fetch_constant;
        VkDescriptorImageInfo& info = infos[info_count++];
        info.sampler = SlotSampler(binding);
        info.imageView = VK_NULL_HANDLE;
        info.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      }
      VkDescriptorSetLayout set_layout =
          stage ? layout->textures_pixel : layout->textures_vertex;
      // Keyed by view ids, never reused, rather than handles, which are: the
      // cache was dropped whenever any view was destroyed, which while a level
      // streams in is most frames, and every material then allocated and
      // wrote new sets (10-50 ms in the frames that dropped).
      uint64_t key_words[64];
      for (uint32_t i = 0; i < info_count; ++i) {
        key_words[i] = infos[i].imageView ? textures.ViewId(infos[i].imageView)
                                          : (uint64_t(1) << 63) ^
                                                uint64_t(reinterpret_cast<uintptr_t>(
                                                    infos[i].sampler));
      }
      uint64_t set_key =
          XXH3_64bits_withSeed(key_words, sizeof(uint64_t) * info_count, uint64_t(set_layout));
      auto cached = texture_set_cache.find(set_key);
      if (cached != texture_set_cache.end()) {
        tb.sets[stage] = cached->second;
        continue;
      }
      if (texture_set_cache.size() >= 16384) {
        FlushSetCache();
      }
      VkDescriptorSet set = AllocatePersistentSet(set_layout);
      if (!set) return skip(11);
      texture_set_cache.emplace(set_key, set);
      VkWriteDescriptorSet writes[2] = {};
      uint32_t write_count = 0;
      if (!texture_bindings.empty()) {
        VkWriteDescriptorSet& w = writes[write_count++];
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = 0;
        w.descriptorCount = uint32_t(texture_bindings.size());
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w.pImageInfo = infos;
      }
      if (!sampler_bindings.empty()) {
        VkWriteDescriptorSet& w = writes[write_count++];
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = uint32_t(texture_bindings.size());
        w.descriptorCount = uint32_t(sampler_bindings.size());
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w.pImageInfo = infos + texture_bindings.size();
      }
      dfn().vkUpdateDescriptorSets(vk, write_count, writes, 0, nullptr);
      tb.sets[stage] = set;
    }
    tb.slots |= tb.used_mask;
    tb.valid = true;
    tb.frame = frame;
    tb.submission = submission_number;
    tb.vs = vs;
    tb.ps = ps;
    tb.layout = layout;
    tb.views_epoch = textures.views_epoch();
    tb.set_generation = set_cache_generation;
    tb.answers_epoch = textures.answers_epoch();
    // Only the slots these bindings read are cleared: another draw's
    // bindings may still need to see the others as written.
    owner->texture_fetch_dirty_ &= ~tb.slots;
  }

  // System constants come from the render state, the texture fetch
  // constants and the vertex index registers.
  bool system_same = last_system.submission == submission_number &&
                     system_inputs.setup_generation == setup_generation &&
                     system_inputs.fetch_generation == owner->fetch_generation_ &&
                     system_inputs.index_generation == owner->index_generation_ &&
                     system_inputs.used_mask == tb.used_mask;
  if (!system_same) {
    UpdateSystemConstants(st.primitive_polygonal, st.viewport, tb.used_mask, 0, vs, ps);
    system_inputs = {setup_generation, owner->fetch_generation_, owner->index_generation_,
                     tb.used_mask};
    system_same = last_system.submission == submission_number &&
                  !std::memcmp(&last_system_constants, &system_constants,
                               sizeof(system_constants));
  }
  if (system_same) {
    system_offset = last_system.offset;
    PinDrawStreamPosition(last_system.stream_position);
  } else {
    uint8_t* system_out =
        Allocate(sizeof(SpirvShaderTranslator::SystemConstants), ubo_alignment, system_offset);
    if (!system_out) return skip(9);
    std::memcpy(system_out, &system_constants, sizeof(system_constants));
    std::memcpy(&last_system_constants, &system_constants, sizeof(system_constants));
    last_system = {submission_number, 0, nullptr, frame, system_offset,
                   stream_head - sizeof(SpirvShaderTranslator::SystemConstants)};
  }

  // Texture staging can fail the active draw's overlap guard even though the
  // texture cache returns a null view as its ordinary allocation fallback.
  if (draw_stream_failed) return skip(9);

  // Texture uploads or the constant allocation above may have submitted the
  // old command buffers and started a new submission. Pin all bindings to the
  // submission that will actually record this draw, so a later upload cannot
  // overwrite an image already referenced by this submission's main commands.
  for (void* texture : tb.textures) {
    textures.Touch(texture, submission_number);
  }
  tb.frame = frame;
  tb.submission = submission_number;

  // Record. BeginPass does not allocate stream data or rotate submissions.
  if (!BeginPass(targets)) return skip(14);
  DrawCommand d;
  d.commands = current().main;
  d.flags = 0;
  d.layout = layout->layout;
  if (bound_pipeline != st.pipeline) {
    d.flags |= kDrawPipeline;
    d.pipeline = st.pipeline;
    bound_pipeline = st.pipeline;
  }
  if (bound_layout != layout->layout || !bound_static_sets) {
    d.flags |= kDrawSharedSet;
    d.set_shared = set_shared;
    bound_layout = layout->layout;
    bound_static_sets = true;
    bound_offsets_valid = false;
    bound_texture_sets[0] = bound_texture_sets[1] = VK_NULL_HANDLE;
  }
  uint32_t* dynamic_offsets = d.offsets;
  dynamic_offsets[SpirvShaderTranslator::kConstantBufferSystem] = uint32_t(system_offset);
  dynamic_offsets[SpirvShaderTranslator::kConstantBufferFloatVertex] = uint32_t(vs_float_offset);
  dynamic_offsets[SpirvShaderTranslator::kConstantBufferFloatPixel] = uint32_t(ps_float_offset);
  dynamic_offsets[SpirvShaderTranslator::kConstantBufferBoolLoop] = uint32_t(bool_loop_offset);
  dynamic_offsets[SpirvShaderTranslator::kConstantBufferFetch] = uint32_t(fetch_offset);
  if (!bound_offsets_valid || std::memcmp(bound_offsets, d.offsets, sizeof(d.offsets))) {
    d.flags |= kDrawConstants;
    d.set_constants = set_constants;
    std::memcpy(bound_offsets, d.offsets, sizeof(d.offsets));
    bound_offsets_valid = true;
  }
  if (tb.sets[0] && tb.sets[0] != bound_texture_sets[0]) {
    d.flags |= kDrawTexturesVertex;
    d.texture_sets[0] = tb.sets[0];
    bound_texture_sets[0] = tb.sets[0];
  }
  if (tb.sets[1] && tb.sets[1] != bound_texture_sets[1]) {
    d.flags |= kDrawTexturesPixel;
    d.texture_sets[1] = tb.sets[1];
    bound_texture_sets[1] = tb.sets[1];
  }

  // Dynamic state.
  if (std::memcmp(&st.vp, &bound_viewport, sizeof(st.vp))) {
    d.flags |= kDrawViewport;
    d.viewport = st.vp;
    bound_viewport = st.vp;
  }
  VkRect2D sr;
  sr.offset.x = int32_t(std::min(st.scissor.offset[0], pass_extent.width));
  sr.offset.y = int32_t(std::min(st.scissor.offset[1], pass_extent.height));
  sr.extent.width = std::min(st.scissor.extent[0], pass_extent.width - uint32_t(sr.offset.x));
  sr.extent.height = std::min(st.scissor.extent[1], pass_extent.height - uint32_t(sr.offset.y));
  if (REXCVAR_GET(nx_native_trace_output) && trace_draw_geometries.size() < 32) {
    const uint32_t surface_pitch = r.Get<reg::RB_SURFACE_INFO>().surface_pitch;
    const std::array<uint32_t, 15> trace_key = {
        st.viewport.xy_offset[0], st.viewport.xy_offset[1], st.viewport.xy_extent[0],
        st.viewport.xy_extent[1], st.scissor.offset[0], st.scissor.offset[1],
        st.scissor.extent[0], st.scissor.extent[1], pass_extent.width, pass_extent.height,
        surface_pitch, uint32_t(sr.offset.x), uint32_t(sr.offset.y), sr.extent.width,
        sr.extent.height};
    if (trace_draw_geometries.insert(trace_key).second) {
      REXLOG_INFO(
          "[native][trace] draw viewport=({},{} {}x{}) scissor=({},{} {}x{}) pass={}x{} "
          "pitch={} host_scissor=({},{} {}x{})",
          st.viewport.xy_offset[0], st.viewport.xy_offset[1], st.viewport.xy_extent[0],
          st.viewport.xy_extent[1], st.scissor.offset[0], st.scissor.offset[1],
          st.scissor.extent[0], st.scissor.extent[1], pass_extent.width, pass_extent.height,
          surface_pitch, sr.offset.x, sr.offset.y, sr.extent.width, sr.extent.height);
    }
  }
  if (std::memcmp(&sr, &bound_scissor, sizeof(sr))) {
    d.flags |= kDrawScissor;
    d.scissor = sr;
    bound_scissor = sr;
  }
  if (std::memcmp(&st.bias_constant, &bound_depth_bias[0], sizeof(float)) ||
      std::memcmp(&st.bias_slope, &bound_depth_bias[1], sizeof(float)) ||
      st.bias_exact_unorm != bound_depth_bias_exact) {
    d.flags |= kDrawDepthBias;
    d.bias_constant = st.bias_constant;
    d.bias_slope = st.bias_slope;
    d.bias_exact_unorm = st.bias_exact_unorm;
    bound_depth_bias_exact = st.bias_exact_unorm;
    bound_depth_bias[0] = st.bias_constant;
    bound_depth_bias[1] = st.bias_slope;
  }
  if (std::memcmp(st.blend_constants, bound_blend_constants, sizeof(st.blend_constants))) {
    d.flags |= kDrawBlend;
    std::memcpy(d.blend, st.blend_constants, sizeof(d.blend));
    std::memcpy(bound_blend_constants, st.blend_constants, sizeof(st.blend_constants));
  }
  // Only inspect stencil draws that reached command recording. Keep at most
  // 64 distinct states; camera movement alone must not produce per-frame logs.
  if (REXCVAR_GET(nx_native_trace_output) && st.targets.depth &&
      st.depth_control.stencil_enable && trace_stencil_states.size() < 64) {
    const RenderTarget& depth_target = *st.targets.depth;
    const uint32_t front_raw = r[rex::graphics::XE_GPU_REG_RB_STENCILREFMASK];
    const uint32_t back_raw = r[rex::graphics::XE_GPU_REG_RB_STENCILREFMASK_BF];
    const uint32_t sc_mode_raw = r[rex::graphics::XE_GPU_REG_PA_SU_SC_MODE_CNTL];
    const uint32_t clip_raw = r[rex::graphics::XE_GPU_REG_PA_CL_CLIP_CNTL];
    const std::array<uint32_t, 12> trace_key = {
        depth_target.key.format, uint32_t(depth_target.format), st.depth_control.value,
        sc_mode_raw, clip_raw, front_raw, back_raw, depth_target.key.base_tiles,
        depth_target.key.pitch, uint32_t(st.primitive_polygonal), depth_target.key.guest_msaa,
        r[rex::graphics::XE_GPU_REG_PA_SC_AA_MASK]};
    if (trace_stencil_states.insert(trace_key).second) {
      REXLOG_INFO(
          "[native][trace] stencil draw frame={} guest_depth={} host_format={} "
          "host_stencil={} depth_control={:08X} polygonal={} sc_mode={:08X} clip={:08X} "
          "front={:08X} back={:08X} host_compare_mask={:02X}/{:02X} "
          "host_write_mask={:02X}/{:02X} host_ref={:02X}/{:02X} "
          "depth_base={} depth_pitch={} host_samples={} guest_samples={} aa_mask={:08X} "
          "viewport=({},{} {}x{}) "
          "scissor=({},{} {}x{})",
          frame, depth_target.key.format, uint32_t(depth_target.format),
          HasStencil(depth_target.format), st.depth_control.value, st.primitive_polygonal,
          sc_mode_raw, clip_raw, front_raw, back_raw, st.stencil[0], st.stencil[1],
          st.stencil[2], st.stencil[3], st.stencil[4], st.stencil[5],
          depth_target.key.base_tiles, depth_target.key.pitch, uint32_t(depth_target.samples),
          1u << depth_target.key.guest_msaa,
          r[rex::graphics::XE_GPU_REG_PA_SC_AA_MASK],
          st.viewport.xy_offset[0], st.viewport.xy_offset[1], st.viewport.xy_extent[0],
          st.viewport.xy_extent[1], sr.offset.x, sr.offset.y, sr.extent.width, sr.extent.height);
      if (trace_stencil_states.size() == 64) {
        REXLOG_INFO("[native][trace] stencil state limit reached; further states omitted");
      }
    }
  }
  std::memcpy(d.stencil, st.stencil, sizeof(d.stencil));
  if (st.stencil[0] != bound_stencil[0] || st.stencil[1] != bound_stencil[1]) {
    d.flags |= kDrawStencilCompare;
  }
  if (st.stencil[2] != bound_stencil[2] || st.stencil[3] != bound_stencil[3]) {
    d.flags |= kDrawStencilWrite;
  }
  if (st.stencil[4] != bound_stencil[4] || st.stencil[5] != bound_stencil[5]) {
    d.flags |= kDrawStencilReference;
  }
  std::memcpy(bound_stencil, st.stencil, sizeof(st.stencil));

  d.count = draw_count;
  if (host_indexed) {
    d.flags |= kDrawIndexed;
    if (index_offset != bound_index_offset || index_type != bound_index_type ||
        !bound_index_valid) {
      d.flags |= kDrawIndexBuffer;
      d.index_buffer = stream_buffer;
      d.index_offset = index_offset;
      d.index_type = index_type;
      bound_index_offset = index_offset;
      bound_index_type = index_type;
      bound_index_valid = true;
    }
  }
  SubmitDraw(d);
  draw_stream_committed = true;
  ProfileDraw();
  ++frame_draw_count;
  ++stats().draws;
  for (RenderTarget* rt : targets.color) {
    if (rt) {
      rt->last_used_frame = frame;
      rt->last_draw_guest_msaa = uint32_t(r.Get<reg::RB_SURFACE_INFO>().msaa_samples);
    }
  }
  MirrorStencilClear();
  MirrorZeroDepthColorClear();
}


// Direct3D's fast stencil clear may reinterpret a 2x-MSAA 800-pixel surface
// as a 4x-MSAA 400-pixel surface at the same EDRAM base and sample pitch.
// The latter's [0,360) clear covers [0,720) in the former, not an unrelated
// image. Preserve that operation while keeping native host images and MSAA off.
// This handles only unconditional, whole-pixel stencil replacement; ordinary
// stencil tests, shadow volumes, depth and color writes are left untouched.
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

void Renderer::Impl::MirrorStencilClear() {
  const DrawSetup& st = setup;
  RenderTarget* source = st.targets.depth;
  const RegisterFile& r = *regs;
  const auto initiator = r.Get<reg::VGT_DRAW_INITIATOR>();
  const auto& depth = st.depth_control;
  if (!source || !HasStencil(source->format) || st.ps || depth.z_enable ||
      st.primitive_polygonal || !device->properties().depthClamp ||
      !depth.stencil_enable || depth.stencilfunc != xenos::CompareFunction::kAlways ||
      depth.stencilzpass != xenos::StencilOp::kReplace ||
      initiator.prim_type != xenos::PrimitiveType::kRectangleList ||
      initiator.source_select != xenos::SourceSelect::kAutoIndex ||
      initiator.num_indices != 3 || !r.Get<reg::PA_CL_CLIP_CNTL>().clip_disable ||
      !st.vs || (st.vs->writes_point_size_edge_flag_kill_vertex() & 0b100) ||
      !rex::graphics::ShaderInterpreter::CanInterpretShader(*st.vs)) {
    return;
  }
  const auto ref = r.Get<reg::RB_STENCILREFMASK>();
  // RectangleList is non-polygonal in this backend: both faces use the front
  // stencil state, culling is disabled and polygon wireframe mode is ignored.
  // Restrict this path to clearing to zero, also the initial value of newly
  // allocated depth targets. Nonzero alias contents need general transfers.
  if (ref.stencilwritemask != 0xFF || ref.stencilref != 0) return;
  const uint32_t guest_msaa = source->key.guest_msaa;
  if (guest_msaa > 2) return;
  // Require the fully enabled mask rather than infer partial sample coverage.
  if ((r[rex::graphics::XE_GPU_REG_PA_SC_AA_MASK] & 0xFFFFu) != 0xFFFFu) return;

  auto layout_of = [](const RenderTarget& rt) {
    return StencilClearLayout{rt.key.base_tiles, rt.key.pitch, rt.key.guest_msaa,
                              rt.key.format, rt.width, rt.height};
  };
  const auto source_layout = layout_of(*source);
  bool has_alias = false;
  for (const auto& entry : render_targets) {
    const RenderTarget& target = *entry.second;
    StencilClearRect mapped;
    if (&target != source && target.key.is_depth && HasStencil(target.format) &&
        MapStencilClearRectangle(source_layout, layout_of(target),
                                 {0, 0, int32_t(source->width), int32_t(source->height)}, mapped)) {
      has_alias = true;
      break;
    }
  }
  if (!has_alias) return;

  StencilClearRect rect{};
  if (!GetFastClearRectangle(rect, false)) return;

  bool started = false;
  for (const auto& entry : render_targets) {
    RenderTarget& target = *entry.second;
    StencilClearRect mapped;
    if (&target == source || !target.key.is_depth || !HasStencil(target.format) ||
        !MapStencilClearRectangle(source_layout, layout_of(target), rect, mapped)) continue;
    if (!started) {
      DrainRecorder();
      EndPass();
      started = true;
    }
    PassTargets clear_targets;
    clear_targets.depth = &target;
    if (!BeginPass(clear_targets)) continue;
    VkClearAttachment attachment{};
    attachment.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    attachment.clearValue.depthStencil.stencil = ref.stencilref;
    VkClearRect clear{};
    clear.rect.offset = {mapped.x0, mapped.y0};
    clear.rect.extent = {uint32_t(mapped.x1 - mapped.x0), uint32_t(mapped.y1 - mapped.y0)};
    clear.layerCount = 1;
    dfn().vkCmdClearAttachments(cmd(), 1, &attachment, 1, &clear);
    EndPass();
    target.last_used_frame = frame;
    if (REXCVAR_GET(nx_native_trace_output) && trace_stencil_clear_aliases.size() < 32) {
      const std::array<uint32_t, 8> key = {source->key.base_tiles, source->key.pitch,
          guest_msaa, target.key.base_tiles, target.key.pitch, target.key.guest_msaa,
          target.key.format, ref.stencilref};
      if (trace_stencil_clear_aliases.insert(key).second) {
        REXLOG_INFO("[native][trace] mirrored stencil clear base={} pitch={} guest_samples={} "
                    "rect=({},{})-({},{}) -> base={} pitch={} guest_samples={} "
                    "rect=({},{})-({},{}) stencil={}",
                    source->key.base_tiles, source->key.pitch, 1u << guest_msaa,
                    rect.x0, rect.y0, rect.x1, rect.y1, target.key.base_tiles,
                    target.key.pitch, 1u << target.key.guest_msaa,
                    mapped.x0, mapped.y0, mapped.x1, mapped.y1, ref.stencilref);
      }
    }
  }
}

// D3D9 also clears RGBA8 EDRAM by binding that same memory as D24S8 and
// drawing depth=0, stencil=0. All 32 guest bits become zero. A separate native
// color image must observe that write before the next color pass / resolve.
// Only mirror whole EDRAM tiles with an established, identical guest layout.
// Partial clears and nonzero depth require a general bit-exact alias transfer.
void Renderer::Impl::MirrorZeroDepthColorClear() {
  const DrawSetup& st = setup;
  RenderTarget* source = st.targets.depth;
  const RegisterFile& r = *regs;
  const auto initiator = r.Get<reg::VGT_DRAW_INITIATOR>();
  const auto& depth = st.depth_control;
  if (!source || source->key.format != uint32_t(xenos::DepthRenderTargetFormat::kD24S8) ||
      !HasStencil(source->format) || st.ps || st.primitive_polygonal ||
      !device->properties().depthClamp || !depth.z_enable || !depth.z_write_enable ||
      depth.zfunc != xenos::CompareFunction::kAlways || !depth.stencil_enable ||
      depth.stencilfunc != xenos::CompareFunction::kAlways ||
      depth.stencilzpass != xenos::StencilOp::kReplace ||
      initiator.prim_type != xenos::PrimitiveType::kRectangleList ||
      initiator.source_select != xenos::SourceSelect::kAutoIndex ||
      initiator.num_indices != 3 || !r.Get<reg::PA_CL_CLIP_CNTL>().clip_disable ||
      !st.vs || (st.vs->writes_point_size_edge_flag_kill_vertex() & 0b100) ||
      !rex::graphics::ShaderInterpreter::CanInterpretShader(*st.vs) ||
      st.bias_constant != 0.0f || st.bias_slope != 0.0f ||
      st.vp.minDepth != 0.0f || st.vp.maxDepth != 1.0f) return;
  for (const RenderTarget* target : st.targets.color) if (target) return;
  const auto ref = r.Get<reg::RB_STENCILREFMASK>();
  if (ref.stencilwritemask != 0xFF || ref.stencilref != 0 ||
      (r[rex::graphics::XE_GPU_REG_PA_SC_AA_MASK] & 0xFFFFu) != 0xFFFFu) return;

  const StencilClearLayout source_layout{source->key.base_tiles, source->key.pitch,
      source->key.guest_msaa, source->key.format, source->width, source->height};
  auto layout_of = [](const RenderTarget& target) {
    return StencilClearLayout{target.key.base_tiles, target.key.pitch,
        target.last_draw_guest_msaa, target.key.format, target.width, target.height};
  };
  bool has_alias = false;
  for (const auto& entry : render_targets) {
    const RenderTarget& target = *entry.second;
    if (!target.key.is_depth && target.samples == source->samples &&
        target.key.base_tiles == source->key.base_tiles &&
        target.key.pitch == source->key.pitch &&
        target.last_draw_guest_msaa == source->key.guest_msaa &&
        target.key.format <= uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)) {
      has_alias = true;
      break;
    }
  }
  if (!has_alias) return;
  StencilClearRect rect{};
  if (!GetFastClearRectangle(rect, true)) return;

  bool started = false;
  for (const auto& entry : render_targets) {
    RenderTarget& target = *entry.second;
    StencilClearRect mapped{};
    if (target.key.is_depth || target.samples != source->samples ||
        !MapZeroDepthColorClearRectangle(source_layout, layout_of(target), rect, mapped)) continue;
    if (!started) {
      DrainRecorder();
      EndPass();
      started = true;
    }
    PassTargets clear_targets;
    clear_targets.color[0] = &target;
    if (!BeginPass(clear_targets)) continue;
    VkClearAttachment attachment{};
    attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    attachment.colorAttachment = 0;
    attachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    VkClearRect clear{};
    clear.rect.offset = {mapped.x0, mapped.y0};
    clear.rect.extent = {uint32_t(mapped.x1 - mapped.x0), uint32_t(mapped.y1 - mapped.y0)};
    clear.layerCount = 1;
    dfn().vkCmdClearAttachments(cmd(), 1, &attachment, 1, &clear);
    EndPass();
    target.last_used_frame = frame;
    if (REXCVAR_GET(nx_native_trace_output) && trace_depth_color_clears.size() < 16) {
      const std::array<uint32_t, 8> key = {source->key.base_tiles, source->key.pitch,
          source->key.guest_msaa, target.key.format, uint32_t(mapped.x0), uint32_t(mapped.y0),
          uint32_t(mapped.x1), uint32_t(mapped.y1)};
      if (trace_depth_color_clears.insert(key).second) {
        REXLOG_INFO("[native][trace] mirrored zero depth/color clear base={} pitch={} "
                    "guest_samples={} rect=({},{})-({},{}) color_format={}",
                    source->key.base_tiles, source->key.pitch, 1u << source->key.guest_msaa,
                    mapped.x0, mapped.y0, mapped.x1, mapped.y1, target.key.format);
      }
    }
  }
}

// --- Resolves ------------------------------------------------------------------------

void Renderer::Impl::Resolve() {
  const RegisterFile& r = *regs;
  auto copy_control = r.Get<reg::RB_COPY_CONTROL>();
  auto surface_info = r.Get<reg::RB_SURFACE_INFO>();

  // The rectangle (draw_util::GetResolveInfo): Direct3D 9 puts its three
  // vertices in vertex fetch constant 0.
  xenos::xe_gpu_vertex_fetch_t fetch = r.GetVertexFetch(0);
  if (fetch.type != xenos::FetchConstantType::kVertex || fetch.size != 3 * 2) {
    return;
  }
  const float* vertices = memory->TranslatePhysical<const float*>(fetch.address << 2);
  float half_pixel =
      r.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero ? 0.5f : 0.0f;
  int32_t v[6];
  for (uint32_t i = 0; i < 6; ++i) {
    float f = xenos::GpuSwap(vertices[i], fetch.endian) + half_pixel;
    v[i] = int32_t(f * 256.0f);
  }
  int32_t x0 = (std::min({v[0], v[2], v[4]}) + 127) >> 8;
  int32_t y0 = (std::min({v[1], v[3], v[5]}) + 127) >> 8;
  int32_t x1 = (std::max({v[0], v[2], v[4]}) + 127) >> 8;
  int32_t y1 = (std::max({v[1], v[3], v[5]}) + 127) >> 8;
  if (r.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
    auto offset = r.Get<reg::PA_SC_WINDOW_OFFSET>();
    x0 += offset.window_x_offset;
    x1 += offset.window_x_offset;
    y0 += offset.window_y_offset;
    y1 += offset.window_y_offset;
  }
  draw_util::Scissor scissor;
  draw_util::GetScissor(r, scissor, false);
  x0 = std::clamp(x0, int32_t(scissor.offset[0]), int32_t(scissor.offset[0] + scissor.extent[0]));
  x1 = std::clamp(x1, int32_t(scissor.offset[0]), int32_t(scissor.offset[0] + scissor.extent[0]));
  y0 = std::clamp(y0, int32_t(scissor.offset[1]), int32_t(scissor.offset[1] + scissor.extent[1]));
  y1 = std::clamp(y1, int32_t(scissor.offset[1]), int32_t(scissor.offset[1] + scissor.extent[1]));
  x0 &= ~7;
  y0 &= ~7;
  x1 = rex::align(x1, 8);
  y1 = rex::align(y1, 8);
  x1 = std::min(x1, int32_t(surface_info.surface_pitch));
  if (x0 >= x1 || y0 >= y1) {
    return;
  }
  uint32_t msaa = HostMsaa(surface_info.msaa_samples);
  bool is_depth = copy_control.copy_src_select >= 4;
  ++stats().resolves;

  BeginRecording();
  EndPass();
  ProfileMark((kProfileResolve << 60) | (uint64_t(is_depth) << 56) |
              (uint64_t(surface_info.surface_pitch & 0x7FF) << 32) |
              (uint64_t(uint32_t(x1 - x0) & 0xFFFF) << 16) | uint32_t(y1 - y0));
  VkCommandBuffer c = cmd();

  RenderTargetKey source_key = {};
  source_key.pitch = surface_info.surface_pitch;
  source_key.msaa = msaa;
  source_key.guest_msaa = is_depth ? uint32_t(surface_info.msaa_samples) : 0;
  if (is_depth) {
    auto depth_info = r.Get<reg::RB_DEPTH_INFO>();
    source_key.base_tiles = depth_info.depth_base;
    source_key.format = uint32_t(depth_info.depth_format);
    source_key.is_depth = 1;
  } else {
    auto color_info =
        r.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[copy_control.copy_src_select]);
    source_key.base_tiles = color_info.color_base;
    source_key.format = uint32_t(color_info.color_format);
  }
  RenderTarget* source = GetRenderTarget(source_key, uint32_t(y1));
  if (!source) return;
  c = cmd();
  uint32_t width = uint32_t(x1 - x0);
  uint32_t height = std::min(uint32_t(y1 - y0), source->height - std::min(uint32_t(y0), source->height));
  if (!height) return;

  // Copy.
  uint32_t dest_base = r[rex::graphics::XE_GPU_REG_RB_COPY_DEST_BASE];
  auto dest_info = r.Get<reg::RB_COPY_DEST_INFO>();
  bool copy = dest_base != 0 &&
              (copy_control.copy_command == xenos::CopyCommand::kRaw ||
               copy_control.copy_command == xenos::CopyCommand::kConvert);
  if (copy) {
    const bool precise_resolve_barriers = REXCVAR_GET(nx_native_precise_resolve_barriers);
    auto barrier_to_transfer = [&]() {
      VkMemoryBarrier to_transfer = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                         VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                         VK_ACCESS_TRANSFER_READ_BIT |
                                         VK_ACCESS_TRANSFER_WRITE_BIT,
                                     VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
      dfn().vkCmdPipelineBarrier(c,
                                 VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                     VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &to_transfer, 0, nullptr,
                                 0, nullptr);
    };
    // Preserve the old ordering when requested. In precise mode the compute
    // routes provide their own dependency; transfer routes issue this barrier
    // immediately before the operation that consumes the render target.
    if (!precise_resolve_barriers) barrier_to_transfer();
    // Legacy exact-address size; shared mode below uses the complete pitch
    // and height instead. Textures sample the image across its whole size, and the game
    // samples its full-screen resolves at 1040 wide, where the pitch made the
    // image 1056 and every one of them needed a corrected copy each frame.
    // Smaller textures sampled from a corner still get one (CopyToSized).
    uint32_t dest_width = uint32_t(x1);
    uint32_t dest_height = uint32_t(y0) + height;
    dest_width = std::min(dest_width, 8192u);
    dest_height = std::min(dest_height, 8192u);
    auto& trace_states = is_depth ? trace_depth_resolve_states : trace_resolve_states;
    const size_t trace_limit = is_depth ? 8 : 24;
    if (REXCVAR_GET(nx_native_trace_output) && trace_states.size() < trace_limit) {
      const uint32_t copy_control_raw = r[rex::graphics::XE_GPU_REG_RB_COPY_CONTROL];
      const uint32_t copy_info_raw = r[rex::graphics::XE_GPU_REG_RB_COPY_DEST_INFO];
      const uint32_t copy_pitch_raw = r[rex::graphics::XE_GPU_REG_RB_COPY_DEST_PITCH];
      const uint32_t surface_raw = r[rex::graphics::XE_GPU_REG_RB_SURFACE_INFO];
      const uint32_t scissor_tl_raw = r[rex::graphics::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL];
      const uint32_t scissor_br_raw = r[rex::graphics::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
      const uint32_t window_offset_raw = r[rex::graphics::XE_GPU_REG_PA_SC_WINDOW_OFFSET];
      const uint32_t vtx_control_raw = r[rex::graphics::XE_GPU_REG_PA_SU_VTX_CNTL];
      const uint32_t source_info_raw =
          is_depth ? r[rex::graphics::XE_GPU_REG_RB_DEPTH_INFO]
                   : r[reg::RB_COLOR_INFO::rt_register_indices[
                         uint32_t(copy_control.copy_src_select)]];
      const std::array<uint32_t, 26> trace_key = {
          dest_base, copy_control_raw, copy_info_raw, copy_pitch_raw, surface_raw,
          source_info_raw, scissor_tl_raw, scissor_br_raw, window_offset_raw,
          vtx_control_raw, fetch.address, fetch.size, uint32_t(fetch.endian),
          uint32_t(x0), uint32_t(y0), width, height, dest_width, dest_height,
          source_key.base_tiles, source_key.pitch, source_key.format, source_key.msaa,
          source_key.is_depth, source->width, source->height};
      if (trace_states.insert(trace_key).second) {
        // Ask the SDK's reference resolve path for its EDRAM span and tiled
        // destination origin. This is diagnostic only; native rendering below
        // keeps its existing behavior until the register state proves a mismatch.
        draw_util::ResolveInfo sdk_info{};
        const bool sdk_info_valid =
            draw_util::GetResolveInfo(r, *memory, 1, 1, false, false, sdk_info);
        uint32_t edram_base = 0, edram_row_length = 0, edram_rows = 0, edram_pitch = 0;
        if (sdk_info_valid) {
          sdk_info.GetCopyEdramTileSpan(edram_base, edram_row_length, edram_rows, edram_pitch);
        }
        const auto copy_pitch = r.Get<reg::RB_COPY_DEST_PITCH>();
        REXLOG_INFO(
            "[native][trace] resolve raw_dest={:08X} address={:08X} copy_control={:08X} "
            "src_select={} command={} sample={} depth={} clear_color={} clear_depth={} "
            "rect=({},{} {}x{}) dest_image={}x{} scissor_raw={:08X}/{:08X} "
            "window_offset={:08X} vtx_control={:08X} surface_raw={:08X} "
            "surface_pitch={} msaa={} src_rt_raw={:08X} src_rt=base{} pitch{} format{} "
            "source_image={}x{} vertex_fetch0_addr={:08X} vertex_fetch0_words={} endian={} "
            "dest_pitch_raw={:08X} dest_pitch={}x{} dest_info_raw={:08X} "
            "dest_format={} number={} endian={} swap={} exp_bias={} array={} slice={} "
            "sdk_valid={} sdk_base={:08X} sdk_extent={:08X}+{:X} sdk_rect={}x{} "
            "sdk_dest_pitch={}x{} sdk_dest_offset=({},{}), sdk_edram=base{} rows={} "
            "row_tiles={} pitch_tiles={} sdk_edram_offset=({}, {})",
            dest_base, dest_base & 0x1FFFFFFF, copy_control_raw,
            uint32_t(copy_control.copy_src_select), uint32_t(copy_control.copy_command),
            uint32_t(copy_control.copy_sample_select), is_depth,
            uint32_t(copy_control.color_clear_enable), uint32_t(copy_control.depth_clear_enable),
            x0, y0, width, height, dest_width, dest_height, scissor_tl_raw, scissor_br_raw,
            window_offset_raw, vtx_control_raw, surface_raw,
            uint32_t(surface_info.surface_pitch),
            uint32_t(surface_info.msaa_samples), source_info_raw, source_key.base_tiles,
            source_key.pitch, source_key.format, source->width, source->height,
            uint32_t(fetch.address), uint32_t(fetch.size), uint32_t(fetch.endian),
            copy_pitch_raw, uint32_t(copy_pitch.copy_dest_pitch),
            uint32_t(copy_pitch.copy_dest_height), copy_info_raw,
            uint32_t(dest_info.copy_dest_format),
            uint32_t(dest_info.copy_dest_number), uint32_t(dest_info.copy_dest_endian),
            uint32_t(dest_info.copy_dest_swap), int32_t(dest_info.copy_dest_exp_bias),
            uint32_t(dest_info.copy_dest_array), uint32_t(dest_info.copy_dest_slice),
            sdk_info_valid, sdk_info.copy_dest_base,
            sdk_info.copy_dest_extent_start, sdk_info.copy_dest_extent_length,
            sdk_info.coordinate_info.width_div_8 * 8, sdk_info.height_div_8 * 8,
            sdk_info.copy_dest_coordinate_info.pitch_aligned_div_32 * 32,
            sdk_info.copy_dest_coordinate_info.height_aligned_div_32 * 32,
            sdk_info.copy_dest_coordinate_info.offset_x_div_8 * 8,
            sdk_info.copy_dest_coordinate_info.offset_y_div_8 * 8, edram_base, edram_rows,
            edram_row_length, edram_pitch,
            sdk_info.coordinate_info.edram_offset_x_div_8 * 8,
            sdk_info.coordinate_info.edram_offset_y_div_8 * 8);
      }
    }
    VkFormat dest_format = is_depth ? VK_FORMAT_R32_SFLOAT : source->format;
    uint32_t host_swizzle = is_depth ? uint32_t(xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR)
                                     : uint32_t(xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA);
    if (!is_depth && dest_info.copy_dest_swap) {
      host_swizzle = XE_GPU_MAKE_TEXTURE_SWIZZLE(B, G, R, A);
    }
    const uint32_t resolve_address = dest_base & 0x1FFFFFFF;
    uint32_t tiled_full_width = 0;
    uint32_t tiled_full_height = 0;
    uint32_t tiled_root_address = resolve_address;
    resolve_layout::Point tiled_origin{};
    bool tiled_format_supported = false;
    bool tiled_layout_valid = false;
    bool tiled_split_segment = false;
    bool supported_tiled_root_write = false;
    const auto tiled_copy_pitch = r.Get<reg::RB_COPY_DEST_PITCH>();
    tiled_full_width = uint32_t(tiled_copy_pitch.copy_dest_pitch);
    tiled_full_height = uint32_t(tiled_copy_pitch.copy_dest_height);
    const auto scissor_tl = r.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
    tiled_origin = {uint32_t(scissor_tl.tl_x), uint32_t(scissor_tl.tl_y)};
    const auto tile_offset = resolve_layout::TileAddressOffset(
        tiled_origin, tiled_full_width, tiled_full_height, 2);
    if (tile_offset && resolve_address >= *tile_offset) {
      tiled_root_address = resolve_address - *tile_offset;
    }
    if (!is_depth) {
      tiled_format_supported =
          uint32_t(dest_info.copy_dest_format) == uint32_t(xenos::ColorFormat::k_8_8_8_8) &&
          source->format == VK_FORMAT_R8G8B8A8_UNORM && !dest_info.copy_dest_array &&
          !dest_info.copy_dest_slice;

      // Keep a composite only for the root layout we know how to maintain.
      // Any other write to an address used by a composite root retires it,
      // including depth, alternate-format, and nonzero-scissor writes.
      if (tiled_origin.x == 0 && tiled_origin.y == 0) {
        const bool valid_root_region =
            tiled_full_width <= 8192 && tiled_full_height <= 8192 &&
            resolve_layout::MakeCopyRegion(tiled_origin, uint32_t(x0), uint32_t(y0), width,
                                           height, source->width, source->height,
                                           tiled_full_width, tiled_full_height)
                .has_value();
        supported_tiled_root_write = tiled_format_supported && valid_root_region;
      }

      if (tiled_format_supported && tile_offset && resolve_address >= *tile_offset) {
        tiled_layout_valid = true;
        const auto window_offset = r.Get<reg::PA_SC_WINDOW_OFFSET>();
        const auto sc_mode = r.Get<reg::PA_SU_SC_MODE_CNTL>();
        tiled_split_segment = (tiled_origin.x || tiled_origin.y) &&
                              !scissor_tl.window_offset_disable &&
                              sc_mode.vtx_window_offset_enable &&
                              window_offset.window_x_offset == -int32_t(tiled_origin.x) &&
                              window_offset.window_y_offset == -int32_t(tiled_origin.y);
      }
    }
    // Invalidate before recording the write. This is a no-op for addresses
    // with no composite, while protecting against stale images when a reused
    // resolve root is written through any layout outside the supported path.
    const bool shared_mode = REXCVAR_GET(nx_native_shared_resolves);
    std::optional<resolve_layout::SharedWrite> shared_write;
    if (shared_mode) {
      const bool supported_format = !dest_info.copy_dest_array && !dest_info.copy_dest_slice &&
          ((is_depth && msaa == 0) || tiled_format_supported);
      const bool window_mapping_valid =
          (tiled_origin.x == 0 && tiled_origin.y == 0) || tiled_split_segment;
      // Depth uses the same guest 32-bit tile addressing. Its Vulkan outputs
      // remain separate float-depth and packed-depth/stencil representations.
      const auto window_offset = r.Get<reg::PA_SC_WINDOW_OFFSET>();
      const auto sc_mode = r.Get<reg::PA_SU_SC_MODE_CNTL>();
      const bool depth_window_mapping = is_depth && !scissor_tl.window_offset_disable &&
          sc_mode.vtx_window_offset_enable &&
          window_offset.window_x_offset == -int32_t(tiled_origin.x) &&
          window_offset.window_y_offset == -int32_t(tiled_origin.y);
      if (supported_format) {
        shared_write = resolve_layout::PlanSharedWrite(
            resolve_address, tiled_full_width, tiled_full_height, tiled_origin,
            window_mapping_valid || depth_window_mapping, uint32_t(x0), uint32_t(y0),
            width, height, source->width, source->height);
      }
    }
    int32_t dest_x = x0, dest_y = y0;
    ResolveDestination* dest = nullptr;
    if (shared_write) {
      const auto& shared = *shared_write;
      dest_width = shared.width;
      dest_height = shared.height;
      dest_x = int32_t(shared.region.destination_x);
      dest_y = int32_t(shared.region.destination_y);
      dest = textures.GetSharedResolveDestination(
          resolve_address, shared.root_address, shared.width, shared.height,
          shared.origin.x, shared.origin.y, dest_format, host_swizzle, c);
      const std::array<uint32_t, 12> shared_trace_key{
          shared.root_address, resolve_address, uint32_t(is_depth), shared.width, shared.height,
          uint32_t(x0), uint32_t(y0), uint32_t(dest_x), uint32_t(dest_y), width, height,
          uint32_t(dest != nullptr)};
      if (REXCVAR_GET(nx_native_trace_output) && trace_shared_resolve_states.size() < 48 &&
          trace_shared_resolve_states.insert(shared_trace_key).second) {
        REXLOG_INFO("[native][shared] direct root={:08X} alias={:08X} depth={} "
                    "extent={}x{} src=({}, {}) dst=({}, {}) size={}x{} allocated={}",
                    shared.root_address, resolve_address, is_depth, shared.width, shared.height,
                    x0, y0, dest_x, dest_y, width, height, dest != nullptr);
      }
    } else {
      if (shared_mode) {
        const std::array<uint32_t, 14> fallback_key{
            resolve_address, uint32_t(is_depth), uint32_t(dest_info.copy_dest_format),
            uint32_t(source->format), msaa, tiled_full_width, tiled_full_height,
            tiled_origin.x, tiled_origin.y, uint32_t(x0), uint32_t(y0), width, height,
            uint32_t(dest_info.copy_dest_array) | (uint32_t(dest_info.copy_dest_slice) << 1)};
        if (REXCVAR_GET(nx_native_trace_output) && trace_shared_fallback_states.size() < 24 &&
            trace_shared_fallback_states.insert(fallback_key).second) {
          REXLOG_INFO("[native][shared] independent fallback address={:08X} depth={} "
                      "format={} host_format={} msaa={} pitch={}x{} origin=({}, {}) "
                      "source=({}, {} {}x{}) array={} slice={} (no composition)",
                      resolve_address, is_depth, uint32_t(dest_info.copy_dest_format),
                      uint32_t(source->format), msaa, tiled_full_width, tiled_full_height,
                      tiled_origin.x, tiled_origin.y, x0, y0, width, height,
                      uint32_t(dest_info.copy_dest_array), uint32_t(dest_info.copy_dest_slice));
        }
        textures.InvalidateSharedResolve(resolve_address);
      }
      textures.BeginResolveWrite(resolve_address, supported_tiled_root_write);
      dest = textures.GetResolveDestination(
          resolve_address, dest_width, dest_height, dest_format, host_swizzle, c);
    }
    if (dest && REXCVAR_GET(nx_native_trace_output)) {
      auto found = trace_resolve_rects.find(resolve_address);
      if (found != trace_resolve_rects.end() || trace_resolve_rects.size() < 256) {
        trace_resolve_rects[resolve_address] = {uint32_t(x0), uint32_t(y0), width, height,
                                                dest_width, dest_height};
      }
    }
    if (dest) {
      if (is_depth) {
        // Depth cannot be copied into a color image directly. It goes through
        // a scratch buffer, where a compute pass makes both forms a game can
        // read a depth resolve back as: floats, into this R32_SFLOAT image for
        // k_24_8 fetches, and the words the resolve leaves in Xbox 360
        // memory, (depth24 << 8) | stencil, into a second image for fetches
        // as any other format (see TextureCache::kDepthWordsTag). Games read
        // the stencil byte that way to tell the player from the scene: with
        // depth alone in every channel, a post effect meant to skip the skater
        // drew him doubled with a light halo.
        const uint64_t texels = uint64_t(width) * height;
        const VkDeviceSize stencil_offset = rex::align(VkDeviceSize(texels * 4), VkDeviceSize(256));
        const VkDeviceSize packed_offset =
            rex::align(stencil_offset + VkDeviceSize(texels), VkDeviceSize(256));
        const bool has_stencil = HasStencil(source->format);
        const bool unorm_guest = xenos::DepthRenderTargetFormat(source->key.format) ==
                                 xenos::DepthRenderTargetFormat::kD24S8;
        if (msaa == 0 && depth_copy_pipeline && source->sample_view &&
            !textures.WantsDepthWords(dest_base & 0x1FFFFFFF)) {
          // The usual case: the game samples the resolve only as depth.
          VkMemoryBarrier to_compute = {
              VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                  VK_ACCESS_SHADER_WRITE_BIT,
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
          dfn().vkCmdPipelineBarrier(
              c,
              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                  VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &to_compute, 0, nullptr, 0, nullptr);
          VkDescriptorSet set = AllocateSet(depth_copy_set_layout);
          if (set) {
            VkDescriptorImageInfo images[2] = {
                {VK_NULL_HANDLE, source->sample_view, VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, dest->view, VK_IMAGE_LAYOUT_GENERAL}};
            VkWriteDescriptorSet writes[2] = {};
            for (uint32_t i = 0; i < 2; ++i) {
              writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
              writes[i].dstSet = set;
              writes[i].dstBinding = i;
              writes[i].descriptorCount = 1;
              writes[i].descriptorType =
                  i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
              writes[i].pImageInfo = &images[i];
            }
            dfn().vkUpdateDescriptorSets(vk, 2, writes, 0, nullptr);
            int32_t params[6] = {x0, y0, dest_x, dest_y, int32_t(width), int32_t(height)};
            dfn().vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, depth_copy_pipeline);
            dfn().vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, depth_copy_layout, 0,
                                          1, &set, 0, nullptr);
            dfn().vkCmdPushConstants(c, depth_copy_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                     sizeof(params), params);
            dfn().vkCmdDispatch(c, (width + 7) / 8, (height + 7) / 8, 1);
          }
          // Readers: the draws, CopyToSized below, and the next depth test.
          VkMemoryBarrier after_compute = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                           VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                                           VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
          dfn().vkCmdPipelineBarrier(
              c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                  VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
              0, 1, &after_compute, 0, nullptr, 0, nullptr);
        } else if (msaa == 0 && depth_scratch && depth_convert_pipeline &&
                   packed_offset + texels * 4 <= kDepthScratchSize) {
          if (precise_resolve_barriers) barrier_to_transfer();
          VkBufferImageCopy regions[2] = {};
          regions[0].imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
          regions[0].imageOffset = {x0, y0, 0};
          regions[0].imageExtent = {width, height, 1};
          regions[1] = regions[0];
          regions[1].bufferOffset = stencil_offset;
          regions[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
          dfn().vkCmdCopyImageToBuffer(c, source->image, VK_IMAGE_LAYOUT_GENERAL, depth_scratch,
                                       has_stencil ? 2 : 1, regions);
          VkMemoryBarrier to_compute = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                        VK_ACCESS_TRANSFER_WRITE_BIT,
                                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
          dfn().vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &to_compute, 0,
                                     nullptr, 0, nullptr);
          uint32_t count = uint32_t(texels);
          uint32_t groups = (count + 63) / 64;
          constexpr uint32_t kGroupsPerRow = 4096;
          uint32_t params[5] = {count, kGroupsPerRow * 64, uint32_t(stencil_offset / 4),
                                uint32_t(packed_offset / 4),
                                (source->format == VK_FORMAT_D24_UNORM_S8_UINT ? 1u : 0u) |
                                    (has_stencil ? 2u : 0u)};
          dfn().vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, depth_convert_pipeline);
          dfn().vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, depth_convert_layout,
                                        0, 1, &depth_convert_set, 0, nullptr);
          dfn().vkCmdPushConstants(c, depth_convert_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   sizeof(params), params);
          dfn().vkCmdDispatch(c, std::min(groups, kGroupsPerRow),
                              (groups + kGroupsPerRow - 1) / kGroupsPerRow, 1);
          VkMemoryBarrier to_copy = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                     VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
          dfn().vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &to_copy, 0, nullptr,
                                     0, nullptr);
          VkBufferImageCopy out = regions[0];
          out.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          out.imageOffset = {dest_x, dest_y, 0};
          dfn().vkCmdCopyBufferToImage(c, depth_scratch, dest->image, VK_IMAGE_LAYOUT_GENERAL, 1,
                                       &out);
          // The words only for unorm depth: float24 would need its own
          // encoding, and the game samples those buffers (shadow maps) as depth.
          if (unorm_guest) {
            ResolveDestination* words = shared_write
                ? textures.GetSharedResolveDestination(
                    resolve_address | TextureCache::kDepthWordsTag,
                    shared_write->root_address | TextureCache::kDepthWordsTag,
                    dest_width, dest_height, shared_write->origin.x, shared_write->origin.y,
                    VK_FORMAT_R8G8B8A8_UNORM, uint32_t(xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA), c)
                : textures.GetResolveDestination(
                    resolve_address | TextureCache::kDepthWordsTag, dest_width, dest_height,
                    VK_FORMAT_R8G8B8A8_UNORM, uint32_t(xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA), c);
            if (words) {
              out.bufferOffset = packed_offset;
              dfn().vkCmdCopyBufferToImage(c, depth_scratch, words->image,
                                           VK_IMAGE_LAYOUT_GENERAL, 1, &out);
              textures.CopyToSized(*words, c,
                  resolve_layout::Rect{uint32_t(dest_x), uint32_t(dest_y), width, height});
            }
          }
        } else {
          static bool logged = false;
          if (!logged) {
            logged = true;
            REXLOG_WARN("[native] Depth resolve not done: {}x{}, {}x MSAA", width, height,
                        1u << msaa);
          }
        }
      } else if (msaa) {
        if (precise_resolve_barriers) barrier_to_transfer();
        VkImageResolve resolve = {};
        resolve.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        resolve.dstSubresource = resolve.srcSubresource;
        resolve.srcOffset = {x0, y0, 0};
        resolve.dstOffset = {dest_x, dest_y, 0};
        resolve.extent = {width, height, 1};
        vkCmdResolveImage(c, source->image, VK_IMAGE_LAYOUT_GENERAL, dest->image,
                                VK_IMAGE_LAYOUT_GENERAL, 1, &resolve);
      } else if (source->samples == VK_SAMPLE_COUNT_1_BIT && textures.StorageCapable(dest->format) &&
                 ComputeColorCopy(c, source->view, dest->view, x0, y0, dest_x, dest_y, width, height)) {
        // Done by compute (see copy_color.cs.glsl).
      } else {
        if (precise_resolve_barriers) barrier_to_transfer();
        VkImageCopy region = {};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = region.srcSubresource;
        region.srcOffset = {x0, y0, 0};
        region.dstOffset = {dest_x, dest_y, 0};
        region.extent = {width, height, 1};
        dfn().vkCmdCopyImage(c, source->image, VK_IMAGE_LAYOUT_GENERAL, dest->image,
                             VK_IMAGE_LAYOUT_GENERAL, 1, &region);
      }
      textures.CopyToSized(*dest, c,
          resolve_layout::Rect{uint32_t(dest_x), uint32_t(dest_y), width, height});
      // The shared-resource mode never creates segment resolve images or
      // invokes the legacy composition stage, including for unsupported writes.
      if (!shared_mode) {
        const TiledResolveRenderSource raw_render_source{
            source->image, source->samples, source->width, source->height};
        if (tiled_origin.x || tiled_origin.y) {
          if (tiled_layout_valid) {
            if (!textures.UpdateTiledResolve(*dest, tiled_root_address, tiled_full_width,
                                             tiled_full_height, 2, tiled_origin.x, tiled_origin.y,
                                             uint32_t(x0), uint32_t(y0), width, height,
                                             tiled_split_segment, c, &raw_render_source)) {
              textures.BeginResolveWrite(tiled_root_address, false);
            }
          } else if (tiled_root_address != resolve_address) {
            // The guest wrote into a candidate tile of a known composite root,
            // but this output format/layout is outside the path we can merge.
            textures.BeginResolveWrite(tiled_root_address, false);
          }
        } else if (tiled_layout_valid) {
          textures.UpdateTiledResolve(*dest, tiled_root_address, tiled_full_width,
                                      tiled_full_height, 2, tiled_origin.x, tiled_origin.y,
                                      uint32_t(x0), uint32_t(y0), width, height,
                                      tiled_split_segment, c);
        }
      }
    }
  }

  // Clear (RenderTargetCache::PerformTransfersAndResolveClears).
  bool clear_depth = copy_control.depth_clear_enable != 0;
  bool clear_color = !is_depth && copy_control.color_clear_enable != 0;
  if (clear_depth || clear_color) {
    // After a copy the clear must wait for it (it reads what the clear
    // overwrites). Without one, the clear only follows earlier rendering,
    // which the pending resolve barrier or pass order already covers.
    if (copy) {
      VkMemoryBarrier to_clear = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                  VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
      dfn().vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                                 0, 1, &to_clear, 0, nullptr, 0, nullptr);
    }
    PassTargets clear_targets;
    RenderTarget* depth_target = nullptr;
    RenderTarget* color_target = nullptr;
    if (clear_depth) {
      auto depth_info = r.Get<reg::RB_DEPTH_INFO>();
      RenderTargetKey key = {};
      key.base_tiles = depth_info.depth_base;
      key.pitch = surface_info.surface_pitch;
      key.format = uint32_t(depth_info.depth_format);
      key.msaa = msaa;
      key.guest_msaa = uint32_t(surface_info.msaa_samples);
      key.is_depth = 1;
      depth_target = GetRenderTarget(key, uint32_t(y1));
      clear_targets.depth = depth_target;
    }
    if (clear_color) {
      color_target = source;
      clear_targets.color[0] = color_target;
    }
    EndPass();
    if (depth_target && !depth_target->zcull_compatible && REXCVAR_GET(nx_native_zcull)) {
      const uint32_t clear_width = color_target
                                       ? std::min(depth_target->width, color_target->width)
                                       : depth_target->width;
      const uint32_t clear_height = color_target
                                        ? std::min(depth_target->height, color_target->height)
                                        : depth_target->height;
      if (CanDiscardDepthForClear(depth_target->width, depth_target->height, x0, y0,
                                  width, height, clear_width, clear_height)) {
        // A target grown via transfer copy can regain persistent ZCULL after
        // its contents are no longer needed. Partial clears retain it intact.
        // Copy the key: replacing the target invalidates its old C++ object.
        const RenderTargetKey depth_key = depth_target->key;
        depth_target = GetRenderTarget(depth_key, depth_target->height, true);
        clear_targets.depth = depth_target;
      }
    }
    if (!clear_targets.Empty()) {
      VkClearAttachment attachments[2] = {};
      uint32_t count = 0;
      if (depth_target) {
        VkClearAttachment& a = attachments[count++];
        a.aspectMask = HasStencil(depth_target->format)
                           ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                           : VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT);
        uint32_t clear = r[rex::graphics::XE_GPU_REG_RB_DEPTH_CLEAR];
        uint32_t depth24 = clear >> 8;
        a.clearValue.depthStencil.depth =
            xenos::DepthRenderTargetFormat(depth_target->key.format) ==
                    xenos::DepthRenderTargetFormat::kD24FS8
                ? xenos::Float20e4To32(depth24) * 0.5f
                : xenos::UNorm24To32(depth24);
        a.clearValue.depthStencil.stencil = clear & 0xFF;
      }
      if (color_target) {
        VkClearAttachment& a = attachments[count++];
        a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        a.colorAttachment = 0;
        uint64_t value = r[rex::graphics::XE_GPU_REG_RB_COLOR_CLEAR] |
                         (uint64_t(r[rex::graphics::XE_GPU_REG_RB_COLOR_CLEAR_LO]) << 32);
        using F = xenos::ColorRenderTargetFormat;
        switch (F(color_target->key.format)) {
          case F::k_8_8_8_8:
          case F::k_8_8_8_8_GAMMA:
            for (uint32_t j = 0; j < 4; ++j) {
              a.clearValue.color.float32[j] = float((value >> (j * 8)) & 0xFF) / 255.0f;
            }
            break;
          case F::k_2_10_10_10:
          case F::k_2_10_10_10_AS_10_10_10_10:
            for (uint32_t j = 0; j < 3; ++j) {
              a.clearValue.color.float32[j] = float((value >> (j * 10)) & 0x3FF) / 1023.0f;
            }
            a.clearValue.color.float32[3] = float((value >> 30) & 0x3) / 3.0f;
            break;
          case F::k_2_10_10_10_FLOAT:
          case F::k_2_10_10_10_FLOAT_AS_16_16_16_16:
            for (uint32_t j = 0; j < 3; ++j) {
              a.clearValue.color.float32[j] = xenos::Float7e3To32((value >> (j * 10)) & 0x3FF);
            }
            a.clearValue.color.float32[3] = float((value >> 30) & 0x3) / 3.0f;
            break;
          case F::k_16_16_FLOAT:
          case F::k_16_16_16_16_FLOAT:
            for (uint32_t j = 0; j < 4; ++j) {
              a.clearValue.color.float32[j] =
                  rex::xenos_half_to_float(uint16_t((value >> (j * 16)) & 0xFFFF));
            }
            break;
          case F::k_16_16:
          case F::k_16_16_16_16:
            for (uint32_t j = 0; j < 4; ++j) {
              a.clearValue.color.float32[j] = std::max(
                  float(int16_t((value >> (j * 16)) & 0xFFFF)) * (1.0f / 32767.0f), -1.0f);
            }
            break;
          case F::k_32_FLOAT: {
            uint32_t bits = uint32_t(value);
            std::memcpy(&a.clearValue.color.float32[0], &bits, 4);
          } break;
          case F::k_32_32_FLOAT: {
            uint32_t bits[2] = {uint32_t(value), uint32_t(value >> 32)};
            std::memcpy(a.clearValue.color.float32, bits, 8);
          } break;
          default:
            break;
        }
      }
      VkClearRect rect = {};
      // Match BeginPass's render area, including clipping between color and
      // depth attachments. A full-area load clear preserves allocation padding
      // outside this area, just like the previous rectangle clear.
      const uint32_t clear_width = depth_target
          ? (color_target ? std::min(depth_target->width, color_target->width)
                          : depth_target->width)
          : color_target->width;
      const uint32_t clear_height = depth_target
          ? (color_target ? std::min(depth_target->height, color_target->height)
                          : depth_target->height)
          : color_target->height;
      rect.rect.offset = {x0, y0};
      rect.rect.extent = {std::min(width, clear_width - std::min(uint32_t(x0), clear_width)),
                          std::min(height, clear_height - std::min(uint32_t(y0), clear_height))};
      rect.layerCount = 1;
      const bool load_clear = REXCVAR_GET(nx_native_full_clear_load_op) &&
          CanUseAttachmentLoadClear(x0, y0, rect.rect.extent.width, rect.rect.extent.height,
                                     clear_width, clear_height);
      const VkClearValue* color_clear = load_clear && color_target
          ? &attachments[depth_target ? 1 : 0].clearValue : nullptr;
      const VkClearValue* depth_clear = load_clear && depth_target
          ? &attachments[0].clearValue : nullptr;
      if (BeginPass(clear_targets, color_clear, depth_clear)) {
        if (!load_clear && rect.rect.extent.width && rect.rect.extent.height) {
          dfn().vkCmdClearAttachments(cmd(), count, attachments, 1, &rect);
        }
        EndPass();
      }
    }
  }

  // The barrier that makes the copies and clears visible is recorded before
  // the next rendering or present, so a run of resolves shares one (each
  // barrier is a full wait for idle on this GPU).
  resolve_barrier_pending = true;
}

void Renderer::Impl::FlushResolveBarrier() {
  if (!resolve_barrier_pending) {
    return;
  }
  resolve_barrier_pending = false;
  VkMemoryBarrier after = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                           VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                               VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                               VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                               VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
  dfn().vkCmdPipelineBarrier(cmd(),
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                 VK_PIPELINE_STAGE_TRANSFER_BIT |
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &after, 0, nullptr, 0, nullptr);
}

// --- Swap ------------------------------------------------------------------------------

void Renderer::Impl::Swap(rex::ui::Presenter* presenter, uint32_t frontbuffer, uint32_t width,
                          uint32_t height) {
  const uint32_t packet_width = width;
  const uint32_t packet_height = height;
  // VdSwap describes the frontbuffer in texture fetch constant 0, swizzle
  // included; the SDK presents from it too (RequestSwapTexture). Without the
  // swizzle, red and blue come out swapped.
  xenos::xe_gpu_texture_fetch_t fetch = regs->GetTextureFetch(0);
  uint32_t address = frontbuffer & 0x1FFFFFFF;
  uint32_t guest_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  uint32_t fetch_width = 0, fetch_height = 0;
  if (fetch.type == xenos::FetchConstantType::kTexture && fetch.base_address) {
    if (fetch.dimension == xenos::DataDimension::k2DOrStacked) {
      fetch_width = fetch.size_2d.width + 1;
      fetch_height = fetch.size_2d.height + 1;
    }
    address = fetch.base_address << 12;
    guest_swizzle = fetch.swizzle;
  }
  uint32_t source_width = 0, source_height = 0;
  ResolvePresentSource present_source;
  VkImageView source_view =
      textures.GetResolveDestinationView2D(address, guest_swizzle, &source_width, &source_height,
                                           &present_source);
  static uint32_t missing_logged = 0;
  if (!source_view && missing_logged < 8) {
    ++missing_logged;
    REXLOG_WARN("[native] Nothing was resolved to the frontbuffer {:08X} ({}x{})", address, width,
                height);
  }
  static bool swap_logged = false;
  if (source_view && !swap_logged) {
    swap_logged = true;
    REXLOG_INFO("[native] Frontbuffer {:08X} {}x{}, swizzle {:03X}", address, width, height,
                guest_swizzle);
  }
  if (presenter && source_view) {
    width = std::min(width ? width : source_width, source_width);
    height = std::min(height ? height : source_height, source_height);
    rex::system::X_VIDEO_MODE mode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    uint32_t display_width = std::max(1u, uint32_t(mode.display_width));
    uint32_t display_height = std::max(1u, uint32_t(mode.display_height));
    if (REXCVAR_GET(nx_native_trace_output) && trace_swap_geometries.size() < 12) {
      TraceResolveRect resolve_rect{};
      auto resolved = trace_resolve_rects.find(address);
      const bool has_resolve_rect = resolved != trace_resolve_rects.end();
      if (has_resolve_rect) resolve_rect = resolved->second;
      const std::array<uint32_t, 20> trace_key = {
          frontbuffer, address, packet_width, packet_height, fetch_width, fetch_height,
          source_width, source_height, width, height, display_width, display_height,
          guest_swizzle, uint32_t(has_resolve_rect), resolve_rect.x, resolve_rect.y,
          resolve_rect.width, resolve_rect.height, resolve_rect.allocation_width,
          resolve_rect.allocation_height};
      if (trace_swap_geometries.insert(trace_key).second) {
        REXLOG_INFO(
            "[native][trace] swap frontbuffer={:08X} address={:08X} packet={}x{} "
            "fetch={}x{} resolve_alloc={}x{} output={}x{} mode={}x{} swizzle={:03X} "
            "resolve_rect={} ({},{} {}x{}, alloc={}x{})",
            frontbuffer, address, packet_width, packet_height, fetch_width, fetch_height,
            source_width, source_height, width, height, display_width, display_height,
            guest_swizzle, has_resolve_rect, resolve_rect.x, resolve_rect.y,
            resolve_rect.width, resolve_rect.height, resolve_rect.allocation_width,
            resolve_rect.allocation_height);
      }
    }
    bool fused_presented = false;
    if (REXCVAR_GET(nx_native_fused_gamma)) {
      auto* vulkan_presenter = static_cast<rex::ui::vulkan::VulkanPresenter*>(presenter);
      fused_presented = vulkan_presenter->RefreshGuestOutputWithGammaTable(
          width, height, display_width, display_height, present_source.format,
          present_source.components,
          [&](rex::ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext& context) {
            BeginRecording();
            EndPass();
            FlushResolveBarrier();
            const uint32_t slot = uint32_t(submission_number % kSubmissionSlots);
            const VkDeviceSize ramp_offset = sizeof(gamma_ramp) * slot;
            std::memcpy(ramp_mapping + ramp_offset, gamma_ramp, sizeof(gamma_ramp));
            VkCommandBuffer c = cmd();
            // Keep a stable, uncorrected mailbox snapshot. Its view carries the
            // same guest/host swizzle as source_view (image copies ignore views).
            VkImageMemoryBarrier image_barrier = {};
            image_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            image_barrier.srcAccessMask = context.image_ever_written_previously()
                                             ? VK_ACCESS_SHADER_READ_BIT : 0;
            image_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            image_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            image_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            image_barrier.image = context.image();
            image_barrier.subresourceRange = rex::ui::vulkan::util::InitializeSubresourceRange();
            VkBufferMemoryBarrier ramp_barrier = {};
            ramp_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            ramp_barrier.srcAccessMask = image_barrier.srcAccessMask;
            ramp_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            ramp_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ramp_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ramp_barrier.buffer = context.gamma_table_buffer();
            ramp_barrier.size = VK_WHOLE_SIZE;
            dfn().vkCmdPipelineBarrier(
                c, context.image_ever_written_previously()
                       ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &ramp_barrier, 1, &image_barrier);
            VkImageCopy copy = {};
            copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.srcSubresource.layerCount = 1;
            copy.dstSubresource = copy.srcSubresource;
            copy.extent = {width, height, 1};
            dfn().vkCmdCopyImage(c, present_source.image, VK_IMAGE_LAYOUT_GENERAL,
                                  context.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            VkBufferCopy ramp_copy = {ramp_offset, 0, sizeof(gamma_ramp)};
            dfn().vkCmdCopyBuffer(c, ramp_buffer, context.gamma_table_buffer(), 1, &ramp_copy);
            image_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            image_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            image_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            image_barrier.newLayout =
                rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout;
            ramp_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            ramp_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            // Complete the source read before a later resolve overwrites it.
            VkMemoryBarrier after_copy = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                            VK_ACCESS_TRANSFER_READ_BIT,
                                            VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT};
            dfn().vkCmdPipelineBarrier(
                c, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 1, &after_copy, 1, &ramp_barrier, 1, &image_barrier);
            Submit();
            context.SetIs8bpc(true);
            return true;
          });
    }
    static int logged_fused_present = -1;
    if (logged_fused_present != int(fused_presented)) {
      logged_fused_present = int(fused_presented);
      REXLOG_INFO("[native] Final gamma: {} (source format {})",
                  fused_presented ? "fused" : "separate", uint32_t(present_source.format));
    }
    if (!fused_presented) presenter->RefreshGuestOutput(
        width, height, display_width, display_height,
        [&](rex::ui::Presenter::GuestOutputRefreshContext& context) -> bool {
          auto& vulkan_context = static_cast<
              rex::ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
          BeginRecording();
          EndPass();
          uint32_t slot = uint32_t(submission_number % kSubmissionSlots);
          std::memcpy(ramp_mapping + 256 * sizeof(uint32_t) * slot, gamma_ramp,
                      sizeof(gamma_ramp));
          VkDescriptorImageInfo source_info = {VK_NULL_HANDLE, source_view,
                                               VK_IMAGE_LAYOUT_GENERAL};
          VkDescriptorImageInfo dest_info = {VK_NULL_HANDLE, vulkan_context.image_view(),
                                             VK_IMAGE_LAYOUT_GENERAL};
          VkWriteDescriptorSet writes[2] = {};
          writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
          writes[0].dstSet = swap_sets[slot][1];
          writes[0].descriptorCount = 1;
          writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
          writes[0].pImageInfo = &source_info;
          writes[1] = writes[0];
          writes[1].dstSet = swap_sets[slot][2];
          writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
          writes[1].pImageInfo = &dest_info;
          dfn().vkUpdateDescriptorSets(vk, 2, writes, 0, nullptr);
          FlushResolveBarrier();
          VkCommandBuffer c = cmd();
          VkImageMemoryBarrier barrier = {};
          barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
          barrier.srcAccessMask =
              vulkan_context.image_ever_written_previously()
                  ? rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask
                  : 0;
          barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
          barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
          barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
          barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          barrier.image = vulkan_context.image();
          barrier.subresourceRange = rex::ui::vulkan::util::InitializeSubresourceRange();
          dfn().vkCmdPipelineBarrier(
              c,
              vulkan_context.image_ever_written_previously()
                  ? rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask
                  : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
          dfn().vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, swap_pipeline);
          dfn().vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, swap_pipeline_layout, 0,
                                        3, swap_sets[slot], 0, nullptr);
          uint32_t size[2] = {width, height};
          dfn().vkCmdPushConstants(c, swap_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   sizeof(size), size);
          dfn().vkCmdDispatch(c, (width + 15) / 16, (height + 7) / 8, 1);
          barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
          barrier.dstAccessMask = rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask;
          barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
          barrier.newLayout = rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout;
          dfn().vkCmdPipelineBarrier(
              c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask, 0, 0, nullptr, 0,
              nullptr, 1, &barrier);
          // Submitted before returning, so the presenter's own work comes
          // after it on the queue.
          Submit();
          context.SetIs8bpc(true);
          return true;
        });
  }
  Submit();
  // Frame boundary.
  auto now = std::chrono::steady_clock::now();
  if (last_swap_time.time_since_epoch().count()) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_swap_time).count();
    if (ms > 50) {
      RendererStats& st = stats();
      ++st.frames_over_50ms;
      if (ms > 100) ++st.frames_over_100ms;
      st.slow_frame_pipelines += st.pipelines_created - at_last_swap.pipelines_created;
      st.slow_frame_shaders += st.shaders_translated - at_last_swap.shaders_translated;
      st.slow_frame_uploads += textures.stats().uploads - at_last_swap.textures_uploaded;
      st.slow_frame_render_targets +=
          st.render_targets_created - at_last_swap.render_targets_created;
    }
  }
  last_swap_time = now;
  at_last_swap = stats();
  at_last_swap.textures_uploaded = textures.stats().uploads;
  // Save the driver's pipeline cache once new pipelines stop appearing for a
  // while, off this thread: the file is tens of megabytes on the SD card.
  if (!driver_cache_path.empty() && stats().pipelines_created != pipelines_at_last_save &&
      now - last_pipeline_time > std::chrono::seconds(5) && !driver_cache_writing.load()) {
    SaveDriverCache();
  }
  stats().textures_uploaded = textures.stats().uploads;
  stats().texture_bytes_uploaded = textures.stats().upload_bytes;
  stats().textures_created = textures.stats().created;
  stats().textures_streamed = textures.stats().streamed;
  stats().textures_pending = textures.stats().pending;
  stats().textures_replaced = textures.stats().replaced;
  stats().frame_upload_wait_ns = frame_upload_wait_ns;
  stats().frame_fence_wait_ns = frame_fence_wait_ns;
  stats().frame_drain_wait_ns = frame_drain_wait_ns;
  stats().frame_texture_ns = frame_texture_ns;
  stats().frame_draws = frame_draw_count;
  stats().frame_stream_wait_ns = frame_stream_wait_ns;
  frame_stream_wait_ns = 0;
  frame_upload_wait_ns = frame_fence_wait_ns = frame_drain_wait_ns = frame_texture_ns = 0;
  frame_draw_count = 0;
  ++frame;
  profile_frame = profile_pool && frame % 16 == 0;
  PollCompleted();
  ProfileReport();
  textures.BeginFrame(frame, completed_frame);
  FreeRetiredSetPools();
  for (auto it = retired_render_targets.begin(); it != retired_render_targets.end();) {
    if (it->frame <= completed_frame) {
      if (it->sample_view) dfn().vkDestroyImageView(vk, it->sample_view, nullptr);
      dfn().vkDestroyImageView(vk, it->view, nullptr);
      dfn().vkDestroyImage(vk, it->image, nullptr);
      dfn().vkFreeMemory(vk, it->memory, nullptr);
      it = retired_render_targets.erase(it);
    } else {
      ++it;
    }
  }
}

// --- Driver pipeline cache ---------------------------------------------------------------

namespace {
constexpr uint64_t kDriverCacheMagic = 0x3148434E54414E52ULL;  // "RNATNCH1"

std::vector<uint8_t> ReadDriverCache(const std::filesystem::path& path) {
  std::vector<uint8_t> bytes;
  if (FILE* file = rex::filesystem::OpenFile(path, "rb")) {
    uint64_t header[3] = {};
    if (fread(header, sizeof(header), 1, file) == 1 && header[0] == kDriverCacheMagic &&
        header[1] >= 32 && header[1] <= 128u * 1024 * 1024) {
      bytes.resize(size_t(header[1]));
      if (fread(bytes.data(), bytes.size(), 1, file) != 1 ||
          XXH3_64bits(bytes.data(), bytes.size()) != header[2]) {
        bytes.clear();
      }
    }
    fclose(file);
  }
  return bytes;
}
}  // namespace

void Renderer::InitializeDriverCache(const std::filesystem::path& cache_root,
                                     uint32_t title_id) {
  Impl& im = *impl_;
  if (!im.device) {
    return;
  }
  std::error_code error;
  auto directory = cache_root / "shaders" / "driver";
  std::filesystem::create_directories(directory, error);
  if (error) {
    return;
  }
  const auto& properties = im.device->properties();
  im.driver_cache_path =
      directory / fmt::format("native-{:08X}-{:08X}-{:08X}-{:08X}.cache", title_id,
                              properties.vendorID, properties.deviceID, properties.driverVersion);
  // The newest complete file: the rename that replaces it can fail on Horizon
  // (it does not overwrite), leaving the data in .tmp.
  std::vector<uint8_t> data = ReadDriverCache(im.driver_cache_path);
  auto temporary = im.driver_cache_path;
  temporary += ".tmp";
  std::vector<uint8_t> newer = ReadDriverCache(temporary);
  if (newer.size() > data.size()) {
    data = std::move(newer);
  }
  VkPipelineCacheCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  info.initialDataSize = data.size();
  info.pInitialData = data.empty() ? nullptr : data.data();
  VkPipelineCache cache = VK_NULL_HANDLE;
  if (im.dfn().vkCreatePipelineCache(im.vk, &info, nullptr, &cache) != VK_SUCCESS) {
    info.initialDataSize = 0;
    info.pInitialData = nullptr;
    if (im.dfn().vkCreatePipelineCache(im.vk, &info, nullptr, &cache) != VK_SUCCESS) {
      return;
    }
    data.clear();
  }
  // Called before the guest runs, so the ring thread is not using the old one.
  if (im.driver_cache) {
    im.dfn().vkDestroyPipelineCache(im.vk, im.driver_cache, nullptr);
  }
  im.driver_cache = cache;
  im.driver_cache_saved_size = data.size();
  REXLOG_INFO("[native] Driver pipeline cache: {} KB loaded from {}", data.size() >> 10,
              im.driver_cache_path.string());
}

void Renderer::Impl::SaveDriverCache() {
  pipelines_at_last_save = stats().pipelines_created;
  size_t size = 0;
  if (dfn().vkGetPipelineCacheData(vk, driver_cache, &size, nullptr) != VK_SUCCESS ||
      size < 32 || size == driver_cache_saved_size) {
    return;
  }
  auto data = std::make_shared<std::vector<uint8_t>>(size);
  if (dfn().vkGetPipelineCacheData(vk, driver_cache, &size, data->data()) != VK_SUCCESS) {
    return;
  }
  data->resize(size);
  driver_cache_saved_size = size;
  driver_cache_writing = true;
  std::filesystem::path path = driver_cache_path;
  // Joined, never detached: detaching throws on Horizon, which ended the game.
  if (driver_cache_writer.joinable()) {
    driver_cache_writer.join();
  }
  driver_cache_writer = std::thread([this, data, path]() {
    auto temporary = path;
    temporary += ".tmp";
    if (FILE* file = rex::filesystem::OpenFile(temporary, "wb")) {
      const uint64_t header[] = {kDriverCacheMagic, uint64_t(data->size()),
                                 XXH3_64bits(data->data(), data->size())};
      bool written = fwrite(header, sizeof(header), 1, file) == 1 &&
                     fwrite(data->data(), data->size(), 1, file) == 1;
      if (fclose(file) != 0) written = false;
      if (written) {
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::rename(temporary, path, error);
      }
    }
    driver_cache_writing = false;
  });
}

// --- Renderer ---------------------------------------------------------------------------

Renderer::Renderer() : impl_(std::make_unique<Impl>()) { impl_->owner = this; }
Renderer::~Renderer() { Shutdown(); }

bool Renderer::Initialize(rex::ui::vulkan::VulkanProvider* provider, rex::memory::Memory* memory,
                          RegisterFile* regs) {
  impl_->provider = provider;
  impl_->memory = memory;
  impl_->regs = regs;
  if (!impl_->Initialize()) {
    impl_->Shutdown();
    return false;
  }
  return true;
}

std::string Renderer::TakeTextureUploadReport() {
  return impl_ ? impl_->textures.TakeUploadReport() : std::string();
}

void Renderer::Flush() {
  if (impl_) impl_->Submit();
}

void Renderer::KickRecorder() {
  if (impl_) impl_->KickRecorder();
}

void Renderer::Shutdown() {
  if (impl_) {
    impl_->Shutdown();
  }
}

void Renderer::LoadShader(xenos::ShaderType type, const uint32_t* ucode, uint32_t dword_count,
                          uint32_t guest_address) {
  // The game switches shaders for most draws, loading them from the same few
  // hundred addresses: remember them by address and size, confirmed by a hash
  // of all of the code. A sample of it (the first, middle and last 16 words)
  // was tried first and was not enough: a level loading its shaders where the
  // previous screen's had been got the old shader whenever the two agreed at
  // the sampled words, and drew its world with the wrong vertex shader.
  // A full hash on every load (twice a draw, 12 MB a frame at 3000 draws) was
  // too slow, so each location is verified in full once a frame, and within
  // the frame by its first words: shaders are replaced between screens, not
  // in the middle of drawing one.
  uint64_t location = 0;
  uint64_t sample = 0;
  uint64_t head = 0;
  if (guest_address) {
    location = (uint64_t(guest_address) << 20) | (uint64_t(dword_count) << 2) | uint64_t(type);
    auto known = impl_->shaders_by_location.find(location);
    head = XXH3_64bits(ucode, std::min(dword_count, 16u) * sizeof(uint32_t));
    if (known != impl_->shaders_by_location.end() &&
        known->second.verified_frame == impl_->frame && known->second.head == head) {
      if (type == xenos::ShaderType::kVertex) {
        impl_->active_vs = known->second.shader;
      } else {
        impl_->active_ps = known->second.shader;
      }
      return;
    }
    sample = XXH3_64bits(ucode, dword_count * sizeof(uint32_t));
    if (known != impl_->shaders_by_location.end() && known->second.sample == sample) {
      known->second.verified_frame = impl_->frame;
      known->second.head = head;
      if (type == xenos::ShaderType::kVertex) {
        impl_->active_vs = known->second.shader;
      } else {
        impl_->active_ps = known->second.shader;
      }
      return;
    }
  }
  uint64_t hash = guest_address ? sample : XXH3_64bits(ucode, dword_count * sizeof(uint32_t));
  auto& shaders = impl_->shaders;
  auto it = shaders.find(hash);
  VulkanShader* shader;
  if (it != shaders.end()) {
    shader = it->second.get();
  } else {
    auto created = std::make_unique<VulkanShader>(impl_->device, type, hash, ucode, dword_count);
    shader = created.get();
    shaders.emplace(hash, std::move(created));
  }
  if (type == xenos::ShaderType::kVertex) {
    impl_->active_vs = shader;
  } else {
    impl_->active_ps = shader;
  }
  if (location) {
    impl_->shaders_by_location[location] = {shader, sample, impl_->frame, head};
  }
}

namespace {
struct CpuTimer {
  uint64_t& total;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  explicit CpuTimer(uint64_t& t) : total(t) {}
  ~CpuTimer() {
    total += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count());
  }
};
}  // namespace

void Renderer::Draw(const uint32_t* immediate_indices, uint32_t immediate_index_words) {
  CpuTimer timer(stats_.cpu_ns);
  impl_->Draw(immediate_indices, immediate_index_words);
}

void Renderer::Resolve() {
  CpuTimer timer(stats_.cpu_ns);
  impl_->Resolve();
}

void Renderer::Swap(rex::ui::Presenter* presenter, uint32_t frontbuffer, uint32_t width,
                    uint32_t height) {
  CpuTimer timer(stats_.cpu_ns);
  impl_->Swap(presenter, frontbuffer, width, height);
}

void Renderer::SetGammaRamp256(const uint32_t* entries) {
  std::memcpy(impl_->gamma_ramp, entries, sizeof(impl_->gamma_ramp));
}

}  // namespace fear_nx::native
