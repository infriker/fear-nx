// SPDX-License-Identifier: GPL-3.0-or-later
// F.E.A.R. Xenos textures and image descriptor ownership on deko3d.
#pragma once

#include <deko3d.h>
#include "deko_binding_cache.h"
#include "deko_texture_checks.h"
#include "deko_guest_writes.h"
#include "deko_image_ops.h"
#include "deko_image_clear.h"
#include "deko_draw_dependencies.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/deko3d/nucleo.h>

namespace rex::memory {
class Memory;
}

namespace fear_nx::deko {

class ValidationCache;

// An owned deko3d image. Descriptor slots are allocated separately and remain
// valid until their consuming queue work has passed its fence.
struct Image {
  uint64_t identity=0,content_version=1;
  DkImage image{};
  rex::deko3d::BloqueImagen allocation{};
  DkImageFormat format = DkImageFormat_None;
  DkImageType type = DkImageType_2D;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t layers = 1;
  uint32_t mip_levels = 1;
  uint32_t flags = 0;
  ImageClearQueue pending_clear;
  uint32_t host_format_swizzle = rex::graphics::xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  bool valid = false;
  uint64_t last_used_work = 0;
  // Guest swizzle (12 bits) to descriptor slot. The image format fixes the
  // numeric type; callers create a distinct image when signed sampling needs a
  // distinct native format.
  std::unordered_map<uint32_t, uint32_t> views;
  std::vector<uint32_t> descriptor_slots;
};

struct TextureResourceBinding {
  // Only bindings with live image/content and guest-memory dependencies may
  // skip GetTexture; guest textures also keep their scheduled content checks.
  uint32_t image_slot = 0;
  uint32_t sampler_slot = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  bool reusable_in_draw_cache = false;
  uint64_t image_identity=0,content_version=0;
  // Renderer resolve alias routing is checked separately from descriptor life.
  uint32_t guest_address=0;
  uint64_t resolve_version=0;
  GuestMemoryLease guest_memory{};
};

// Cumulative CPU cost of the texture path. get_texture_ns is inclusive;
// the other durations measure nested or distinct phases within it.
struct TextureProfile {
  uint64_t get_texture_ns = 0;
  uint64_t get_texture_calls = 0;
  uint64_t content_hash_ns = 0;
  uint64_t content_hash_bytes = 0;
  uint64_t content_hash_calls = 0;
  uint64_t decode_upload_ns = 0;
  uint64_t decode_upload_calls = 0;
  uint64_t decode_upload_bytes = 0;
  uint64_t image_alloc_ns = 0;
  uint64_t image_alloc_calls = 0;
  uint64_t descriptor_ns = 0;
  uint64_t descriptor_calls = 0;
  uint64_t sampler_ns = 0;
  uint64_t sampler_calls = 0;
  uint64_t binding_hits = 0;
  uint64_t binding_misses = 0;
  uint64_t scheduled_binding_reuses = 0;
};

class TextureCache {
 public:
  TextureCache(rex::deko3d::Nucleo& nucleo, rex::memory::Memory* memory,
               ValidationCache& validation_cache);
  ~TextureCache();
  TextureCache(const TextureCache&) = delete;
  TextureCache& operator=(const TextureCache&) = delete;

  // Create and initialize the black 2D, 3D, and cube descriptors at slots
  // 0, 1, and 2. Must be called with an open Nucleo work slot.
  bool Initialize();
  void Shutdown();

  void SetProfilingEnabled(bool enabled) { profiling_enabled_ = enabled; }
  const TextureProfile& GetProfile() const { return profile_; }
  const TextureChecks::Stats& GetCheckStats() const { return texture_checks_.stats(); }
  uint64_t ImageGeneration() const { return image_generation_; }
  bool BindingCurrent(uint32_t slot,uint64_t identity,uint64_t content) const {
    return descriptor_versions_.Alive(slot,identity,content);
  }
  bool BindingCurrent(const TextureResourceBinding& binding) const;
  void Changed(Image& image) {
    ++image.content_version;
    for(const auto slot:image.descriptor_slots)descriptor_versions_.Bind(slot,image.identity,image.content_version);
  }

  // Call after Nucleo::EmpezarTrabajo(work); slots retired in this work ring
  // position can now be reused because its previous GPU fence has completed.
  void BeginWork(uint64_t work);
  // Call after BeginWork and before recording draws. Budget eviction still
  // runs for each work; periodic aging runs once per renderer frame. Image
  // retirement is deferred through the open Nucleo work slot.
  void BeginFrame(uint64_t frame);
  void Touch(Image& image, uint64_t work);
  // True when the current upload/descriptor slot could not fit this request.
  // The caller should flush the current draw batch and retry that draw once.
  bool NeedsFlushRetry() const { return needs_flush_retry_; }
  void ClearFlushRetry() { needs_flush_retry_ = false; }

  // Image helpers shared by render targets and resolve destinations.
  std::optional<Image> CreateImage(DkImageFormat format, uint32_t width, uint32_t height,
                                   DkImageType type = DkImageType_2D, uint32_t layers = 1,
                                   uint32_t mip_levels = 1, uint32_t flags = 0);
  // Queues image memory and descriptors for retirement after current ordered
  // GPU work completes. The caller must stop recording later references first.
  void FreeImage(Image& image);
  std::optional<uint32_t> GetImageView(
      Image& image, uint32_t guest_swizzle, DkImageType view_type = DkImageType_None,
      DkDsSource ds_source = DkDsSource_Depth, bool uses_load_store = false,
      DkImageFormat format_override = DkImageFormat_None, bool apply_host_swizzle = true);
  // Build a resolve-image view with the guest fetch's numeric type and channel
  // mapping when the image has a byte-compatible Xenos host format.
  std::optional<uint32_t> GetTextureImageView(
      Image& image, const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch, bool is_signed,
      DkImageType view_type = DkImageType_None);

  // Resolve guest fetch constant 0x4800+slot into an image descriptor. Large
  // guest ranges use NFSMW's per-texture frame schedule, 4 KiB samples and
  // periodic full guards. Stable content can be delayed by the check interval
  // and budget; this is not an exact same-frame write guarantee.
  // Unsupported formats or dimensions return nullopt; this path never
  // fabricates scene pixels.
  std::optional<TextureResourceBinding> GetTexture(
      const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
      rex::graphics::xenos::FetchOpDimension dimension, bool is_signed,
      const rex::graphics::SpirvShader::SamplerBinding& sampler,
      const rex::graphics::xenos::xe_gpu_texture_fetch_t* sampler_fetch = nullptr);

  // Cached sampler descriptor for a fetch-constant/sampler declaration pair.
  std::optional<uint32_t> GetSampler(
      const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
      const rex::graphics::SpirvShader::SamplerBinding& binding, bool force_point = false);

 private:
  struct Texture;
  struct MemoBinding {
    Texture* texture=nullptr;
    TextureResourceBinding binding{};
    mutable uint64_t validation_epoch=0, frame=0;
  };
  BindingMemo<MemoBinding,18,1024,4> bindings_;
  // Separate from geometry validation: exact texture candidates survive its
  // boundaries, then recheck their own schedule and guest-memory dependencies.
  uint64_t binding_cache_epoch_=1;
  uint64_t image_generation_ = 1;
  ImageDescriptorVersions descriptor_versions_;
  struct TextureKey {
    std::array<uint32_t, 16> words{};
    bool operator==(const TextureKey& other) const { return words == other.words; }
  };
  struct RetiredWork {
    std::vector<uint32_t> descriptor_slots;
  };
  struct SamplerKeyHash {
    size_t operator()(uint64_t key) const { return size_t(key ^ (key >> 32)); }
  };

  uint32_t AllocateImageDescriptor();
  void RetireDescriptor(uint32_t slot);

  rex::deko3d::Nucleo& nucleo_;
  rex::memory::Memory* memory_;
  ValidationCache& validation_cache_;
  uint64_t work_ = 0;
  uint64_t frame_ = 0;
  uint64_t last_aged_frame_ = UINT64_MAX;
  uint64_t cached_texture_bytes_ = 0;
  bool needs_flush_retry_ = false;
  bool profiling_enabled_ = true;
  TextureProfile profile_{};
  TextureChecks texture_checks_;
  uint32_t next_image_slot_ = 3;
  uint32_t next_sampler_slot_ = 1;
  uint32_t null_image_slots_[3] = {0, 1, 2};
  std::vector<uint32_t> free_image_slots_;
  std::vector<RetiredWork> retired_work_;
  std::vector<std::unique_ptr<Image>> null_images_;
  std::unordered_multimap<uint64_t, std::unique_ptr<Texture>, SamplerKeyHash> textures_;
  std::unordered_map<uint64_t, uint32_t, SamplerKeyHash> samplers_;
};

}  // namespace fear_nx::deko
