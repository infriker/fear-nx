# Native Vulkan renderer

The renderer consumes the Xbox 360 Xenos command ring produced by the game and
records Vulkan commands. Game shaders are translated to SPIR-V by ReXGlue;
Mesa/NVK executes them on the Switch GPU. The source is in `src/native/`.

Render targets use host images. Shared resolve destinations preserve the
relationship between tiled segments of a guest texture. Compatible views of
EDRAM propagate stencil clears and zero depth/color clears, preventing stale
shadow masks and light-ray occlusion data. D24 depth bias uses
`VK_EXT_depth_bias_control` when the device exposes exact bias support.

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
| `native_gpu_profile` | `true` | Collect GPU pass diagnostics |
| `native_trace_output` | `true` | Record bounded renderer diagnostics |

The release retains the settings used for the current build. Clock selection
is separate: `[switch] gpu_mhz = 0` leaves the system profile unchanged.
`[input] xbox_layout` changes face-button mapping, not rendering.

The renderer derives from Project8 SW and nfsmw-nx. The complete runtime source
snapshot is in `patches/native-vulkan`; driver sources and patches are described
in [driver-build.md](driver-build.md).
