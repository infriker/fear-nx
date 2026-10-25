# Vulkan performance and change history

Updated 2026-10-25. `main` retains the Vulkan/Mesa/NVK renderer. The separate
[`deko3d` branch](https://github.com/infriker/fear-nx/tree/deko3d) contains the
experimental backend and its own test history.

## Hardware observations

Earlier Switch tests at a reported 460 MHz GPU reached about 30 FPS in most
gameplay, 25–27 FPS in heavy firefights, and approximately 17 FPS in unusually
heavy areas. GPU utilization reached 100% in those drops while CPU headroom
remained. These measurements describe the preceding test builds, not a new
controlled benchmark of the latest fixes.

The current user comparison reports 20–30 FPS with overclocking on deko3d,
with frequent drops. **Vulkan currently provides more stable performance.**
Scenes, settings and runs were not identical; no percentage advantage is claimed.

## Renderer changes retained in main

| Stage | Change | Result and limits |
|---|---|---|
| Shared resolves | Reuse compatible destination images, preserve crop origins and update changed regions. | Reduces redundant composition work; it does not eliminate guest EDRAM tile replay. |
| Depth and alias clears | Propagate supported depth/stencil and zero depth/color clears; correct D24FS8 conversion and exact depth bias. | Source-level correctness fixes; not proof that all graphical artifacts are gone. |
| ZCULL preparation | Make eligible depth attachments usable by the driver's depth hierarchy; retain preservation fallbacks. | Enabled by default. Allocation eligibility alone is not a measurement of GPU savings. |
| Gamma presentation | Fuse supported gamma conversion into final presentation while retaining a stable mailbox snapshot. | Removes a separate compute pass for supported formats; fallback paths remain. |
| Resolve barriers and full clears | Use precise resolve dependencies and attachment load clears only when the rectangle fully covers the active render area. | Focused host checks and a Switch build passed; a separate matched hardware timing is not available. |
| Correctness audit, 2026-10-22 | Preserve resolve layout/lifetime and fallback invalidation; retain conservative handling of unsupported EDRAM cases. | Built and packaged successfully. Remaining half-screen and decal defects have unidentified triggers. |

Version 0.1.1 adds constant depth/color and zero-color alias clears for weapon
displays and other reused targets, per-frame linear R8 video-plane updates,
correct uniform-descriptor writes, non-coherent upload flushing, MSAA depth
sample resolves and a fix for descriptor-cache rollover. Weapon displays and
the scene-copy effect are not hidden. The user confirmed the earlier weapon
display correction and improved performance with the balanced quality profile;
the final descriptor-lifetime change still needs hardware confirmation.

Mesa patches 0007 (GPFIFO capacity) and 0008 (depth/stencil memory bindings)
are included. Patch 0008 may disable compression on images needing a separate
stencil-copy plane. No new controlled FPS result is claimed for this release.
The Eden-only storage-buffer compatibility fix is not part of the Switch NRO.

Texture-sign specialization and eligible implicit-LOD sampling remain enabled.
The normal profile keeps profiling/tracing off, anisotropy at 2x, host MSAA off,
`native_record_thread = false` and `gpu_mhz = 0`. Host MSAA off does not disable
the guest game's tile scheduling. A recording-thread experiment was worse in
the user's comparison and is not enabled.


Version 0.1.2 disables routine logging, periodic log flushing, Vulkan validation,
GPU verification/statistics and the separate Switch performance report by
default. The NRO also defaults to quiet logging when no TOML is present.
FPS/resolution overlays, shader caches and the existing graphics profile remain.
To update an old diagnostic installation, replace both the NRO and the supplied
TOML (back up custom settings first); explicit settings in an old TOML still win.
For deliberate profiling, set `[switch] perf_report = true` and restart.
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

## Next work

- Find reproducible triggers for incomplete frames and decal artifacts.
- Measure expensive GPU passes and resolve traffic on the same checkpoint.
- Optimize CPU game work in the deko3d branch, whose current bottleneck differs
  from the earlier Vulkan hardware observations.

Compare the same save, camera, clocks, resolution and warmed shader cache.
CPU wall timers include waits and are nested; they cannot be summed as separate
costs or converted directly into a Switch GPU frame time. Build success is not
runtime or FPS validation.
