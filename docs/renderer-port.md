# Native Xenos/Vulkan renderer port

## What this renderer does

The native path is an app-owned Vulkan backend for the Xbox 360 GPU command stream. It consumes the guest command ring and register state, builds draw state, translates guest vertex and pixel shaders to SPIR-V at runtime, and submits Vulkan work. It also handles Xenos texture layout conversion, render-target copies/resolves, depth conversion, and the small compute shaders used for those operations.

This is a graphics backend only. ReXGlue remains responsible for recompiling/running the Xbox 360 CPU code and providing guest memory and system services. The renderer does not recreate FEAR's gameplay, replace the original game code, or include game assets. The existing ReXGlue Xenos renderer remains selectable as a fallback on desktop.

The code avoids EDRAM, fragment-shader interlock, and tessellation from the original Xbox 360 Vulkan backend. It uses regular Vulkan images and resolve paths suitable for the target API. Runtime guest shader translation is still required; this is not a static shader conversion.

## Configuration

These cvars are declared by the FEAR native renderer under the `F.E.A.R. NX` category:

| Cvar | Default | Purpose |
| --- | --- | --- |
| `nx_renderer` | `native` on Switch; `xenos` on desktop | Selects the app-owned Vulkan renderer or ReXGlue's emulated Xenos renderer. Init-only. |
| `nx_native_msaa` | `true` | Honors the guest's multisample count; disabling it forces single-sample targets. |
| `nx_native_async_pipelines` | `true` | Compiles new Vulkan pipelines on a worker and skips draws until they are ready. |
| `nx_native_record_thread` | `false` | Records Vulkan draw calls on a second thread. |
| `nx_native_depth24` | `false` | Uses 32-bit storage for guest 24-bit depth rather than float depth with stencil. |
| `nx_native_gpu_profile` | `false` | Logs GPU time by render pass and resolve for sampled frames. |
| `nx_native_max_anisotropy` | `16` | Caps anisotropic filtering; `1` disables it. |
| `nx_pace_before_present` | `false` | Applies the frame cap before presentation for more even frame spacing. |
| `nx_min_frame_vblanks` | `1` | Minimum 60 Hz vblanks between presents; `2` targets an even 30 fps cadence. |

The renderer also reads ReXGlue's `gpu_specialize_texture_signs` and `query_occlusion_fake_sample_count` cvars. These remain SDK-owned settings.

## Provenance and licensing

The generic renderer structure and Xenos handling were adapted from Project8Recomp's `src/game/src/native/` at commit `3393f2615deb7e8ac11ce51d2cd0e9defded5669`. The port removes Project8-specific game hooks, shader overrides, and pacing integration. Portions of Vulkan state and shader handling also follow ReXGlue/Xenia source, with their existing license notices retained in source files. Preserve the applicable GPL-3.0 and BSD-3-Clause notices when redistributing; include the required license and attribution files in the repository package.

## Verification status

The ARM64 native renderer links and packages as an NRO. Test 17's partial SUST
change removed the two observed compute-pipeline errors in Eden. The user
confirmed original-menu rendering and navigation on Eden and Switch Lite.

Two earlier physical campaign loads froze on guest memory mapping failures.
SDK patch 0022 and NRO B3CCF37C6D0007E443B147F3B64D72975F907993161032A39327F456F37DCD44
subsequently reached Chapter 01 gameplay on the physical console. The third
run passes the old failure point without recorded memory-commit errors.
Full-campaign stability and acceptable performance remain unproven.

The native resolve cache still loses a right-hand tiled segment in the
hardware-tested build. There is also an unsupported 2x MSAA depth resolve.
The user reports a cropped image and lighting confined to an edge.

The existing single-sample option can be tested without rebuilding:
set native_msaa=false in the existing [nx] section of fear-nx.toml and restart.
release/fear-nx-no-msaa.toml is the diagnostic profile with only that change.
It changes neither resolution nor console clocks. Treat it as an opt-in A/B
test until performance and effect correctness have been checked on hardware.

See hardware-diagnostics.md and status.md for current evidence and limits.

### Tiled color-frame candidate

The current source composes validated RGBA8 resolve segments using their
32x32 Xenos tile offsets. It retains segment coordinates and routes both
texture lookups and Swap to the composed image. Eden run 014 confirms full-size selection, but the user still sees black
pixels on the right. The first candidate is only a partial improvement;
pixel correctness has not been established. Source keeps legacy behavior for unsupported
layouts, including depth assembly. See status.md for the exact NRO hash.
