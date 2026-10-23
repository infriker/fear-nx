# Native Vulkan renderer

This document describes the retained optional Vulkan path. This branch defaults
to deko3d; see the [README](../README.md) and [test history](deko3d-history.md).

The renderer consumes the Xbox 360 Xenos command ring produced by the game and
records Vulkan commands. Game shaders are translated to SPIR-V by ReXGlue;
Mesa/NVK executes them on the Switch GPU. The source is in `src/native/`.

Render targets use host images. Shared resolve destinations preserve the
relationship between tiled segments of a guest texture. Compatible views of
EDRAM propagate stencil clears and zero depth/color clears, preventing stale
shadow masks and light-ray occlusion data. D24 depth bias uses
`VK_EXT_depth_bias_control` when the device exposes exact bias support.

These alias clear paths do not implement general EDRAM ownership transfers. See
[known issues](known-issues.md) for unresolved visual symptoms and
[performance history](performance.md) for the scope of the retained fixes.

## Configuration

These keys belong under `[nx]` in `fear-nx.toml`:

| Key | Release value | Purpose |
|---|---|---|
| `renderer` | `"native"` | Select the app-owned Vulkan renderer |
| `native_shared_resolves` | `true` | Share validated resolve destinations |
| `native_depth24` | `true` | D24 storage for compatible guest depth surfaces |
| `native_msaa` | `false` | Disable host MSAA |
| `native_max_anisotropy` | `2` | Limit anisotropic filtering to 2× |
| `native_async_pipelines` | `true` | Compile pipelines asynchronously |
| `native_record_thread` | `false` | Keep command recording on the current path |
| `native_fused_gamma` | `true` | Apply gamma in the final output shader for supported frontbuffers |
| `native_zcull` | `true` | Allocate depth attachments eligible for persistent NVK ZCULL |
| `native_precise_resolve_barriers` | `true` | Omit a redundant transfer wait on compute resolve/crop paths |
| `native_full_clear_load_op` | `true` | Clear a fully covered render area at attachment load |
| `native_gpu_profile` | `false` | Collect GPU pass diagnostics when explicitly enabled |
| `native_trace_output` | `false` | Record bounded renderer diagnostics when explicitly enabled |

The release disables profiling and trace output and uses warning-level logging
for normal play. Enable diagnostics separately when collecting a capture. Clock selection
is separate: `[switch] gpu_mhz = 0` leaves the system profile unchanged.
`[input] xbox_layout` changes face-button mapping, not rendering.

## ZCULL

`native_zcull` is enabled by default, including with an older configuration that
does not contain the key. Set `native_zcull = false` and fully restart to compare
against the previous depth-allocation path. Keep the other settings and clocks
fixed for an A/B test; `native_record_thread` should remain `false`.

The bundled NVK already supports persistent ZCULL on Switch. Depth images now
omit `TRANSFER_DST` and start with an attachment clear, allowing NVK to allocate,
load and store their depth hierarchy across rendering scopes. Normal passes
still use `LOAD`, preserving depth and stencil. This does not require a driver
or ReXGlue version change.

When a depth image must grow, the renderer preserves its old depth and stencil
samples with the existing transfer copy. That replacement temporarily uses the
previous allocation path without persistent ZCULL. It becomes eligible again
when a subsequent guest depth/stencil clear covers its entire image, including
padded rows and after clipping to any color attachment. Partial and stencil-only
clears never discard the image. Dimensions and MSAA storage are unchanged.

With info logging, render-target creation reports `zcull_eligible=true` for the
new allocation path. This reports the application's eligibility, not a GPU
counter measurement. Actual activation also requires the driver's successful
Switch ZCULL initialization. FPS improvement and visual correctness must be
checked on hardware, especially shadows, moving lights and transitions between
scenes. For timing runs, disable `native_gpu_profile` and `native_trace_output`
and use warning-level logging; enable diagnostics only for a separate capture.

The standalone `src/native/native_zcull_test.cpp` checks full-clear promotion
against a depth/stencil sample model, including partial clears and clipping.

## Resolve synchronization and full clears

The 2026-10-22 test build enables `native_precise_resolve_barriers` and
`native_full_clear_load_op` by default, also with an older configuration that
omits these keys. Both are initialization-only settings: fully restart the game
after changing them. Setting both to `false` restores the preceding behavior
for these two optimizations; it retains the earlier ZCULL and gamma fixes.

Compute resolves and sized texture copies already supply their own producer
and consumer barriers. The precise-barrier path omits the extra broad wait for
transfer before them. Actual transfer copies and MSAA resolves retain their
transfer barriers, and the following draw/present still flushes pending writes.

A guest clear covering the complete active render area now uses attachment
`loadOp = CLEAR` instead of `LOAD` followed by `vkCmdClearAttachments`. Partial
and offset clears retain the previous path. Color values, depth conversion,
stencil, multisampling, stored results and pixels outside the active render
area are preserved. Ordinary draw passes continue to load existing contents.
The standalone `native_attachment_clear_test.cpp` exercises the selection
and sample model, including padded allocations and smaller render areas.

## Tiled rendering and measured performance

Shared resolves remove supported tile-composition copies by writing into one
destination image. They do **not** rewrite the game's commands to draw the
scene once. Disabling host MSAA also does not remove guest tile replay. A
single-pass path requires a verified F.E.A.R.-specific change to target setup
and tile scheduling; the NFSMW game-specific hook cannot be copied verbatim.

The user's 2026-10-22 measurements at GPU **460 MHz** report **30 FPS** for
most of the game, **25–27 FPS** in heavy firefights and about **17 FPS** in
some anomalous areas. Below 30 FPS the GPU reaches **100%** while the CPU has
headroom. These are observations of the preceding test builds, not measurements
of the new clear/barrier changes. See [performance.md](performance.md) for the
detailed NFSMW comparison, remaining questions and hardware test procedure.

The renderer derives from Project8 SW and nfsmw-nx. The complete runtime source
snapshot is in `patches/native-vulkan`; driver sources and patches are described
in [driver-build.md](driver-build.md).
