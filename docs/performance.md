# Vulkan performance and change history

Updated 2026-10-23. `main` retains the Vulkan/Mesa/NVK renderer. The separate
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

Texture-sign specialization and eligible implicit-LOD sampling remain enabled.
The normal profile keeps profiling/tracing off, anisotropy at 2x, host MSAA off,
`native_record_thread = false` and `gpu_mhz = 0`. Host MSAA off does not disable
the guest game's tile scheduling. A recording-thread experiment was worse in
the user's comparison and is not enabled.

## Next work

- Find reproducible triggers for incomplete frames and decal artifacts.
- Measure expensive GPU passes and resolve traffic on the same checkpoint.
- Optimize CPU game work in the deko3d branch, whose current bottleneck differs
  from the earlier Vulkan hardware observations.

Compare the same save, camera, clocks, resolution and warmed shader cache.
CPU wall timers include waits and are nested; they cannot be summed as separate
costs or converted directly into a Switch GPU frame time. Build success is not
runtime or FPS validation.
