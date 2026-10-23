// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from Project8Recomp (GPL-3.0), src/game/src/native/; generic Xenos GPU code.
//
// Textures for the native renderer (see native_renderer.h).
//
// Guest textures are untiled and converted on the CPU into a staging buffer and
// copied into an image by the setup command buffer, which runs before the draws
// of its submission. An image is identified by its fetch-constant layout and a
// hash of its guest data, so a texture whose memory changes becomes a different
// image, and an image never changes while a submission may be reading it: that
// is what lets every upload be hoisted out of the draw command buffer, which
// then never has to break a render pass to upload.
//
// Resolve destinations are images the renderer copies render targets into.
// They are registered by guest address, and a texture fetched from that address
// uses them instead of the guest memory, which the native renderer never writes.
//
// The formats, the view swizzles and the sampler parameters follow ReXGlue's
// VulkanTextureCache (itself from Xenia, BSD-3-Clause, see LICENSES/), which is
// what the translated shaders are written against.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/vulkan/device.h>

#include "native_resolve_layout.h"

namespace rex::memory {
class Memory;
}

namespace fear_nx::native {

// Where the texture cache puts upload data and records the copies.
struct UploadTarget {
  // Returns a CPU pointer for `size` bytes aligned to `alignment`, and their
  // offset in `buffer`. Null if the arena is exhausted.
  std::function<uint8_t*(size_t size, size_t alignment, VkDeviceSize& offset_out)> allocate;
  VkBuffer buffer = VK_NULL_HANDLE;
  // Records into the command buffer that runs before the submission's draws.
  VkCommandBuffer setup_commands = VK_NULL_HANDLE;
  // Ends rendering and returns the command buffer that follows the current resolves.
  // Shared resolve crops must be copied after the current frame's producer writes.
  std::function<VkCommandBuffer()> resolve_commands;
  // The submission being recorded (increasing).
  uint64_t submission = 0;
};

// An image a resolve writes into. Owned by the texture cache.
struct ResolveDestination {
  uint32_t address = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  // Plain 2D view, for presenting.
  VkImageView view = VK_NULL_HANDLE;
  // Guest components (XYZW, as a texture fetch sees the resolved memory) in
  // terms of the host components (xenos swizzle encoding, 3 bits each).
  uint32_t host_swizzle = 0;
  // Views as textures (2D arrays), by host swizzle; bit 31 set for plain 2D.
  std::unordered_map<uint32_t, VkImageView> texture_views;
  uint64_t last_written_frame = 0;
  // A shared owner can outlive the resolve that created it. Keep it while
  // any draw or presentation in the current frame still references its image.
  uint64_t last_used_frame = 0;
  // Copies of the top-left corner at the sizes the game samples this address
  // as, where that differs from the image. Texture coordinates span the whole
  // image, so sampling the image itself scaled the content: the blur the game
  // makes from a 264x152 resolve into a 320-wide destination came back 1.2
  // times too small, a ghost of the skater up and to the left with a halo.
  // Updated only where a resolve overlaps the crop (CopyToSized).
  struct Sized {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t origin_x = 0;
    uint32_t origin_y = 0;
    bool initialized = false;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::unordered_map<uint32_t, VkImageView> views;
    // Plain 2D view for the compute copy into it (null: copied by transfer).
    VkImageView storage_view = VK_NULL_HANDLE;
    bool filled = false;
    // Last frame a view was sampled. CopyToSized writes are covered by deferred
    // retirement at the recording frame and do not make this crop live for draws.
    uint64_t last_used_frame = 0;
  };
  std::vector<std::unique_ptr<Sized>> sized;
};

// Optional single-sample render target source for bounded tiled-resolve
// diagnostics. The texture cache never owns this image.
struct TiledResolveRenderSource {
  VkImage image = VK_NULL_HANDLE;
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
  uint32_t width = 0;
  uint32_t height = 0;
};

// The image and component mapping underlying a presentation view. Borrowed
// until the next renderer operation; presentation must copy it before returning.
struct ResolvePresentSource {
  VkImage image = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkComponentMapping components = {};
};

struct TextureCacheStats {
  uint64_t uploads = 0;
  uint64_t created = 0;
  uint64_t streamed = 0;
  uint64_t upload_bytes = 0;
  uint64_t hash_bytes = 0;
  uint64_t unsupported = 0;
  uint64_t resolved_hits = 0;
  uint64_t live_textures = 0;
  // Lookups answered with the empty texture: still streaming in, or a format
  // or dimension not handled (logged once each).
  uint64_t pending = 0;
  // Textures whose content changed after a draw of this frame had used them.
  uint64_t replaced = 0;
};

class TextureCache {
 public:
  // Added to a depth resolve's address for the image holding what the resolve
  // leaves in guest memory, (depth24 << 8) | stencil per texel: fetches of
  // that address as anything but a depth format get it. Resolve addresses are
  // page aligned, so the tag never names a real one.
  static constexpr uint32_t kDepthWordsTag = 1;

  TextureCache();
  ~TextureCache();

  bool Initialize(const rex::ui::vulkan::VulkanDevice* device, rex::memory::Memory* memory);
  void Shutdown();

  // Frame boundary: textures not used for a while are destroyed once the GPU
  // is done with them. completed_frame is the last frame whose work finished.
  void BeginFrame(uint64_t frame, uint64_t completed_frame);

  // Records the null images' first transition. Called once, before any use.
  void InitializeNullImages(VkCommandBuffer commands);

  // The view for a translated shader's texture binding. Never null: a texture
  // that cannot be made gives a transparent black one.
  VkImageView GetView(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
                      rex::graphics::xenos::FetchOpDimension dimension, bool is_signed,
                      UploadTarget& upload);

  // About the view the last GetView returned: the texture behind it (null for
  // a resolve or the empty texture), and the last frame through which that
  // answer stays the same while the fetch constant does.
  void* last_texture() const;
  uint64_t last_valid_until() const;
  // Marks a texture as used by this submission and frame, as GetView does:
  // what frees textures and rewrites them in place goes by it.
  void Touch(void* texture, uint64_t submission);

  VkSampler GetSampler(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
                       const rex::graphics::SpirvShader::SamplerBinding& binding);

  // Finds or makes the image a resolve at this address writes into; recreated
  // if the size or the format changes. commands receives the layout
  // transition of a new image.
  ResolveDestination* GetResolveDestination(uint32_t address, uint32_t width, uint32_t height,
                                            VkFormat format, uint32_t host_swizzle,
                                            VkCommandBuffer commands);
  // Registers a validated tiled Xenos resolve address as an alias into one
  // canonical image. The addresses carry the same optional depth-words tag.
  ResolveDestination* GetSharedResolveDestination(uint32_t address, uint32_t root_address,
                                                  uint32_t width, uint32_t height,
                                                  uint32_t origin_x, uint32_t origin_y,
                                                  VkFormat format, uint32_t host_swizzle,
                                                  VkCommandBuffer commands);
  // Invalidates shared surface owners whose address ranges contain this address.
  void InvalidateSharedResolve(uint32_t address);
  const ResolveDestination* FindResolveDestination(uint32_t address) const;
  // After a resolve wrote dest: update its sized copies in destination-image
  // coordinates. No dirty rectangle, or an unfilled crop, requests a full copy.
  void CopyToSized(ResolveDestination& dest, VkCommandBuffer commands,
                   std::optional<resolve_layout::Rect> dirty = std::nullopt);
  // Tracks and combines only validated, 4-byte tiled color resolve segments.
  // `origin` is the segment's guest tile origin in the full destination image;
  // source coordinates are the rectangle stored in the source resolve image.
  bool UpdateTiledResolve(ResolveDestination& source, uint32_t root_address,
                          uint32_t destination_width, uint32_t destination_height,
                          uint32_t bytes_per_pixel_log2, uint32_t origin_x, uint32_t origin_y,
                          uint32_t source_x, uint32_t source_y, uint32_t width, uint32_t height,
                          bool split_segment, VkCommandBuffer commands,
                          const TiledResolveRenderSource* raw_render_source = nullptr);
  // Called before any color write to this exact resolve base. Until a matching
  // split segment arrives, the retained composite must not be presented or sampled.
  void BeginResolveWrite(uint32_t address, bool supported_root);
  // Copies a rectangle between two views on the GPU without vkCmdCopyImage,
  // barriers included (the renderer's compute copy); false if unavailable.
  // Tiled resolve composites prefer this path when storage images are
  // supported, then fall back to a transfer copy.
  using ComputeCopy = std::function<bool(VkCommandBuffer commands, VkImageView source,
                                         VkImageView dest, int32_t source_x, int32_t source_y,
                                         int32_t dest_x, int32_t dest_y, uint32_t width,
                                         uint32_t height)>;
  void SetComputeCopy(ComputeCopy copy);
  // Whether images of this format can be written by the compute copy.
  bool StorageCapable(VkFormat format);

  // Whether a depth resolve to this address has been sampled as anything but
  // a depth format, so it needs the guest memory words (kDepthWordsTag).
  bool WantsDepthWords(uint32_t address) const;
  // The textures uploaded most since the last call (address, format, size,
  // uploads, content fingerprint), for the periodic report; empty if none.
  std::string TakeUploadReport();
  // A plain 2D view of a resolve destination with a host swizzle, for the
  // swap (which samples a non-array image).
  VkImageView GetResolveDestinationView2D(uint32_t address, uint32_t guest_swizzle,
                                          uint32_t* width_out, uint32_t* height_out,
                                          ResolvePresentSource* source_out = nullptr);

  // Untiling runs on a worker: call before submitting the command buffers
  // that copy from the staging data.
  void WaitForUploads();

  const TextureCacheStats& stats() const { return stats_; }
  // Changes whenever image views are destroyed, so descriptor sets cached by
  // view handle must be dropped (a new view may reuse a handle).
  uint64_t views_epoch() const;
  // An id for a view that is never given to another: a handle can be reused
  // once its view is destroyed, an id cannot, so descriptor sets cached by id
  // stay right without being dropped whenever any view is destroyed.
  uint64_t ViewId(VkImageView view);
  // Whether the view with this id still exists.
  bool ViewAlive(VkImageView view, uint64_t id) const;
  // Changes when what GetView answers for some fetch may have changed other
  // than by its own checks (a resolve destination made, a texture replaced).
  uint64_t answers_epoch() const;
  // Retry cached lookup answers after a transient upload failure. Images and
  // their content hashes remain valid; renderer binding caches follow the epoch.
  void InvalidateAnswers();

 private:
  VkImageView GetViewUncached(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
                              rex::graphics::xenos::FetchOpDimension dimension, bool is_signed,
                              UploadTarget& upload, VkImageView null_view);
  struct Texture;
  struct Impl;
  std::unique_ptr<Impl> impl_;
  TextureCacheStats stats_;
};

}  // namespace fear_nx::native
