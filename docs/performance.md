# Vulkan performance and change history

## Switch performance in v0.2.0

These figures were observed on a physical Nintendo Switch running the Vulkan
build on 2026-11-06. The **CPU is now the main bottleneck** in demanding
gameplay. Raising just the CPU from 1020 to 1224 MHz with the GPU fixed at
460 MHz made drops in heavy firefights less frequent.

| CPU | GPU | RAM | Observed gameplay |
|---|---|---|---|
| 1020 MHz (stock) | 460 MHz | — | 30 FPS in normal play, with drops to 25 FPS in heavy firefights. |
| 1224 MHz | 460 MHz | — | Usually holds 30 FPS, with rare drops to 25 FPS in heavy firefights. |
| 1428 MHz | 614 MHz | At least 1800 MHz | Stable 30 FPS in tested gameplay, including heavy firefights. |

RAM speed was reported only for the last configuration. These are the clocks
used for the observations. The game does not set them automatically.

## What changed in v0.2.0

- The game submits one full 1280×720 scene rectangle. Earlier builds replayed
  the main scene in two tiles. The same full-height layout fixes the half-screen
  image seen while a level loaded.
- Vulkan uses single-sample color and depth targets. Dynamic water and mirror
  reflections are disabled. Water refraction and ordinary camera targets still work.
- The translated game code uses direct calls, GCC link-time optimization and
  a gameplay profile recorded on Switch. The release has no recording counters
  or per-pass GPU timers.

## Earlier Vulkan builds

On 2026-10-22, older tiled builds at 460 MHz GPU reached 30 FPS in normal
gameplay, 25–27 FPS in heavy firefights and about 17 FPS in some GPU-bound
areas. The table above describes the full-frame v0.2.0 build.

## Renderer changes retained in main

| Stage | Change | Result and limits |
|---|---|---|
| Shared resolves | Reuse compatible destination images, preserve crop origins and update changed regions. | Reduces redundant composition work; it does not eliminate guest EDRAM tile replay. |
| Depth and alias clears | Propagate supported depth/stencil and zero depth/color clears; correct D24FS8 conversion and exact depth bias. | Source-level correctness fixes; not proof that all graphical artifacts are gone. |
| ZCULL preparation | Make eligible depth attachments usable by the driver's depth hierarchy; retain preservation fallbacks. | Enabled by default. Allocation eligibility alone is not a measurement of GPU savings. |
| Gamma presentation | Fuse supported gamma conversion into final presentation while retaining a stable mailbox snapshot. | Removes a separate compute pass for supported formats; fallback paths remain. |
| Resolve barriers and full clears | Use precise resolve dependencies and attachment load clears only when the rectangle fully covers the active render area. | Focused host checks and a Switch build passed; a separate matched hardware timing is not available. |
| Correctness audit, 2026-10-22 | Preserve resolve layout/lifetime and fallback invalidation; retain conservative handling of unsupported EDRAM cases. | Built and packaged successfully. At that checkpoint, half-screen and decal defects were unresolved; see the current status below. |

Version 0.1.1 adds constant depth/color and zero-color alias clears for weapon
displays and other reused targets, per-frame linear R8 video-plane updates,
correct uniform-descriptor writes, non-coherent upload flushing, MSAA depth
sample resolves and a fix for descriptor-cache rollover. Weapon displays and
the scene-copy effect are not hidden. The user confirmed the earlier weapon
display correction and improved performance with the balanced quality profile;
hardware confirmation of the final descriptor-lifetime change was still pending
at that release checkpoint. Current reported visual status is recorded below.

Mesa patches 0007 (GPFIFO capacity) and 0008 (depth/stencil memory bindings)
are included. Patch 0008 may disable compression on images needing a separate
stencil-copy plane. No new controlled FPS result is claimed for this release.
The Eden-only storage-buffer compatibility fix is not part of the Switch NRO.

Texture-sign specialization and eligible implicit-LOD sampling remain enabled.
The pre-0.2.0 profile kept profiling/tracing off, anisotropy at 2x, host MSAA off,
`native_record_thread = false` and `gpu_mhz = 0`. Host MSAA off does not disable
the guest game's tile scheduling. A recording-thread experiment was worse in
the user's comparison and is not enabled.


Version 0.1.2 disables routine logging, periodic log flushing, Vulkan validation,
GPU verification/statistics and the separate Switch performance report by
default. The NRO also defaults to quiet logging when no TOML is present.
FPS/resolution overlays, shader caches and the existing graphics profile remain.
To update an old diagnostic installation, replace both the NRO and the supplied
TOML (back up custom settings first); explicit settings in an old TOML still win.
In versions 0.1.2–0.1.4, `[switch] perf_report = true` enabled deliberate profiling.
No new controlled hardware FPS measurement is claimed.

Version 0.1.3 fixes the reported stale-texture artifact when selecting an
affected weapon on Switch. It prevents reuse of stale vertex/index uploads
after the game rewrites data within a frame. Fresh content checks and safe
vertex spans are enabled by default; unchanged uploads are
reused, and only shader-proven spans are checked where possible. The user
confirmed the fix on Switch with the preceding 0.1.2-vf1 build on 2026-10-25.
Weapon displays remain enabled. No new controlled FPS comparison is available.

The freshness/span implementation is adapted from
[fear360-recomp fc572279](https://github.com/Juansolo457/fear360-recomp/commit/fc5722793f71217ca888c329d654bba6f9eeeedc).
It retains local upload-ring lifetime checks and non-coherent memory flushing.
Unsafe span analysis falls back to checking the full active buffer; it does not
skip content validation. No new explicit GPU synchronization path is added;
its effect on existing upload-ring waits and FPS has not been measured.

Version 0.1.4 uses the game's medium effect detail (`ClientFXDetailLevel = 1`)
and halves eligible particle bounce/splat probabilities (`ParticleBounceScale = 0.5`).
Bloom uses the original game behavior. Texture mip selection and anisotropic
filtering retain the v0.1.3 release values. The user accepted the preceding
0.1.3-fx3 build on Switch on 2026-10-25; no controlled FPS gain is claimed.

Version 0.1.4 resolved the reported weapon, decal and screen-flicker defects.
Version 0.2.0 fixes the half-screen scene during level loading.
