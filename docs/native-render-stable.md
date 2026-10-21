# Windows-validated native renderer — Switch candidate, 2026-10-21

The user confirmed the reported Vulkan rendering defects fixed on Windows.
The same `src/native` sources are built into this Switch candidate: shared
resolve destinations, stencil-clear alias propagation, zero depth/color alias
clears, depth conversion and exact D24 depth bias. The PC branch vendors an
identical copy of this renderer and its shader headers.

The complete current ReXGlue snapshot is in `patches/native-vulkan`. Restore it
with `scripts/bootstrap-native-sdk.ps1`, instead of the older numbered series.
The SDK base remains ReXGlue 0.10.0. The existing Horizon Mesa/NVK driver is used;
its source advertises depthBiasControl and depthBiasExact. Runtime negotiation
and actual rendering on Switch still need hardware confirmation.

Build: `pwsh -File scripts/build-switch.ps1 -Jobs 4`.
Profile: `release/fear-nx-render-stable.toml` (installed as `fear-nx.toml`).
The profile keeps shared resolves, D24, MSAA off, 2x anisotropy, async pipelines
and the same non-threaded recording path used in the tested Windows config.
Existing Switch clocks, input and audio settings are retained.

Copy both `fear_nx.nro` and `fear-nx.toml` from the candidate package into
`sd:/switch/fear-nx/`, preserving game data and saves. Keep the prior NRO/config
for rollback. Use the same full-memory homebrew launch method as before.

Check menu/campaign startup, the swinging-light scene, light rays behind walls,
weapon/hands under light and a level transition. The log should report
`D24S8 depth bias: exact D24 UNORM quantum (VK_EXT_depth_bias_control)` and
`Resolve resource mode: shared-direct-v1`.

The branch is local until the user tests this NRO and approves publication.
Compilation, CPU regression tests and NRO header checks do not establish
correct rendering or stable gameplay on the console. No game/emulator launch
is part of the packaging step.

Validation on 2026-10-21: all four existing CPU regression tests passed
(`native_resolve_layout`, `native_shared_resolve`, `native_stencil_clear`,
`native_depth_color_clear`). The SDK snapshot was restored in a temporary index
and its tree matched the working sources; the bootstrap verification passed.
All 22 renderer files matched the PC branch byte for byte. The PC executable
was rebuilt from its bundled sources without launching the game.

Full Switch build completed successfully. NRO0 image bounds and the AArch64
ELF were checked. Candidate NRO size: 79071429 bytes; SHA-256:
`fa92a8a9810837d6255a94f2d78194df42a107d3bba55e4ce13bedcaed662888`. The package includes this NRO and the matching
`fear-nx.toml`. Switch hardware validation remains pending.
