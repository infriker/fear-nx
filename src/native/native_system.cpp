// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from Project8Recomp's generic Xenos native renderer (GPL-3.0),
// itself based on nfsmw-nx. Title-specific hooks and shader replacements are excluded.
//
// Native Xenos graphics system for F.E.A.R. NX. It consumes the Xbox 360 GPU
// command ring, updates register and memory state, and presents through the selected backend.
#include "native_system.h"
#if defined(FEAR_NX_RENDERER_DEKO3D)
#include "../deko/deko_renderer.h"
#include "../deko/deko_profile.h"
#include "native_packet_profile.h"
#else
#include "native_renderer.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/system/xvideo.h>
#include <rex/thread.h>
#include <rex/ui/presenter.h>
#if defined(FEAR_NX_RENDERER_DEKO3D)
#include <rex/ui/deko3d/presentador.h>
#else
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#endif
#include <rex/ui/windowed_app_context.h>

REXCVAR_DECLARE(int32_t, switch_cp_core);
REXCVAR_DECLARE(int32_t, query_occlusion_fake_sample_count);

// The game starts its next frame as soon as the swap-complete interrupt of the
// previous one arrives (its frame limiter waits for a counter that interrupt
// bumps). With the native renderer that interrupt came back immediately, so
// frames were paced only by how long they took: 16.7, 33 or 50 ms in turn,
// which feels choppy even at a good average. Holding the interrupt to vblank
// boundaries, at least this many vblanks after the previous one, gives the
// even cadence a console gets from vsync; 2 is a steady 30.
REXCVAR_DEFINE_BOOL(nx_pace_before_present, false, "F.E.A.R. NX",
                    "Hold a frame for the frame rate cap before presenting it rather than "
                    "after, so frames go on screen evenly spaced");
REXCVAR_DEFINE_UINT32(nx_min_frame_vblanks, 1, "F.E.A.R. NX",
                      "Native renderer: minimum 60 Hz vblanks between frames (1 = the game's "
                      "own pacing, 2 = an even 30 fps)");

#if defined(__SWITCH__)
// Adapted from Project8Recomp (GPL-3.0), src/game/src/native/; generic Xenos GPU code.
#define FEAR_NX_RENDERER_DEFAULT "native"
#else
#define FEAR_NX_RENDERER_DEFAULT "xenos"
#endif
REXCVAR_DEFINE_STRING(nx_renderer, FEAR_NX_RENDERER_DEFAULT, "F.E.A.R. NX",
                      "xenos = ReXGlue's emulated Xbox 360 GPU; native = the app-owned "
                      "Vulkan Xenos renderer")
    .allowed({"xenos", "native"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(__SWITCH__)
extern "C" {
#include <switch/kernel/svc.h>
}
extern "C" void RexGmSetReadOnlyMapped(bool enabled);
#if defined(FEAR_NX_RENDERER_DEKO3D)
// The shared input driver exposes this Vulkan draw-census shortcut, but Deko3D
// does not yet collect those records.
extern "C" void RexSwitchCensusRequest(void) {}
#endif
// The frame counter the console FPS overlay and the profiler read (the
// emulated path counts its swaps there too).
extern "C" void RexSwitchPerfCount(unsigned id);
#endif

namespace fear_nx::native {
namespace {

namespace xenos = rex::graphics::xenos;
using rex::X_STATUS;
using Clock = std::chrono::steady_clock;
#if defined(FEAR_NX_RENDERER_DEKO3D)
using BackendProvider = rex::deko3d::ProveedorDeko3d;
using ActiveRenderer = fear_nx::deko::Renderer;
using RendererStats = fear_nx::deko::RendererStats;
using GuestMemoryBoundary = fear_nx::deko::GuestMemoryBoundary;
#else
using BackendProvider = rex::ui::vulkan::VulkanProvider;
using ActiveRenderer = fear_nx::native::Renderer;
#endif

constexpr uint32_t kMmioBase = 0x7FC80000;
constexpr uint32_t kMmioMask = 0xFFFF0000;
constexpr uint32_t kMmioSize = 0x0000FFFF;
constexpr uint32_t kRegisterCount = 0x5003;  // RegisterFile::kRegisterCount

// Registers with a fixed value or an effect, as in the SDK's graphics_system.cpp.
constexpr uint32_t kRegCpRbWptr = 0x01C5;
constexpr uint32_t kRegRbEdramTiming = 0x0F00;
constexpr uint32_t kRegRbBcControl = 0x0F01;
constexpr uint32_t kRegD1ModeVCounter = 0x194C;
constexpr uint32_t kRegD1ModeVblankVlineStatus = 0x1951;
constexpr uint32_t kRegD1ModeViewportSize = 0x1961;

constexpr uint16_t kMaxExtent = 2048 >> 3;        // xenos::kTexture2DCubeMaxWidthHeight >> 3
constexpr int kMaxIndirectDepth = 4;
// Without a GPU some waits can never be satisfied; they are cut short so the
// ring cannot hang.
constexpr auto kWaitRegMemMax = std::chrono::milliseconds(200);
constexpr uint32_t kOutputWidth = 1280;
constexpr uint32_t kOutputHeight = 720;

std::atomic<uint64_t> g_swaps_presented{0};

// Big-endian words of a command buffer. In the ring the position wraps around
// (its size is a power of two); in an indirect buffer it does not.
struct Reader {
  const uint8_t* base = nullptr;
  uint32_t mask = 0;  // words - 1 in the ring; 0 in a linear buffer
  uint32_t pos = 0;
  uint32_t end = 0;

  uint32_t Pending() const { return mask ? ((end - pos) & mask) : (end - pos); }
  uint32_t Peek() const { return rex::memory::load_and_swap<uint32_t>(base + size_t(pos) * 4); }
  uint32_t Read() {
    const uint32_t value = Peek();
    Advance(1);
    return value;
  }
  void Advance(uint32_t words) { pos = mask ? ((pos + words) & mask) : (pos + words); }
};

bool Compare(uint32_t info, uint32_t value, uint32_t reference) {
  switch (info & 0x7) {
    case 0x0:
      return false;
    case 0x1:
      return value < reference;
    case 0x2:
      return value <= reference;
    case 0x3:
      return value == reference;
    case 0x4:
      return value != reference;
    case 0x5:
      return value >= reference;
    case 0x6:
      return value > reference;
    default:
      return true;
  }
}

class NativeGraphicsSystem final : public rex::system::IGraphicsSystem {
 public:
  NativeGraphicsSystem()
      : register_file_(std::make_unique<rex::graphics::RegisterFile>()),
        registers_(register_file_->values) {
#if defined(__SWITCH__)
    // Nothing watches guest pages for writes without the SDK's shared memory,
    // so the pages the game makes read-only can stay mapped. Otherwise every
    // read of them is an emulated fault: 200,000 a second at the menu, which
    // held it at 8 fps. The emulated path gets this from SharedMemory.
    RexGmSetReadOnlyMapped(true);
#endif
    std::memset(registers_, 0, sizeof(register_file_->values));
    // The linear 256-entry gamma ramp the SDK starts with.
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t value = i * 0x3FF / 0xFF;
      gamma_ramp_[i] = value | (value << 10) | (value << 20);
    }
  }
  ~NativeGraphicsSystem() override { Shutdown(); }

  X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override {
    if (presenter_) {
      return X_STATUS_SUCCESS;
    }
    app_context_ = app_context;
    if (!provider_) {
#if defined(FEAR_NX_RENDERER_DEKO3D)
      provider_ = rex::deko3d::ProveedorDeko3d::Crear();
      constexpr const char* backend_name = "Deko3D";
#else
      provider_ = rex::ui::vulkan::VulkanProvider::Create(true, true);
      constexpr const char* backend_name = "Vulkan";
#endif
      if (!provider_) {
        REXLOG_ERROR("[native] Could not create the {} graphics provider", backend_name);
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    auto create = [this]() { presenter_ = provider_->CreatePresenter(); };
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous(create);
    } else {
      create();
    }
    if (!presenter_) {
      REXLOG_ERROR("[native] Could not create the presenter");
      return X_STATUS_UNSUCCESSFUL;
    }
#if defined(FEAR_NX_RENDERER_DEKO3D)
    REXLOG_INFO("[native] Native Xenos graphics system initialized with the Deko3D presenter");
#else
    REXLOG_INFO("[native] Native Xenos graphics system initialized with the Vulkan presenter");
#endif
    return X_STATUS_SUCCESS;
  }

  X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* dispatcher,
                         rex::system::KernelState* kernel_state) override {
    dispatcher_ = dispatcher;
    kernel_state_ = kernel_state;
    memory_ = dispatcher->memory();
    if (!renderer_ready_ && provider_) {
      renderer_ready_ = renderer_.Initialize(provider_.get(), memory_, register_file_.get());
      if (!renderer_ready_) {
        REXLOG_ERROR("[native] The selected Xenos renderer did not start");
      }
    }
    if (!mmio_registered_) {
      mmio_registered_ = memory_->AddVirtualMappedRange(kMmioBase, kMmioMask, kMmioSize, this,
                                                        &ReadMmioThunk, &WriteMmioThunk);
      if (!mmio_registered_) {
        REXLOG_ERROR("[native] Could not register the GPU MMIO range");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    if (running_.exchange(true)) {
      return X_STATUS_SUCCESS;
    }
    vblank_thread_ = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() { return VblankLoop(); }));
    vblank_thread_->set_name("GPU VSync native");
    vblank_thread_->Create();
    ring_thread_ = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() { return RingLoop(); }));
    ring_thread_->set_name("GPU ring native");
    ring_thread_->Create();
    return X_STATUS_SUCCESS;
  }

  bool has_presentation() const override { return presenter_ != nullptr; }
  rex::ui::GraphicsProvider* provider() const override { return provider_.get(); }
  rex::ui::Presenter* presenter() const override { return presenter_.get(); }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    callback_data_.store(user_data, std::memory_order_release);
    callback_.store(callback, std::memory_order_release);
    REXLOG_INFO("[native] Interrupt callback {:08X} ({:08X})", callback, user_data);
  }

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override {
    // As CommandProcessor::InitializeRingBuffer: (1 << (size_log2 + 3)) bytes.
    ring_words_.store(uint32_t(1) << (size_log2 + 1), std::memory_order_release);
    ring_base_.store(ptr, std::memory_order_release);
    ring_generation_.fetch_add(1, std::memory_order_acq_rel);
    REXLOG_INFO("[native] Ring at {:08X}, {} words", ptr, uint32_t(1) << (size_log2 + 1));
  }

  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override {
    (void)block_size_log2;
    read_pointer_writeback_.store(ptr, std::memory_order_release);
  }

  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                               bool blocking) override {
    (void)blocking;
    if (renderer_ready_) {
      renderer_.InitializeDriverCache(cache_root, title_id);
    }
  }

  void Shutdown() override {
    if (running_.exchange(false)) {
      {
        std::lock_guard<std::mutex> lock(ring_mutex_);
      }
      ring_cv_.notify_all();
      if (ring_thread_) {
        ring_thread_->Wait(0, 0, 0, nullptr);
        ring_thread_.reset();
      }
      if (vblank_thread_) {
        vblank_thread_->Wait(0, 0, 0, nullptr);
        vblank_thread_.reset();
      }
    }
    if (renderer_ready_) {
      renderer_.Shutdown();
      renderer_ready_ = false;
    }
#if !defined(FEAR_NX_RENDERER_DEKO3D)
    DestroyVulkan();
#endif
    if (presenter_) {
      if (app_context_) {
        app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
      }
      presenter_.reset();
    }
    provider_.reset();
  }

 private:
#if defined(FEAR_NX_RENDERER_DEKO3D)
  using OutputContext = rex::deko3d::ContextoSalidaDeko3d;
#else
  using OutputContext = rex::ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext;
#endif

  // --- MMIO registers ----------------------------------------------------------

  static uint32_t ReadMmioThunk(void* ppc_context, void* context, uint32_t address) {
    (void)ppc_context;
    return static_cast<NativeGraphicsSystem*>(context)->ReadMmio(address);
  }

  static void WriteMmioThunk(void* ppc_context, void* context, uint32_t address, uint32_t value) {
    (void)ppc_context;
    static_cast<NativeGraphicsSystem*>(context)->WriteMmio(address, value);
  }

  uint32_t ReadMmio(uint32_t address) {
    const uint32_t index = (address & 0xFFFF) / 4;
    switch (index) {
      case kRegRbEdramTiming:
        return 0x08100748;
      case kRegRbBcControl:
        return 0x0000200E;
      case kRegD1ModeVCounter: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        return std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
      }
      case kRegD1ModeVblankVlineStatus:
        return 1;  // in vblank
      case kRegD1ModeViewportSize: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        const uint32_t width = std::min(uint32_t(mode.display_width), uint32_t(0x0FFF));
        const uint32_t height = std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
        return (width << 16) | height;
      }
      default:
        break;
    }
    return Register(index);
  }

  void WriteMmio(uint32_t address, uint32_t value) {
    const uint32_t index = (address & 0xFFFF) / 4;
    if (index == kRegCpRbWptr) {
      write_pointer_.store(value, std::memory_order_release);
      // Taking the lock for an instant keeps the wakeup from being lost when
      // the ring thread is between checking and going to sleep.
      {
        std::lock_guard<std::mutex> lock(ring_mutex_);
      }
      ring_cv_.notify_one();
    }
    // No side effects through MMIO (graphics_system.cpp).
    if (index < kRegisterCount && registers_[index] != value) {
      const uint32_t old_value = registers_[index];
      registers_[index] = value;
      renderer_.RegisterWritten(index, old_value, value);
    }
  }

  uint32_t Register(uint32_t index) const {
    return index < kRegisterCount ? registers_[index] : 0;
  }

  // A register write from the ring, with the side effects it has when the GPU
  // processes it (CommandProcessor::WriteRegister). The Direct3D writes a
  // SCRATCH_REG and then waits with WAIT_REG_MEM until it appears at
  // SCRATCH_ADDR, so those are copied to memory; COHER_STATUS_HOST stays
  // pending until polled.
  // Consecutive registers from the stream. Blocks of shader constants, most of
  // the register traffic (every draw sets its matrices), skip the per-register
  // call and switch: a changed one only bumps a counter in the renderer.
  void WriteRegisters(uint32_t index, uint32_t count, Reader& data) {
    if (index >= 0x4000 && index + count <= 0x4928) {
      bool vs_float = false, ps_float = false, bool_loop = false;
      uint32_t fetch_slots = 0;
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t r = index + i;
        const uint32_t v = data.Read();
        if (registers_[r] == v) continue;
        registers_[r] = v;
#if defined(FEAR_NX_RENDERER_DEKO3D)
        renderer_.FloatConstantWritten(r);
#endif
        if (r < 0x4400) {
          vs_float = true;
        } else if (r < 0x4800) {
          ps_float = true;
        } else if (r < 0x48C0) {
          fetch_slots |= 1u << ((r - 0x4800) / 6);
        } else {
          bool_loop = true;
        }
      }
      renderer_.ConstantsWritten(vs_float, ps_float, fetch_slots, bool_loop);
      return;
    }
    for (uint32_t i = 0; i < count; ++i) {
      WriteRegister(index + i, data.Read());
    }
  }

  void WriteRegister(uint32_t index, uint32_t value) {
    if (index >= kRegisterCount) {
      return;
    }
    if (index == rex::graphics::XE_GPU_REG_COHER_STATUS_HOST) {
      value |= UINT32_C(0x80000000);
    }
    // Caches in the renderer depend on register values, and the game rewrites
    // most state with the same value before every draw: only a change counts.
    if (registers_[index] != value) {
      const uint32_t old_value = registers_[index];
      registers_[index] = value;
      renderer_.RegisterWritten(index, old_value, value);
    }
    switch (index) {
      case rex::graphics::XE_GPU_REG_DC_LUT_RW_INDEX:
        gamma_ramp_component_ = 0;
        return;
      case rex::graphics::XE_GPU_REG_DC_LUT_SEQ_COLOR: {
        // CommandProcessor::WriteRegister: red, green, blue in sequence, the
        // write enable mask in blue, green, red order.
        uint32_t entry = registers_[rex::graphics::XE_GPU_REG_DC_LUT_RW_INDEX] & 0xFF;
        if (registers_[rex::graphics::XE_GPU_REG_DC_LUT_WRITE_EN_MASK] &
            (1u << (2 - gamma_ramp_component_))) {
          uint32_t color = (value & 0xFFFF) >> 6;
          uint32_t shift = 20 - 10 * gamma_ramp_component_;  // red is bits 20:29
          gamma_ramp_[entry] = (gamma_ramp_[entry] & ~(0x3FFu << shift)) | (color << shift);
        }
        if (++gamma_ramp_component_ >= 3) {
          gamma_ramp_component_ = 0;
          registers_[rex::graphics::XE_GPU_REG_DC_LUT_RW_INDEX] =
              (registers_[rex::graphics::XE_GPU_REG_DC_LUT_RW_INDEX] & ~0xFFu) |
              ((entry + 1) & 0xFF);
        }
        renderer_.SetGammaRamp256(gamma_ramp_);
        return;
      }
      case rex::graphics::XE_GPU_REG_DC_LUT_30_COLOR: {
        uint32_t entry = registers_[rex::graphics::XE_GPU_REG_DC_LUT_RW_INDEX] & 0xFF;
        uint32_t enable = registers_[rex::graphics::XE_GPU_REG_DC_LUT_WRITE_EN_MASK] & 0b111;
        uint32_t mask = ((enable & 0b001) ? 0x3FFu : 0) | ((enable & 0b010) ? 0x3FFu << 10 : 0) |
                        ((enable & 0b100) ? 0x3FFu << 20 : 0);
        gamma_ramp_[entry] = (gamma_ramp_[entry] & ~mask) | (value & mask);
        renderer_.SetGammaRamp256(gamma_ramp_);
        return;
      }
      default:
        break;
    }
    if (index >= rex::graphics::XE_GPU_REG_SCRATCH_REG0 &&
        index <= rex::graphics::XE_GPU_REG_SCRATCH_REG7) {
      const uint32_t n = index - rex::graphics::XE_GPU_REG_SCRATCH_REG0;
      if ((UINT32_C(1) << n) & registers_[rex::graphics::XE_GPU_REG_SCRATCH_UMSK]) {
        const uint32_t address = registers_[rex::graphics::XE_GPU_REG_SCRATCH_ADDR] + n * 4;
        rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(address), value);
#if defined(FEAR_NX_RENDERER_DEKO3D)
        if (renderer_ready_) renderer_.InvalidateGuestMemory();
#endif
      }
    }
  }

  void WriteMemory(uint32_t address, uint32_t value) {
    const uint32_t swapped = xenos::GpuSwap(value, static_cast<xenos::Endian>(address & 0x3));
    std::memcpy(memory_->TranslatePhysical(address & ~uint32_t(0x3)), &swapped, sizeof(swapped));
#if defined(FEAR_NX_RENDERER_DEKO3D)
    if (renderer_ready_) renderer_.InvalidateGuestMemory();
#endif
  }

  uint32_t ReadMemory(uint32_t address) {
    uint32_t value;
    std::memcpy(&value, memory_->TranslatePhysical(address & ~uint32_t(0x3)), sizeof(value));
    return xenos::GpuSwap(value, static_cast<xenos::Endian>(address & 0x3));
  }

  // --- Threads -------------------------------------------------------------------

  void Interrupt(uint32_t source, uint32_t cpu) {
    const uint32_t callback = callback_.load(std::memory_order_acquire);
    if (!callback || !dispatcher_) {
      return;
    }
    auto* thread = rex::system::XThread::GetCurrentThread();
    if (!thread) {
      return;
    }
    thread->SetActiveCpu(uint8_t(cpu));
    uint64_t args[] = {source, callback_data_.load(std::memory_order_acquire)};
    dispatcher_->ExecuteInterrupt(thread->thread_state(), callback, args, 2);
  }

  int VblankLoop() {
    rex::system::X_VIDEO_MODE mode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    const double hz = std::max(1.0, double(float(mode.refresh_rate)));
    const auto interval =
        std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / hz));
    // Not raised above the game's threads: the vblank interrupt runs the
    // game's handler, which takes a guest spinlock, and at a higher priority
    // this thread spun on it while the holder could not run on its core. The
    // vblanks stopped and the game never started another frame (black
    // screen).
    auto next = Clock::now() + interval;
    while (running_.load(std::memory_order_acquire)) {
      // To the deadline, not in 1 ms steps: those made every vblank up to a
      // millisecond or more late.
      rex::thread::SleepUntil(next);
      const auto now = Clock::now();
      // After a long pause (loading, a debugger) hundreds of vblanks are not
      // replayed.
      if (now - next > std::chrono::milliseconds(250)) {
        next = now;
      }
      while (now >= next) {
        vblanks_.fetch_add(1, std::memory_order_release);
        counter_.fetch_add(1, std::memory_order_relaxed);
        // Vblank delivery is not a GPU-memory-reuse fence.
        Interrupt(0, 2);
        next += interval;
      }
    }
    return 0;
  }

  int RingLoop() {
#if defined(__SWITCH__)
    // The same core the emulated path's command processor is pinned to: on a
    // core of its own it is not migrated around the game's threads.
    {
      const int32_t core = REXCVAR_GET(switch_cp_core);
      if (core >= 0 && core <= 2) {
        const Result result = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
        REXLOG_INFO("[native] Ring thread on core {}: result {:08X}", core, uint32_t(result));
      }
    }
#endif
    uint32_t read = 0;
    uint32_t generation = 0;
#if defined(FEAR_NX_RENDERER_DEKO3D)
    bool partial_packet = false;
    const auto kick_before_wait = [&] {
      if (!renderer_ready_) return;
      ++ring_idle_kicks_;
      fear_nx::deko::ProfileScope profile(ring_kick_recorder_ns_,
                                         renderer_.CpuProfilingEnabled());
      renderer_.KickRecorder();
    };
#endif
    auto last_report = Clock::now();
    while (running_.load(std::memory_order_acquire)) {
#if defined(FEAR_NX_RENDERER_DEKO3D)
      // A write-pointer snapshot is not a guest fence. If the producer has
      // already published more commands, keep recording this work.
      // Partial packets, ring resets and actual idle still submit as before.
      const bool continue_batch = !partial_packet &&
          running_.load(std::memory_order_acquire) &&
          ring_generation_.load(std::memory_order_acquire) == generation &&
          write_pointer_.load(std::memory_order_acquire) != read;
#endif
      {
#if defined(FEAR_NX_RENDERER_DEKO3D)
        if (!continue_batch) kick_before_wait();
#else
        if (renderer_ready_) renderer_.KickRecorder();
#endif
        const auto idle_start = Clock::now();
        std::unique_lock<std::mutex> lock(ring_mutex_);
        ring_cv_.wait_for(lock, std::chrono::milliseconds(4), [&] {
          return !running_.load(std::memory_order_acquire) ||
                 write_pointer_.load(std::memory_order_acquire) != read ||
                 ring_generation_.load(std::memory_order_acquire) != generation;
        });
        ring_idle_ns_ += uint64_t(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - idle_start)
                .count());
      }
      if (!running_.load(std::memory_order_acquire)) {
        break;
      }
      if (Clock::now() - last_report > std::chrono::seconds(10)) {
        last_report = Clock::now();
        Report();
      }
      const uint32_t gen = ring_generation_.load(std::memory_order_acquire);
      if (gen != generation) {
#if defined(FEAR_NX_RENDERER_DEKO3D)
        // A reset can race the backlog check above. Preserve the old work's
        // submission boundary before consuming the new ring in that case too.
        if (continue_batch) kick_before_wait();
        partial_packet = false;
#endif
        generation = gen;
        read = 0;  // InitializeRingBuffer leaves the read pointer at zero
      }
#if defined(FEAR_NX_RENDERER_DEKO3D)
      else if (continue_batch) ++ring_backlog_coalesced_;
#endif
      const uint32_t base = ring_base_.load(std::memory_order_acquire);
      const uint32_t words = ring_words_.load(std::memory_order_acquire);
      if (!base || !words) {
        continue;
      }
      const uint32_t written = write_pointer_.load(std::memory_order_acquire) & (words - 1);
      if (written == read) {
        continue;
      }
      Reader reader;
      reader.base = memory_->TranslatePhysical(base);
      reader.mask = words - 1;
      reader.pos = read;
      reader.end = written;
#if defined(FEAR_NX_RENDERER_DEKO3D)
      partial_packet = false;
#endif
      while (reader.Pending()) {
#if defined(FEAR_NX_RENDERER_DEKO3D)
        bool packet_complete;
        if (renderer_ready_ && renderer_.CpuProfilingEnabled()) {
          const uint64_t packet_start = fear_nx::deko::ProfileClockNs();
          packet_complete = Packet(reader, 0);
          ring_packet_ns_ += fear_nx::deko::ProfileClockNs() - packet_start;
          ++ring_top_level_packets_;
        } else {
          packet_complete = Packet(reader, 0);
        }
        if (!packet_complete) {
          partial_packet = true;
#else
        if (!Packet(reader, 0)) {
#endif
          // A partial packet: the rest arrives with the next CP_RB_WPTR.
          break;
        }
      }
      read = reader.pos;
      const uint32_t writeback = read_pointer_writeback_.load(std::memory_order_acquire);
      if (writeback) {
        // Ring progress reports consumed commands; it is not a GPU-memory-reuse fence.
        rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(writeback), read);
      }
    }
    return 0;
  }

  void Report() {
    REXLOG_INFO("[native] last 10 s: {} packets, {} draws, {} copies, {} swaps, {} waits cut short",
                packets_ - reported_packets_, draws_ - reported_draws_, copies_ - reported_copies_,
                swaps_ - reported_swaps_, waits_cut_short_);
    reported_packets_ = packets_;
    reported_draws_ = draws_;
    reported_copies_ = copies_;
    reported_swaps_before_ = reported_swaps_;
    reported_swaps_ = swaps_;
    if (renderer_ready_) {
      const RendererStats& s = renderer_.stats();
#if defined(FEAR_NX_RENDERER_DEKO3D)
      const uint64_t frames = std::max<uint64_t>(swaps_ - reported_swaps_before_, 1);
      // Ring-thread counters only: no per-draw logging, timing, GPU queries
      // or waits. Independent of CPU adapter / renderer profiling toggles.
      REXLOG_INFO(
          "[deko-visibility] patch=058 mode=visible-fallback; last 10 s: VIZ begin {}, "
          "end {}, ZPD {}, EXT {}; draw requests: conditional {}, query-enabled {}, "
          "kill-hi-z {}, kill-detail {}; discarded probes {}",
          visibility_.viz_begin - reported_visibility_.viz_begin,
          visibility_.viz_end - reported_visibility_.viz_end,
          visibility_.zpd_reports - reported_visibility_.zpd_reports,
          visibility_.extent_reports - reported_visibility_.extent_reports,
          visibility_.conditional_draws - reported_visibility_.conditional_draws,
          visibility_.query_enabled_draws - reported_visibility_.query_enabled_draws,
          visibility_.kill_hi_z_draws - reported_visibility_.kill_hi_z_draws,
          visibility_.kill_detail_draws - reported_visibility_.kill_detail_draws,
          s.visibility_probes_discarded - reported_render_.visibility_probes_discarded);
      reported_visibility_ = visibility_;
#endif
#if defined(FEAR_NX_RENDERER_DEKO3D)
      REXLOG_INFO(
          "[deko-cache] last 10 s: prepared hits {}, DKSH hits {}, UAM compiles {}; "
          "Xenos {:.1f} ms, SPIRV-Cross {:.1f} ms, UAM {:.1f} ms; invalid {}, write failures {}",
          s.shader_prepared_hits-reported_render_.shader_prepared_hits,
          s.shader_dksh_hits-reported_render_.shader_dksh_hits,
          s.shader_compiles-reported_render_.shader_compiles,
          double(s.shader_translate_ns-reported_render_.shader_translate_ns)/1e6,
          double(s.shader_convert_ns-reported_render_.shader_convert_ns)/1e6,
          double(s.shader_compile_ns-reported_render_.shader_compile_ns)/1e6,
          s.shader_cache_invalid-reported_render_.shader_cache_invalid,
          s.shader_cache_write_failures-reported_render_.shader_cache_write_failures);
#endif
#if defined(FEAR_NX_RENDERER_DEKO3D)
      REXLOG_INFO(
          "[native] renderer: {} drawn, {} skipped, {} resolves, {} passes, {} submissions, "
          "{} shaders total, {} pipelines total, {} MB vertices",
          s.draws - reported_render_.draws, s.draws_skipped - reported_render_.draws_skipped,
          s.resolves - reported_render_.resolves, s.render_passes - reported_render_.render_passes,
          s.submissions - reported_render_.submissions, s.shaders_translated, s.pipelines_created,
          (s.vertex_bytes_copied - reported_render_.vertex_bytes_copied) >> 20);
#else
      REXLOG_INFO(
          "[native] renderer: {} drawn, {} skipped, {} resolves, {} passes, {} submissions, "
          "{} shaders, {} pipelines, {} textures ({} MB), {} MB vertices",
          s.draws - reported_render_.draws, s.draws_skipped - reported_render_.draws_skipped,
          s.resolves - reported_render_.resolves, s.render_passes - reported_render_.render_passes,
          s.submissions - reported_render_.submissions, s.shaders_translated,
          s.pipelines_created, s.textures_uploaded - reported_render_.textures_uploaded,
          (s.texture_bytes_uploaded - reported_render_.texture_bytes_uploaded) >> 20,
          (s.vertex_bytes_copied - reported_render_.vertex_bytes_copied) >> 20);
#endif
#if !defined(FEAR_NX_RENDERER_DEKO3D)
      REXLOG_INFO(
          "[native] textures: {} made now, {} streamed in, {} replaced after use, {} lookups "
          "answered empty (streaming or not handled)",
          s.textures_created - reported_render_.textures_created,
          s.textures_streamed - reported_render_.textures_streamed,
          s.textures_replaced - reported_render_.textures_replaced,
          s.textures_pending - reported_render_.textures_pending);
      {
        std::string uploads = renderer_.TakeTextureUploadReport();
        if (!uploads.empty()) {
          REXLOG_INFO("[native] most uploaded textures: {}", uploads);
        }
      }
#endif
#if defined(FEAR_NX_RENDERER_DEKO3D)
      renderer_.ReportProfile(frames);
      REXLOG_INFO(
          "[deko-ring] patch=059: {} backlog boundaries coalesced, {} idle/partial/reset "
          "Kick calls; WAIT/swap/capacity submission rules unchanged",
          ring_backlog_coalesced_ - reported_ring_backlog_coalesced_,
          ring_idle_kicks_ - reported_ring_idle_kicks_);
      reported_ring_backlog_coalesced_ = ring_backlog_coalesced_;
      reported_ring_idle_kicks_ = ring_idle_kicks_;
      if (renderer_.CpuProfilingEnabled()) {
        const uint64_t packet_ns = ring_packet_ns_ - reported_ring_packet_ns_;
        const uint64_t renderer_ns = s.cpu_ns - reported_render_.cpu_ns;
        const uint64_t packet_external_ns =
            ring_packet_external_ns_ - reported_ring_packet_external_ns_;
        const uint64_t top_level_packets =
            ring_top_level_packets_ - reported_ring_top_level_packets_;
        REXLOG_INFO(
            "[deko-profile] ring-thread top-level Packet calls: {:.1f} ms total, {:.2f} ms/frame "
            "({} calls); wall time includes waits/preemption and nested packet work; UI "
            "paint/present is on another thread",
            double(packet_ns) / 1e6, double(packet_ns) / 1e6 / double(frames), top_level_packets);
        const uint64_t wait_ns = wait_ns_ - reported_wait_ns_;
        const uint64_t wait_kick_ns = ring_wait_kick_ns_ - reported_ring_wait_kick_ns_;
        const uint64_t pacing_ns = hold_ns_ - reported_ring_hold_ns_;
        const auto dispatch = BreakDownPacketDispatch(
            packet_ns, renderer_ns, packet_external_ns, wait_ns, wait_kick_ns, pacing_ns);
        if (dispatch.remainder_valid) {
          REXLOG_INFO(
              "[deko-profile] packet dispatch outside renderer phases and external Flush/Kick "
              "calls: {:.1f} ms total, {:.2f} ms/frame; includes WAIT_REG_MEM and frame pacing",
              double(dispatch.outside_renderer_ns) / 1e6,
              double(dispatch.outside_renderer_ns) / 1e6 / double(frames));
        } else {
          REXLOG_INFO(
              "[deko-profile] packet dispatch remainder unavailable: measured renderer phases "
              "and in-packet Flush/Kick calls exceed top-level packet wall time "
              "({:.1f} ms renderer, {:.1f} ms external, {:.1f} ms packet)",
              double(renderer_ns) / 1e6, double(packet_external_ns) / 1e6,
              double(packet_ns) / 1e6);
        }
        if (dispatch.wait_breakdown_valid) {
          REXLOG_INFO(
              "[deko-profile] packet dispatch v2 ms/frame: WAIT_REG_MEM excluding Kick {:.2f}, "
              "pacing {:.2f}, other dispatch {:.2f}; in-wait Kick {:.2f} is already in external "
              "calls; wall time includes preemption, not CPU utilization",
              double(dispatch.wait_excluding_kick_ns) / 1e6 / double(frames),
              double(pacing_ns) / 1e6 / double(frames),
              double(dispatch.other_dispatch_ns) / 1e6 / double(frames),
              double(wait_kick_ns) / 1e6 / double(frames));
        } else {
          REXLOG_INFO(
              "[deko-profile] packet dispatch v2 unavailable: interval phase totals do not "
              "nest ({:.1f} ms wait, {:.1f} ms in-wait Kick, {:.1f} ms pacing)",
              double(wait_ns) / 1e6, double(wait_kick_ns) / 1e6, double(pacing_ns) / 1e6);
        }
        REXLOG_INFO(
            "[deko-profile] external ring-thread calls: KickRecorder {:.2f} ms/frame, "
            "Flush {:.2f} ms/frame (not included in renderer CPU total)",
            double(ring_kick_recorder_ns_ - reported_ring_kick_recorder_ns_) / 1e6 /
                double(frames),
            double(ring_flush_ns_ - reported_ring_flush_ns_) / 1e6 / double(frames));
      }
      reported_ring_packet_ns_ = ring_packet_ns_;
      reported_ring_packet_external_ns_ = ring_packet_external_ns_;
      reported_ring_top_level_packets_ = ring_top_level_packets_;
      reported_ring_kick_recorder_ns_ = ring_kick_recorder_ns_;
      reported_ring_flush_ns_ = ring_flush_ns_;
      reported_ring_wait_kick_ns_ = ring_wait_kick_ns_;
      reported_ring_hold_ns_ = hold_ns_;
#else
      uint64_t frames = std::max<uint64_t>(swaps_ - reported_swaps_before_, 1);
      uint64_t gpu_count =
          std::max<uint64_t>(s.gpu_submissions - reported_render_.gpu_submissions, 1);
      REXLOG_INFO(
          "[native] per frame: renderer CPU {:.1f} ms, GPU {:.1f} ms over {:.1f} submissions "
          "(GPU time as the driver reports it)",
          double(s.cpu_ns - reported_render_.cpu_ns) / 1e6 / double(frames),
          double(s.gpu_ns - reported_render_.gpu_ns) / 1e6 / double(frames),
          double(s.gpu_submissions - reported_render_.gpu_submissions) / double(frames));
      (void)gpu_count;
#endif
      REXLOG_INFO("[native] per frame: {:.0f} draws, {:.0f} state rebuilds, {:.0f} texture "
                  "binding rebuilds",
                  double(s.draws - reported_render_.draws) / double(frames),
                  double(s.setups_built - reported_render_.setups_built) / double(frames),
                  double(s.texture_bindings_built - reported_render_.texture_bindings_built) /
                      double(frames));
      REXLOG_INFO("[native] per frame: {:.1f} waits for the game, {:.1f} ms waiting",
                  double(waits_ - reported_waits_) / double(frames),
                  double(wait_ns_ - reported_wait_ns_) / 1e6 / double(frames));
      reported_waits_ = waits_;
    slow_frames_logged_ = 0;
      REXLOG_INFO(
#if defined(FEAR_NX_RENDERER_DEKO3D)
          "[native] slow frames: {} over 50 ms, {} over 100 ms; in them {} pipelines, "
          "{} shaders, {} render targets",
#else
          "[native] slow frames: {} over 50 ms, {} over 100 ms; in them {} pipelines, "
          "{} shaders, {} texture uploads, {} render targets",
#endif
          s.frames_over_50ms - reported_render_.frames_over_50ms,
          s.frames_over_100ms - reported_render_.frames_over_100ms,
          s.slow_frame_pipelines - reported_render_.slow_frame_pipelines,
          s.slow_frame_shaders - reported_render_.slow_frame_shaders,
#if !defined(FEAR_NX_RENDERER_DEKO3D)
          s.slow_frame_uploads - reported_render_.slow_frame_uploads,
#endif
          s.slow_frame_render_targets - reported_render_.slow_frame_render_targets);
      reported_wait_ns_ = wait_ns_;
      {
        // The registers that most often changed the render state.
        std::vector<std::pair<uint32_t, uint32_t>> top;
        for (uint32_t i = 0; i < 0x4000; ++i) {
          if (renderer_.state_change_counts_[i]) {
            top.emplace_back(renderer_.state_change_counts_[i], i);
            renderer_.state_change_counts_[i] = 0;
          }
        }
        std::sort(top.rbegin(), top.rend());
        std::string list;
        for (size_t i = 0; i < std::min<size_t>(top.size(), 12); ++i) {
          list += fmt::format(" {:04X}:{}", top[i].second, top[i].first);
        }
        REXLOG_INFO("[native] state changes by register (10 s):{}", list);
      }
      std::string reasons;
      for (uint32_t i = 0; i < 16; ++i) {
        uint64_t n = s.skipped_by_reason[i] - reported_render_.skipped_by_reason[i];
        if (n) reasons += fmt::format(" {}:{}", i, n);
      }
      REXLOG_INFO("[native] skipped by reason:{}", reasons);
      reported_render_ = s;
    }
  }
  RendererStats reported_render_;
  uint64_t reported_swaps_before_ = 0;

  // --- PM4 packets -----------------------------------------------------------------

  // Consumes a whole packet, or leaves the position untouched if it is not
  // complete yet.
  bool Packet(Reader& reader, int depth) {
    const uint32_t packet = reader.Peek();
    uint32_t count = 0;
    switch (packet >> 30) {
      case 0:
        count = packet ? ((packet >> 16) & 0x3FFF) + 1 : 0;
        break;
      case 1:
        count = 2;
        break;
      case 2:
        count = 0;
        break;
      default:
        count = ((packet >> 16) & 0x3FFF) + 1;
        break;
    }
    if (reader.Pending() < count + 1) {
      return false;
    }
    reader.Advance(1);
    ++packets_;
    Reader data = reader;
    reader.Advance(count);
    if (!packet) {
      return true;
    }
    switch (packet >> 30) {
      case 0: {
        const uint32_t index = packet & 0x7FFF;
        const bool one_register = (packet >> 15) & 0x1;
        if (one_register) {
          for (uint32_t i = 0; i < count; ++i) {
            WriteRegister(index, data.Read());
          }
        } else {
          WriteRegisters(index, count, data);
        }
        break;
      }
      case 1: {
        const uint32_t value1 = data.Read();
        const uint32_t value2 = data.Read();
        WriteRegister(packet & 0x7FF, value1);
        WriteRegister((packet >> 11) & 0x7FF, value2);
        break;
      }
      case 2:
        break;
      default:
        Type3Packet(data, packet, count, depth);
        break;
    }
    return true;
  }

  void Type3Packet(Reader& data, uint32_t packet, uint32_t words, int depth) {
    const uint32_t opcode = (packet >> 8) & 0x7F;
    if ((packet & 0x1) && (!(bin_select_ & bin_mask_) || opcode == xenos::PM4_XE_SWAP)) {
      return;  // predicate fails; predicated swaps are always dropped
    }
    switch (opcode) {
      case xenos::PM4_INTERRUPT: {
        if (words < 1) break;
        const uint32_t cpus = data.Read();
#if defined(FEAR_NX_RENDERER_DEKO3D)
        if (renderer_ready_) renderer_.InvalidateGuestMemory(GuestMemoryBoundary::kInterrupt);
#endif
        if (swap_interrupt_pending_) {
          // The swap-complete interrupt: right after the present, which the
          // frame rate cap already held (PM4_XE_SWAP), or held here if the
          // cap is not applied before presenting.
          swap_interrupt_pending_ = false;
          if (!REXCVAR_GET(nx_pace_before_present)) PaceFrame();
        }
        for (uint32_t cpu = 0; cpu < 6; ++cpu) {
          if (cpus & (1u << cpu)) {
            Interrupt(1, cpu);
          }
        }
        break;
      }
      case xenos::PM4_XE_SWAP:
        // VdSwap writes "SWAP", the physical address of the frontbuffer and
        // its size.
        if (words >= 4) {
          data.Read();
          swap_frontbuffer_ = data.Read();
          swap_width_ = data.Read();
          swap_height_ = data.Read();
        }
        if (REXCVAR_GET(nx_pace_before_present)) PaceFrame();
        Present();
        swap_interrupt_pending_ = true;
        ReportSlowFrame();
        break;
      case xenos::PM4_INDIRECT_BUFFER:
      case xenos::PM4_INDIRECT_BUFFER_PFD: {
        if (words < 2) break;
        const uint32_t address = data.Read() & 0x1FFFFFFF;
        const uint32_t length = data.Read() & 0xFFFFF;
        if (depth < kMaxIndirectDepth) {
          IndirectBuffer(address, length, depth + 1);
        }
        break;
      }
      case xenos::PM4_WAIT_REG_MEM: {
        if (words < 5) break;
        const uint32_t info = data.Read();
        const uint32_t poll = data.Read();
        const uint32_t reference = data.Read();
        const uint32_t mask = data.Read();
        // Most coherency polls are already satisfied. Start the timeout only
        // after a failed comparison, avoiding a clock read on the ready path.
        Clock::time_point wait_start{}, limit{};
        bool timer_started = false;
        bool waited = false;
        for (uint32_t attempt = 0;; ++attempt) {
          if (!(info & 0x10) && poll == rex::graphics::XE_GPU_REG_COHER_STATUS_HOST &&
              (Register(poll) & UINT32_C(0x80000000))) {
            registers_[poll] = 0;  // MakeCoherent: no shared memory to synchronise
          }
          const uint32_t value = (info & 0x10) ? ReadMemory(poll) : Register(poll);
          if (Compare(info, value & mask, reference)) {
            break;
          }
          const auto now = Clock::now();
          if (!timer_started) {
            wait_start = now;
            limit = now + kWaitRegMemMax;
            timer_started = true;
          }
          if (now >= limit || !running_.load(std::memory_order_acquire)) {
            if (waits_cut_short_++ == 0) {
              REXLOG_WARN("[native] WAIT_REG_MEM not met ({} {:08X} ref {:08X} mask {:08X})",
                          (info & 0x10) ? "memory" : "register", poll, reference, mask);
            }
            break;
          }
          if (!waited && renderer_ready_) {
            // Idle time: let the draw recording thread empty its queue.
#if defined(FEAR_NX_RENDERER_DEKO3D)
            const bool profile_enabled = renderer_.CpuProfilingEnabled();
            const uint64_t external_before = ring_packet_external_ns_;
            {
              fear_nx::deko::ProfileScope profile(ring_kick_recorder_ns_, profile_enabled);
              fear_nx::deko::ProfileScope packet_profile(ring_packet_external_ns_,
                                                        profile_enabled);
              renderer_.KickRecorder();
            }
            // Reuse the existing measurement after both scopes close. WAIT
            // already contains this time; do not subtract it twice in Report.
            ring_wait_kick_ns_ += ring_packet_external_ns_ - external_before;
#else
            renderer_.KickRecorder();
#endif
          }
          waited = true;
          // The game usually releases what the ring waits for within tens of
          // microseconds; yielding first avoids oversleeping each wait.
          if (attempt < 64) {
            std::this_thread::yield();
          } else {
            rex::thread::Sleep(std::chrono::microseconds(50));
          }
        }
        if (waited) {
          ++waits_;
          wait_ns_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   Clock::now() - wait_start)
                                   .count());
        }
#if defined(FEAR_NX_RENDERER_DEKO3D)
        if (renderer_ready_) {
          // An immediately satisfied coherency-register poll is GPU state,
          // not evidence that every guest resource changed. It still ends
          // address-only vertex reuse: dynamic contents get an exact hash
          // before using the immutable content-keyed cache again.
          // Actual memory polls, other registers and blocking waits retain
          // full revalidation, including an already-satisfied CPU handshake.
          if (!waited && !(info & 0x10) &&
              poll == rex::graphics::XE_GPU_REG_COHER_STATUS_HOST) {
            renderer_.InvalidateVertexReuse();
          } else {
            renderer_.InvalidateGuestMemory((info & 0x10)
                ? GuestMemoryBoundary::kMemoryWait : GuestMemoryBoundary::kRegisterWait);
          }
        }
#endif
        break;
      }
      case xenos::PM4_REG_RMW: {
        if (words < 3) break;
        const uint32_t info = data.Read();
        const uint32_t and_value = data.Read();
        const uint32_t or_value = data.Read();
        uint32_t value = Register(info & 0x1FFF);
        value &= ((info >> 31) & 0x1) ? Register(and_value & 0x1FFF) : and_value;
        value |= ((info >> 30) & 0x1) ? Register(or_value & 0x1FFF) : or_value;
        WriteRegister(info & 0x1FFF, value);
        break;
      }
      case xenos::PM4_REG_TO_MEM: {
        if (words < 2) break;
        const uint32_t reg = data.Read();
        const uint32_t address = data.Read();
        WriteMemory(address, Register(reg));
        break;
      }
      case xenos::PM4_MEM_WRITE: {
        if (words < 1) break;
        uint32_t address = data.Read();
        for (uint32_t i = 1; i < words; ++i) {
          WriteMemory(address, data.Read());
          address += 4;
        }
        break;
      }
      case xenos::PM4_COND_WRITE: {
        if (words < 6) break;
        const uint32_t info = data.Read();
        const uint32_t poll = data.Read();
        const uint32_t reference = data.Read();
        const uint32_t mask = data.Read();
        const uint32_t destination = data.Read();
        const uint32_t value_to_write = data.Read();
        const uint32_t value = (info & 0x10) ? ReadMemory(poll) : Register(poll);
        if (Compare(info, value & mask, reference)) {
          if (info & 0x100) {
            WriteMemory(destination, value_to_write);
          } else {
            WriteRegister(destination, value_to_write);
          }
        }
        break;
      }
      case xenos::PM4_EVENT_WRITE:
        if (words < 1) break;
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, data.Read() & 0x3F);
        break;
      case xenos::PM4_EVENT_WRITE_SHD: {
        if (words < 3) break;
        const uint32_t initiator = data.Read();
        const uint32_t address = data.Read();
        const uint32_t value = data.Read();
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        WriteMemory(address, ((initiator >> 31) & 0x1) ? counter_.load(std::memory_order_relaxed)
                                                       : value);
        break;
      }
      case xenos::PM4_EVENT_WRITE_EXT: {
        if (words < 2) break;
#if defined(FEAR_NX_RENDERER_DEKO3D)
        ++visibility_.extent_reports;
#endif
        const uint32_t initiator = data.Read();
        const uint32_t address = data.Read();
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        // The screen extent of the previous draw: the maximum is faked.
        const uint16_t extent[] = {0, kMaxExtent, 0, kMaxExtent, 0, 1};
        uint8_t* destination = memory_->TranslatePhysical(address & ~uint32_t(0x3));
        for (size_t i = 0; i < std::size(extent); ++i) {
          rex::memory::store_and_swap<uint16_t>(destination + i * 2, extent[i]);
        }
#if defined(FEAR_NX_RENDERER_DEKO3D)
        if (renderer_ready_) renderer_.InvalidateExtentWrite(address & ~uint32_t(0x3));
#endif
        break;
      }
      case xenos::PM4_EVENT_WRITE_ZPD: {
        if (words < 1) break;
#if defined(FEAR_NX_RENDERER_DEKO3D)
        ++visibility_.zpd_reports;
#endif
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, data.Read() & 0x3F);
        const uint32_t address = Register(rex::graphics::XE_GPU_REG_RB_SAMPLE_COUNT_ADDR);
        if (address) {
          FakeOcclusionResult(address);
        }
        break;
      }
      case xenos::PM4_SET_CONSTANT: {
        if (words < 1) break;
        const uint32_t type_index = data.Read();
        const uint32_t base = ConstantBase(type_index);
        if (base != UINT32_MAX) {
          const uint32_t index = base + (type_index & 0x7FF);
          WriteRegisters(index, words - 1, data);
        }
        break;
      }
      case xenos::PM4_SET_CONSTANT2:
      case xenos::PM4_SET_SHADER_CONSTANTS: {
        // Same format: the first value is the register index, the rest are
        // written consecutively.
        if (words < 1) break;
        const uint32_t index = data.Read() & 0xFFFF;
        WriteRegisters(index, words - 1, data);
        break;
      }
      case xenos::PM4_LOAD_ALU_CONSTANT: {
        // Registers loaded from guest memory instead of from the ring.
        if (words < 3) break;
        const uint32_t address = data.Read() & 0x3FFFFFFF;
        const uint32_t type_index = data.Read();
        const uint32_t amount = data.Read() & 0xFFF;
        const uint32_t base = ConstantBase(type_index);
        if (base != UINT32_MAX) {
          const uint8_t* source = memory_->TranslatePhysical(address);
          const uint32_t index = base + (type_index & 0x7FF);
          // Use the same changed-word and dirty-generation batching as
          // constants embedded in PM4 packets. Nonconstant ranges retain
          // WriteRegisters' per-register fallback and all its side effects.
          Reader constants;
          constants.base = source;
          constants.end = amount;
          WriteRegisters(index, amount, constants);
        }
        break;
      }
      case xenos::PM4_SET_BIN_MASK_LO:
        if (words < 1) break;
        bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | data.Read();
        break;
      case xenos::PM4_SET_BIN_MASK_HI:
        if (words < 1) break;
        bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (uint64_t(data.Read()) << 32);
        break;
      case xenos::PM4_SET_BIN_SELECT_LO:
        if (words < 1) break;
        bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | data.Read();
        break;
      case xenos::PM4_SET_BIN_SELECT_HI:
        if (words < 1) break;
        bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (uint64_t(data.Read()) << 32);
        break;
      case xenos::PM4_SET_BIN_MASK: {
        if (words < 2) break;
        const uint64_t high = data.Read();
        const uint64_t low = data.Read();
        bin_mask_ = (high << 32) | low;
        break;
      }
      case xenos::PM4_SET_BIN_SELECT: {
        if (words < 2) break;
        const uint64_t high = data.Read();
        const uint64_t low = data.Read();
        bin_select_ = (high << 32) | low;
        break;
      }
      case xenos::PM4_VIZ_QUERY: {
        // Like the emulation: when the query ends it is reported visible, in
        // case the game reads the result.
        if (words < 1) break;
        const uint32_t value = data.Read();
        const uint32_t id = value & 0x3F;
        if (!(value & 0x100)) {
#if defined(FEAR_NX_RENDERER_DEKO3D)
          ++visibility_.viz_begin;
#endif
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, xenos::VIZQUERY_START);
        } else {
#if defined(FEAR_NX_RENDERER_DEKO3D)
          ++visibility_.viz_end;
#endif
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, xenos::VIZQUERY_END);
          const uint32_t reg = id < 32 ? uint32_t(rex::graphics::XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_0)
                                       : uint32_t(rex::graphics::XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_1);
          registers_[reg] |= UINT32_C(1) << (id < 32 ? id : id - 32);
        }
        break;
      }
      case xenos::PM4_DRAW_INDX:
      case xenos::PM4_DRAW_INDX_2: {
        // DRAW_INDX starts with the visibility query word.
        const uint32_t skip = opcode == xenos::PM4_DRAW_INDX ? 1 : 0;
        if (words < skip + 1) break;
        // Bit 8 selects a VIZ result (ID in bits 0..5). Until real queries
        // exist, keep the visible fallback; only count conditional draws.
        [[maybe_unused]] const uint32_t visibility_token = skip ? data.Read() : 0;
        const uint32_t initiator = data.Read();
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR, initiator);
        // Indices in a buffer (DMA): its base and size come in the packet and
        // are stored in their registers, as ExecutePacketType3Draw does.
        if (((initiator >> 6) & 0x3) == uint32_t(xenos::SourceSelect::kDMA)) {
          if (words < skip + 3) break;
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_DMA_BASE, data.Read());
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_DMA_SIZE, data.Read());
        }
        // Indices in the packet itself.
        const uint32_t* immediate = nullptr;
        uint32_t immediate_words = 0;
        if (((initiator >> 6) & 0x3) == uint32_t(xenos::SourceSelect::kImmediate)) {
          immediate_words = words - skip - 1;
          immediate_indices_.resize(immediate_words);
          for (uint32_t i = 0; i < immediate_words; ++i) {
            immediate_indices_[i] = data.Read();
          }
          immediate = immediate_indices_.data();
        }
        // A draw in copy mode is a resolve and/or a clear.
        if ((Register(rex::graphics::XE_GPU_REG_RB_MODECONTROL) & 0x7) ==
            uint32_t(xenos::EdramMode::kCopy)) {
          ++copies_;
          if (renderer_ready_) {
            renderer_.Resolve();
          }
        } else {
          ++draws_;
#if defined(FEAR_NX_RENDERER_DEKO3D)
          const uint32_t viz = Register(rex::graphics::XE_GPU_REG_PA_SC_VIZ_QUERY);
          visibility_.conditional_draws += (visibility_token & 0x100u) != 0;
          visibility_.query_enabled_draws += (viz & 0x1u) != 0;
          visibility_.kill_hi_z_draws += (viz & 0x80u) != 0;
          visibility_.kill_detail_draws += (viz & 0x100u) != 0;
#endif
          if (renderer_ready_) {
            renderer_.Draw(immediate, immediate_words);
          }
        }
        break;
      }
      case xenos::PM4_IM_LOAD: {
        // The shader microcode from guest memory (CommandProcessor::
        // ExecutePacketType3_IM_LOAD).
        if (words < 2) break;
        const uint32_t address_type = data.Read();
        const uint32_t start_size = data.Read();
        const uint32_t dwords = start_size & 0xFFFF;
        const uint32_t address = address_type & ~uint32_t(0x3) & 0x1FFFFFFF;
        if (renderer_ready_ && dwords && uint64_t(address) + dwords * 4 <= 0x20000000u) {
          renderer_.LoadShader(xenos::ShaderType(address_type & 0x3),
                               memory_->TranslatePhysical<const uint32_t*>(address), dwords,
                               address);
        }
        break;
      }
      case xenos::PM4_IM_LOAD_IMMEDIATE: {
        // The microcode is in the packet, which may wrap around the ring.
        if (words < 2) break;
        const uint32_t type = data.Read();
        const uint32_t dwords = std::min(data.Read() & 0xFFFF, words - 2);
        shader_words_.resize(dwords);
        for (uint32_t i = 0; i < dwords; ++i) {
          // Kept in guest byte order, as the shader expects.
          shader_words_[i] = rex::byte_swap(data.Read());
        }
        if (renderer_ready_ && dwords) {
          renderer_.LoadShader(xenos::ShaderType(type & 0x3), shader_words_.data(), dwords, 0);
        }
        break;
      }
      default:
        // Cache invalidations and the rest: nothing to do.
        break;
    }
  }

  static uint32_t ConstantBase(uint32_t type_index) {
    switch ((type_index >> 16) & 0xFF) {
      case 0:
        return 0x4000;  // ALU
      case 1:
        return 0x4800;  // fetch
      case 2:
        return 0x4900;  // bool
      case 3:
        return 0x4908;  // loop
      case 4:
        return 0x2000;  // registers
      default:
        return UINT32_MAX;
    }
  }

  // Match the SDK fallback: honor its configurable fake sample count, and
  // only replace the query result when the guest marks the query complete.
  void FakeOcclusionResult(uint32_t address) {
    const int32_t fake_sample_count = REXCVAR_GET(query_occlusion_fake_sample_count);
    if (fake_sample_count < 0) return;
    auto* counts = memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(address);
    const uint32_t kFinished = rex::byte_swap(UINT32_C(0xFFFFFEED));
    const bool end = counts->ZPass_A == kFinished || counts->ZPass_B == kFinished ||
                     counts->ZFail_A == kFinished || counts->ZFail_B == kFinished;
    std::memset(counts, 0, sizeof(*counts));
    if (end) {
      counts->ZPass_A = uint32_t(fake_sample_count);
      counts->Total_A = uint32_t(fake_sample_count);
    }
#if defined(FEAR_NX_RENDERER_DEKO3D)
    if (renderer_ready_) renderer_.InvalidateGuestMemory(GuestMemoryBoundary::kOcclusionWrite);
#endif
  }

  void IndirectBuffer(uint32_t address, uint32_t words, int depth) {
    Reader reader;
    reader.base = memory_->TranslatePhysical(address);
    reader.pos = 0;
    reader.end = words;
    while (reader.Pending()) {
      if (!Packet(reader, depth)) {
        break;
      }
    }
  }

  // --- Output presentation -----------------------------------------------------------

  // A frame longer than 50 ms on the ring thread, broken down, at most 12
  // per report period: the drops left are single long frames, which the 10 s
  // averages hide.
  void ReportSlowFrame() {
    const auto now = Clock::now();
    const uint64_t game_wait_ns = wait_ns_ - frame_start_wait_ns_;
    frame_start_wait_ns_ = wait_ns_;
    const uint64_t idle_ns = ring_idle_ns_ - frame_start_idle_ns_;
    frame_start_idle_ns_ = ring_idle_ns_;
    const uint64_t hold_ns = hold_ns_ - frame_start_hold_ns_;
    frame_start_hold_ns_ = hold_ns_;
    const auto previous = frame_start_;
    frame_start_ = now;
    if (!renderer_ready_ || previous.time_since_epoch().count() == 0) return;
    const double ms =
        double(std::chrono::duration_cast<std::chrono::microseconds>(now - previous).count()) /
        1000.0;
    const RendererStats& s = renderer_.stats();
    const uint64_t pipelines = s.pipelines_created - frame_start_pipelines_;
    const uint64_t shaders = s.shaders_translated - frame_start_shaders_;
#if !defined(FEAR_NX_RENDERER_DEKO3D)
    const uint64_t textures = s.textures_uploaded - frame_start_textures_;
    const uint64_t created = s.textures_created - frame_start_created_;
#endif
    frame_start_pipelines_ = s.pipelines_created;
    frame_start_shaders_ = s.shaders_translated;
#if !defined(FEAR_NX_RENDERER_DEKO3D)
    frame_start_textures_ = s.textures_uploaded;
    frame_start_created_ = s.textures_created;
#endif
    if (ms < 50.0 || slow_frames_logged_ >= 12) return;
    ++slow_frames_logged_;
#if defined(FEAR_NX_RENDERER_DEKO3D)
    REXLOG_INFO(
        "[native] slow frame {:.0f} ms: ring idle {:.1f} ms, pacing hold {:.1f} ms",
        ms, double(idle_ns) / 1e6, double(hold_ns) / 1e6);
    REXLOG_INFO(
        "[native] slow frame {:.0f} ms: waited for the game {:.1f} ms, renderer fence {:.1f} ms, "
        "texture draw phase {:.1f} ms; {} draws, {} shaders, {} pipelines",
        ms, double(game_wait_ns) / 1e6, double(s.frame_fence_wait_ns) / 1e6,
        double(s.frame_texture_ns) / 1e6, s.frame_draws, shaders, pipelines);
#else
    REXLOG_INFO(
        "[native] slow frame {:.0f} ms: ring idle {:.1f} ms, pacing hold {:.1f} ms",
        ms, double(idle_ns) / 1e6, double(hold_ns) / 1e6);
    REXLOG_INFO(
        "[native] slow frame {:.0f} ms: waited for the game {:.1f} ms, the GPU {:.1f} ms "
        "({:.1f} of it for stream space), texture untiling {:.1f} ms, the draw thread {:.1f} ms; "
        "textures {:.1f} ms; {} draws, {} texture uploads ({} made now), {} shaders, {} pipelines",
        ms, double(game_wait_ns) / 1e6, double(s.frame_fence_wait_ns) / 1e6,
        double(s.frame_stream_wait_ns) / 1e6, double(s.frame_upload_wait_ns) / 1e6,
        double(s.frame_drain_wait_ns) / 1e6, double(s.frame_texture_ns) / 1e6, s.frame_draws,
        textures, created, shaders, pipelines);
#endif
  }

  // The frame rate cap (nx_min_frame_vblanks): a frame is presented at a
  // vblank at least that many after the previous present, or at once if it is
  // later than that. The hold used to come after the present, before the
  // swap-complete interrupt: an early frame went on screen at once and the
  // next one, if a little over budget, 50 ms later, a short gap then a long
  // one, felt as a hitch even at 30 fps on average. Held before it, presents
  // are evenly spaced, and the interrupt still follows the present.
  void PaceFrame() {
    const uint32_t min_vblanks = REXCVAR_GET(nx_min_frame_vblanks);
    if (min_vblanks > 1 && last_swap_vblank_) {
      const uint64_t target = last_swap_vblank_ + min_vblanks;
      if (REXCVAR_GET(nx_pace_before_present) &&
          vblanks_.load(std::memory_order_acquire) < target && renderer_ready_) {
        // The frame's draws go to the GPU now, not after the wait.
#if defined(FEAR_NX_RENDERER_DEKO3D)
        const bool profile_enabled = renderer_.CpuProfilingEnabled();
        fear_nx::deko::ProfileScope profile(ring_flush_ns_, profile_enabled);
        fear_nx::deko::ProfileScope packet_profile(ring_packet_external_ns_, profile_enabled);
#endif
        renderer_.Flush();
      }
      const auto hold_start = Clock::now();
      const auto limit = hold_start + std::chrono::milliseconds(100);
      while (vblanks_.load(std::memory_order_acquire) < target &&
             running_.load(std::memory_order_acquire) && Clock::now() < limit) {
        rex::thread::Sleep(std::chrono::microseconds(250));
      }
      hold_ns_ += uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - hold_start)
              .count());
    }
    last_swap_vblank_ = vblanks_.load(std::memory_order_acquire);
  }

  void Present() {
    const uint64_t swap = ++swaps_;
    g_swaps_presented.fetch_add(1, std::memory_order_relaxed);
    counter_.fetch_add(1, std::memory_order_relaxed);
#if defined(__SWITCH__)
    RexSwitchPerfCount(0);
#endif
    if (renderer_ready_) {
      renderer_.Swap(presenter_.get(), swap_frontbuffer_, swap_width_, swap_height_);
      return;
    }
    if (!presenter_) return;
#if !defined(FEAR_NX_RENDERER_DEKO3D)
    presenter_->RefreshGuestOutput(
        kOutputWidth, kOutputHeight, kOutputWidth, kOutputHeight,
        [this, swap](rex::ui::Presenter::GuestOutputRefreshContext& context) {
          return ClearOutput(static_cast<OutputContext&>(context), swap);
        });
#else
    (void)swap;
#endif
  }

#if !defined(FEAR_NX_RENDERER_DEKO3D)
  bool ClearOutput(OutputContext& context, uint64_t swap) {
    const rex::ui::vulkan::VulkanDevice* device_info = provider_->vulkan_device();
    const auto& dfn = device_info->functions();
    const VkDevice device = device_info->device();

    if (render_pass_ == VK_NULL_HANDLE) {
      VkAttachmentDescription attachment{};
      attachment.format = rex::ui::vulkan::VulkanPresenter::kGuestOutputFormat;
      attachment.samples = VK_SAMPLE_COUNT_1_BIT;
      attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      attachment.finalLayout = rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout;
      VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      VkSubpassDescription subpass{};
      subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
      subpass.colorAttachmentCount = 1;
      subpass.pColorAttachments = &reference;
      VkRenderPassCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
      info.attachmentCount = 1;
      info.pAttachments = &attachment;
      info.subpassCount = 1;
      info.pSubpasses = &subpass;
      if (dfn.vkCreateRenderPass(device, &info, nullptr, &render_pass_) != VK_SUCCESS) {
        render_pass_ = VK_NULL_HANDLE;
        return false;
      }
    }

    if (pool_ == VK_NULL_HANDLE) {
      VkCommandPoolCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      info.queueFamilyIndex = device_info->queue_family_graphics_compute();
      if (dfn.vkCreateCommandPool(device, &info, nullptr, &pool_) != VK_SUCCESS) {
        pool_ = VK_NULL_HANDLE;
        return false;
      }
      VkCommandBufferAllocateInfo allocate{};
      allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      allocate.commandPool = pool_;
      allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      allocate.commandBufferCount = 1;
      if (dfn.vkAllocateCommandBuffers(device, &allocate, &commands_) != VK_SUCCESS) {
        commands_ = VK_NULL_HANDLE;
      }
      VkFenceCreateInfo fence_info{};
      fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      if (dfn.vkCreateFence(device, &fence_info, nullptr, &fence_) != VK_SUCCESS) {
        fence_ = VK_NULL_HANDLE;
      }
    }
    if (commands_ == VK_NULL_HANDLE || fence_ == VK_NULL_HANDLE) {
      return false;
    }

    // A single command buffer: wait for the previous one before reusing it.
    if (fence_pending_) {
      dfn.vkWaitForFences(device, 1, &fence_, VK_TRUE, UINT64_MAX);
      dfn.vkResetFences(device, 1, &fence_);
      fence_pending_ = false;
    }
    dfn.vkResetCommandPool(device, pool_, 0);

    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    for (const Framebuffer& f : framebuffers_) {
      if (f.framebuffer != VK_NULL_HANDLE && f.version == context.image_version()) {
        framebuffer = f.framebuffer;
      }
    }
    if (framebuffer == VK_NULL_HANDLE) {
      Framebuffer& f = framebuffers_[next_framebuffer_];
      next_framebuffer_ = (next_framebuffer_ + 1) % std::size(framebuffers_);
      if (f.framebuffer != VK_NULL_HANDLE) {
        dfn.vkDestroyFramebuffer(device, f.framebuffer, nullptr);
        f.framebuffer = VK_NULL_HANDLE;
      }
      VkImageView view = context.image_view();
      VkFramebufferCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      info.renderPass = render_pass_;
      info.attachmentCount = 1;
      info.pAttachments = &view;
      info.width = kOutputWidth;
      info.height = kOutputHeight;
      info.layers = 1;
      if (dfn.vkCreateFramebuffer(device, &info, nullptr, &f.framebuffer) != VK_SUCCESS) {
        f.framebuffer = VK_NULL_HANDLE;
        return false;
      }
      f.version = context.image_version();
      framebuffer = f.framebuffer;
    }

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dfn.vkBeginCommandBuffer(commands_, &begin) != VK_SUCCESS) {
      return false;
    }
    // Test colour: green rises and falls with the swap count, so a capture
    // tells a running game from a stalled one.
    const float phase = float(swap % 240) / 239.0f;
    VkClearValue color{};
    color.color.float32[0] = 0.05f;
    color.color.float32[1] = 0.10f + 0.40f * phase;
    color.color.float32[2] = 0.35f;
    color.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = render_pass_;
    pass.framebuffer = framebuffer;
    pass.renderArea.extent.width = kOutputWidth;
    pass.renderArea.extent.height = kOutputHeight;
    pass.clearValueCount = 1;
    pass.pClearValues = &color;
    dfn.vkCmdBeginRenderPass(commands_, &pass, VK_SUBPASS_CONTENTS_INLINE);
    dfn.vkCmdEndRenderPass(commands_);
    if (dfn.vkEndCommandBuffer(commands_) != VK_SUCCESS) {
      return false;
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands_;
    {
      const auto queue =
          device_info->AcquireQueue(device_info->queue_family_graphics_compute(), 0);
      if (dfn.vkQueueSubmit(queue.queue(), 1, &submit, fence_) != VK_SUCCESS) {
        return false;
      }
    }
    fence_pending_ = true;
    context.SetIs8bpc(true);
    return true;
  }

  void DestroyVulkan() {
    if (!provider_ || !provider_->vulkan_device()) {
      return;
    }
    const rex::ui::vulkan::VulkanDevice* device_info = provider_->vulkan_device();
    const auto& dfn = device_info->functions();
    const VkDevice device = device_info->device();
    if (fence_ != VK_NULL_HANDLE) {
      if (fence_pending_) {
        dfn.vkWaitForFences(device, 1, &fence_, VK_TRUE, UINT64_MAX);
        fence_pending_ = false;
      }
      dfn.vkDestroyFence(device, fence_, nullptr);
      fence_ = VK_NULL_HANDLE;
    }
    for (Framebuffer& f : framebuffers_) {
      if (f.framebuffer != VK_NULL_HANDLE) {
        dfn.vkDestroyFramebuffer(device, f.framebuffer, nullptr);
        f = Framebuffer{};
      }
    }
    if (pool_ != VK_NULL_HANDLE) {
      dfn.vkDestroyCommandPool(device, pool_, nullptr);
      pool_ = VK_NULL_HANDLE;
      commands_ = VK_NULL_HANDLE;
    }
    if (render_pass_ != VK_NULL_HANDLE) {
      dfn.vkDestroyRenderPass(device, render_pass_, nullptr);
      render_pass_ = VK_NULL_HANDLE;
    }
  }
#endif

  // --- State -------------------------------------------------------------------------

  rex::ui::WindowedAppContext* app_context_ = nullptr;
  std::unique_ptr<BackendProvider> provider_;
  std::unique_ptr<rex::ui::Presenter> presenter_;

  rex::runtime::FunctionDispatcher* dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  rex::memory::Memory* memory_ = nullptr;
  bool mmio_registered_ = false;

  std::atomic<bool> running_{false};
  rex::system::object_ref<rex::system::XHostThread> vblank_thread_;
  rex::system::object_ref<rex::system::XHostThread> ring_thread_;

  std::atomic<uint32_t> callback_{0};
  std::atomic<uint32_t> callback_data_{0};
  // Incremented by vblanks and swaps; EVENT_WRITE_SHD can write it (the SDK's
  // command processor counter).
  std::atomic<uint32_t> counter_{0};
  // Vblanks delivered, and the vblank of the last swap-complete interrupt.
  std::atomic<uint64_t> vblanks_{0};
  uint64_t last_swap_vblank_ = 0;
  Clock::time_point frame_start_{};
  uint64_t frame_start_wait_ns_ = 0, frame_start_pipelines_ = 0, frame_start_shaders_ = 0,
           frame_start_textures_ = 0, frame_start_created_ = 0;
  uint32_t slow_frames_logged_ = 0;
  uint64_t ring_idle_ns_ = 0, hold_ns_ = 0;
  uint64_t frame_start_idle_ns_ = 0, frame_start_hold_ns_ = 0;
  bool swap_interrupt_pending_ = false;

  std::atomic<uint32_t> ring_base_{0};
  std::atomic<uint32_t> ring_words_{0};
  std::atomic<uint32_t> ring_generation_{0};
  std::atomic<uint32_t> write_pointer_{0};
  std::atomic<uint32_t> read_pointer_writeback_{0};
  std::mutex ring_mutex_;
  std::condition_variable ring_cv_;

  // Touched by the ring thread and, through MMIO, by the game's threads; as in
  // the emulation, races on these integers are harmless.
  // The renderer reads the GPU state from here: it is the SDK's register file,
  // so its helpers (draw_util and the rest) work on it unchanged.
  std::unique_ptr<rex::graphics::RegisterFile> register_file_;
  uint32_t* registers_;
  ActiveRenderer renderer_;
  bool renderer_ready_ = false;
  // CommandProcessor's gamma ramp state (DC_LUT_*), 256-entry table only.
  uint32_t gamma_ramp_[256] = {};
  uint32_t gamma_ramp_component_ = 0;
  std::vector<uint32_t> immediate_indices_;
  std::vector<uint32_t> shader_words_;
  uint64_t bin_mask_ = ~uint64_t(0);
  uint64_t bin_select_ = ~uint64_t(0);

  uint32_t swap_frontbuffer_ = 0;
  uint32_t swap_width_ = 0;
  uint32_t swap_height_ = 0;

  // Ring thread only.
  uint64_t packets_ = 0, draws_ = 0, copies_ = 0, swaps_ = 0, waits_cut_short_ = 0;
  uint64_t waits_ = 0, wait_ns_ = 0, reported_waits_ = 0, reported_wait_ns_ = 0;
  uint64_t reported_packets_ = 0, reported_draws_ = 0, reported_copies_ = 0,
           reported_swaps_ = 0;
#if defined(FEAR_NX_RENDERER_DEKO3D)
  struct VisibilityCounters {
    uint64_t viz_begin = 0, viz_end = 0, zpd_reports = 0, extent_reports = 0;
    uint64_t conditional_draws = 0, query_enabled_draws = 0;
    uint64_t kill_hi_z_draws = 0, kill_detail_draws = 0;
  };
  VisibilityCounters visibility_, reported_visibility_;
  // Ring-thread CPU wall time. Top-level packet timing is inclusive of nested
  // packet handling; KickRecorder/Flush are measured separately.
  uint64_t ring_packet_ns_ = 0, reported_ring_packet_ns_ = 0;
  uint64_t ring_packet_external_ns_ = 0, reported_ring_packet_external_ns_ = 0;
  uint64_t ring_top_level_packets_ = 0, reported_ring_top_level_packets_ = 0;
  uint64_t ring_kick_recorder_ns_ = 0, reported_ring_kick_recorder_ns_ = 0;
  uint64_t ring_flush_ns_ = 0, reported_ring_flush_ns_ = 0;
  uint64_t ring_wait_kick_ns_ = 0, reported_ring_wait_kick_ns_ = 0;
  uint64_t reported_ring_hold_ns_ = 0;
  uint64_t ring_backlog_coalesced_ = 0, reported_ring_backlog_coalesced_ = 0;
  uint64_t ring_idle_kicks_ = 0, reported_ring_idle_kicks_ = 0;
#endif

#if !defined(FEAR_NX_RENDERER_DEKO3D)
  struct Framebuffer {
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    uint64_t version = 0;
  };
  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer commands_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  bool fence_pending_ = false;
  Framebuffer framebuffers_[3];
  size_t next_framebuffer_ = 0;
#endif
};

}  // namespace

bool Enabled() { return REXCVAR_GET(nx_renderer) == "native"; }

uint64_t SwapsPresented() { return g_swaps_presented.load(std::memory_order_relaxed); }

std::unique_ptr<rex::system::IGraphicsSystem> CreateGraphicsSystem() {
  return std::make_unique<NativeGraphicsSystem>();
}

}  // namespace fear_nx::native
