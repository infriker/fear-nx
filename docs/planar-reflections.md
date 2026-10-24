# Dynamic planar reflections

`[nx] planar_reflections = false` disables dynamic water/puddle reflection maps
and plain mirror targets. It defaults to false in the NRO, so existing profiles
do not need replacement. Set it to true and fully restart the game to restore
the original behavior. The setting is applied when level FX objects are created.

This keeps water's separate `tRefractionMap`, ordinary camera render targets,
movie/video textures, shadows, specular lighting and static environment maps.
The latter do not render another view of the scene and may still make a surface
look glossy. Dynamic cube-map camera targets are also outside this change.

## Engine mechanism

The public FEAR SDK distinguishes render-target channels by the material
parameter, not just the Mirror flag. PolyGrid creates both `tReflectionMap` and
`tRefractionMap` with BOTH Mirror and Refraction set. Turning off all mirror-flagged
objects would therefore break water refraction too.

The Xbox hook runs the original `CRenderTargetFX::Init` first. On success, a pure
mirror (`Mirror && !Refraction`) or a mirror target bound to `tReflectionMap` gets
required LOD 3. The game's group LOD is clamped to 0..2. Its normal Update follows
the existing disabled-target branch, restoring the material's default parameter
instead of leaving a frozen render-target texture bound.

Paused simulation can skip SFX Update while still calling UpdateRenderTarget.
A second hook checks the LOD marker before SetupRenderTargetGroup, unbinds an
existing binding through the original method if necessary, and returns success.
This prevents paused creation from bypassing the disable policy. No texture or
geometry hashes, per-draw classification, Vulkan synchronization changes or
additional GPU waits are introduced. Channel-name comparison happens only in
Init; the render-target hook checks one object field.

## Verified Xbox layout

These addresses belong only to the project's already SHA-256-validated XEX.
Hook setup also checks instruction signatures after image loading and falls
back to the original implementation if any signature differs.

| Address / offset | Evidence |
|---|---|
| `0x82310AC0` | Init; references `EnableRenderTargets` at `0x82015588`, then copies the creation structure fields |
| `0x8230FE70` | Constructor installs vtable `0x82015554` |
| `0x8230FFC8` | Update; current LOD below required LOD calls UnbindMaterialParam |
| `0x823E93B0` | GetRenderTargetLOD; clamps the current value to at most 2 |
| `0x8230F988` | UnbindMaterialParam; restores default material parameter and clears bound flag |
| `0x82310860` | UpdateRenderTarget; normally calls SetupRenderTargetGroup before checking bound/dirty flags |
| object `+52`, `+56` | Required LOD, bound-to-material byte |
| object `+80`, `+81`, `+116` | Mirror, Refraction, parameter string |
| string `+4`, `+20`, `+24` | Inline data / heap pointer, length, capacity; checked against string assignment `0x82182060` |

## References and validation limits

- [RenderTargetFX: Init, Update, binding and rendering](https://github.com/xfw5/Fear-SDK-1.08/blob/master/Game/ClientShellDLL/RenderTargetFX.cpp)
- [RenderTargetGroupFX: LOD clamp](https://github.com/xfw5/Fear-SDK-1.08/blob/master/Game/ClientShellDLL/RenderTargetGroupFx.cpp)
- [PolyGrid: separate reflection and refraction channels](https://github.com/xfw5/Fear-SDK-1.08/blob/master/Game/ObjectDLL/PolyGrid.cpp)
- [SFX manager: pause handling](https://github.com/xfw5/Fear-SDK-1.08/blob/master/Game/ClientShellDLL/SFXMgr.cpp)

The SDK source explains behavior; the actual Xbox generated code and loaded
image were checked for the addresses and field layout above. Build, hook symbol
resolution and NRO structure checks do not prove the visual result or FPS on
hardware. A Switch scene with water/mirrors still needs the user's comparison.
