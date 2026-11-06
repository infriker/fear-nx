# Native Vulkan renderer

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
| `planar_reflections` | `false` | Reserved for compatibility. Dynamic water and mirror reflections are disabled in v0.2.0 |
| `client_fx_detail_level` | `1` | Use the game's medium effect detail; `2` restores original detail |
| `particle_bounce_scale` | `0.5` | Scale eligible particle bounce/splat probabilities; `1.0` restores original behavior |
| `volumetric_light_slices` | `128` | Reduce light-volume slices from 256 while keeping the effect |
| `volumetric_light_shadow_size` | `64` | Reduce volumetric shadow maps from 128x128 to 64x64 |
| `native_texture_min_mip` | `1` | Use mip 1 or coarser on eligible large tiled BC1-BC5 textures |
| `native_shared_resolves` | `true` | Share validated resolve destinations |
| `native_depth24` | `true` | D24 storage for compatible guest depth surfaces |
| `native_msaa` | `false` | Full-frame targets always use one sample |
| `native_max_anisotropy` | `2` | Limit anisotropic filtering to 2× |
| `native_async_pipelines` | `true` | Compile pipelines asynchronously |
| `native_record_thread` | `false` | Keep command recording on the current path |
| `native_fused_gamma` | `true` | Apply gamma in the final output shader for supported frontbuffers |
| `native_zcull` | `true` | Allocate depth attachments eligible for persistent NVK ZCULL |
| `native_precise_resolve_barriers` | `true` | Omit a redundant transfer wait on compute resolve/crop paths |
| `native_full_clear_load_op` | `true` | Clear a fully covered render area at attachment load |
| `native_gpu_profile` | `false` | Reserved for compatibility. The release omits per-pass timers |
| `native_trace_output` | `false` | Reserved for compatibility. The release omits native render tracing |

The weapon display remains enabled. The mip setting excludes linear video
planes, render targets and textures without the required mip chain. Set the
`volumetric_light_slices`, `volumetric_light_shadow_size` and
`native_texture_min_mip` values to `0` to restore the game-selected levels. Restart after changing them. See [reflection handling](planar-reflections.md).

The FX controls are initialization-only: fully restart after changing them.
`client_fx_detail_level` accepts `0` (low), `1` (medium) and `2` (original).
`particle_bounce_scale` accepts `0.0` to `1.0`; it changes eligible particle
bounce and splat probabilities, not all physics or the total particle count.
The defaults also apply when these keys are absent from an older TOML.
Bloom retains the original game behavior; no bloom-disabling override is included.

The release omits the PGO recorder and native renderer timers. Routine logging
is disabled. The supplied `[switch] gpu_mhz = 0` leaves GPU clocks to the system.

## Vertex/index freshness

Version 0.1.3 enables these initialization-only settings under `[fear]`:

```toml
[fear]
vertex_fresh = true
vertex_spans = true
```

The game can rewrite vertex or index memory after its first use in a frame.
Reusing a once-per-frame upload then supplies old geometry. The renderer now
checks the current draw's active data and reuses uploads only when their exact
content is unchanged and their ring-buffer allocation is still valid.

For vertex buffers larger than 4096 bytes, shader analysis can restrict checks
and uploads to the span reachable by the draw. Unknown control flow, modified
fetch indices, wrapped indices or an out-of-bounds span keep full active-buffer
checks. Textures are not included in these checks. No new explicit GPU
synchronization path is added; its effect on existing upload-ring waits and
FPS has not been measured. The user confirmed the weapon-artifact fix with the
preceding 0.1.2-vf1 build on 2026-10-25; there is no controlled FPS comparison.

The implementation is adapted from
[fear360-recomp fc572279](https://github.com/Juansolo457/fear360-recomp/commit/fc5722793f71217ca888c329d654bba6f9eeeedc),
retaining this renderer's upload lifetime and flush safeguards. Both settings
also default to `true` when absent from an older TOML. For troubleshooting only,
setting `vertex_fresh = false` and restarting restores the previous cache path;
setting only `vertex_spans = false` retains freshness checks over full buffers.

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
scenes.

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

## Full-frame rendering in v0.2.0

The guest sets up full-height color and depth surfaces for the supported
1280×720 scene and submits a single full-height rectangle to the XDK. The
frontbuffer count, clears, resolves, and swap sequence are preserved. Other
render-target sizes follow the original game path. This also fixes the
half-screen image during level loading.

Shared resolves still reuse eligible destination images. Host attachments use
one sample. See [Switch performance](performance.md#switch-performance-in-v020)
for the CPU, GPU and RAM clocks used in testing.

The renderer derives from Project8 SW and nfsmw-nx. The complete runtime source
snapshot is in `patches/native-vulkan`; driver sources and patches are described
in [driver-build.md](driver-build.md).
