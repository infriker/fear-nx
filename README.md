# F.E.A.R. NX — deko3d branch

A native Nintendo Switch port of the Xbox 360 version of **F.E.A.R. (2005)**.
ReXGlue statically recompiles the game's PowerPC code to AArch64. This branch
uses **deko3d** for rendering on Horizon and builds **`fear_nx_deko3d.nro`**.

The original menu, cutscenes and campaign are playable. This is an experimental
alternative to the [Vulkan/Mesa renderer in `main`](https://github.com/infriker/fear-nx/tree/main).

[Download fear_nx_deko3d.nro](https://raw.githubusercontent.com/infriker/fear-nx/refs/heads/deko3d/builds/deko3d/fear_nx_deko3d.nro) · [Default settings](release/fear-nx.toml)

## Rendering and performance

The game submits the 1280×720 scene as a single full-height region. Color and
depth cover the same rectangle, including during level loading. Scene clears,
resolves and presentation still run. The deko3d renderer uses single-sample
targets and skips dynamic mirror and water reflections. Water refraction and
camera feeds keep their own passes.

The defaults use medium effects detail, half the original particle bounce/splat
probability, 128 volumetric light slices and 64-pixel volumetric shadow maps.
Large mipmapped block-compressed textures start at mip 1, with anisotropy
limited to 2×. The translated game code uses direct calls, LTO and a gameplay
profile recorded on Switch. The downloadable NRO has no runtime PGO recorder
or renderer performance counters.

The October deko3d build reached 20–30 FPS with overclocking and frequent
drops. A comparable Switch measurement for this full-frame build is pending.
See the [deko3d test history](docs/deko3d-history.md).

## How to install

Place `fear_nx_deko3d.nro` beside `fear_nx.nro` in `sdmc:/switch/fear-nx/`
and select **F.E.A.R. NX deko3d Full Frame** in Homebrew Menu. Both executables
use the same game files and saves. You can keep your existing
`fear-nx.toml` or merge settings from the [branch configuration](release/fear-nx.toml).
The [browser installer](https://infriker.github.io/fear-nx/) supplies Vulkan.

```text
/switch/fear-nx/
  fear_nx_deko3d.nro
  fear-nx.toml
  game/
    default.xex
    Game/
  userdata/              # preserve saves when updating
```

You need your own supported Xbox 360 game; the PC release is incompatible.
Copy the complete extracted `Game/` directory, including `uberCatalog.cat`.
Launch via Homebrew Menu in **title takeover mode**: hold **R** while opening
an installed game. Album/applet mode is unsupported.

## Controls

By default, Switch face buttons follow their printed letters. Xbox 360 prompts
therefore refer to the same letters on your controller.

| Switch | Xbox 360 input |
|---|---|
| A / B / X / Y | A / B / X / Y |
| ZR / ZL | RT / LT |
| R / L | RB / LB |
| + / − | Start / Back |
| Left / right stick | Left / right stick |
| Stick clicks | LS / RS |
| D-pad | D-pad |

To use Xbox button positions, set `xbox_layout = true` under `[input]` in
`fear-nx.toml`: Switch B becomes Xbox A, A becomes B, Y becomes X and X becomes Y.

Gyro works during normal play and hip fire by default. The default sensitivity
is 1.5. Turning a handheld left or right steers horizontally even when held
upright. Set `gyro_always_on = false` for aim-only mode or
`gyro_enabled = false` to turn motion off. Both horizontal and vertical
inversion are available under `[input]`. See [gyro settings](docs/gyro-aiming.md)
and [button remapping](docs/button-bindings.md).

## Settings

Edit `fear-nx.toml` beside the NRO. Backend selection is made at build time;
`renderer = "native"` selects this branch's native deko3d renderer. The release
profile leaves clocks to the system/user and does not force overclocking.
It disables profiling and verbose tracing for normal play. User configurations
remain in effect when replacing only the NRO.

Build the diagnostic NRO to enable renderer profiling or GPU validation.
See [build options](docs/building.md) and [shader cache tools](tools/deko/README.md).

## Known issues

First use of a new shader variant can stall. Campaign coverage and hardware
performance measurements are documented in [known issues](docs/known-issues.md).

## Build and documentation

Use ReXGlue 0.10.0, the pinned SDK patches, devkitA64/libnx/deko3d, CMake,
Ninja and the repository's Docker toolchain. No Mesa/NVK driver is required
for the default deko3d build. Original assets and generated translations stay
local. Build tools and source-level regression checks are included; emulator
launchers, captures, personal scripts and binary development bundles are not.

- [Building](docs/building.md) — dependency setup, code generation and NRO.
- [Test history](docs/deko3d-history.md) — changes, measurements and limitations.
- [Shader cache tools](tools/deko/README.md) — optional PC precompilation.
- [Relinking](docs/RELINKING.md) — rebuilding with modified libraries.

Supported `default.xex` SHA-256, Xbox 360 Title ID `565507D9`:

```text
e7fccdf6dd57bb58d10efbdd0b2ca567b8cece5b6a3fc696c7fca879aaea2792
```

## Credits

- The original F.E.A.R. development and publishing teams.
- [nx-mod / rexglue-sdk-nx](https://github.com/nx-mod/rexglue-sdk-nx) — deko3d
  integration and renderer references.
- [VasilkovND / Dante's Inferno NX](https://github.com/VasilkovND/dantes-inferno-nx)
  — geometry, caching, image-operation and CPU-adapter references.
- [deko3d](https://github.com/devkitPro/deko3d), UAM, SPIRV-Cross and their contributors.
- [R-drg / Rodrigo Ferreira](https://github.com/R-drg) —
  [Project8 SW](https://github.com/R-drg/project8-sw), the Switch runtime and
  renderer foundation, and the browser installer this project adapts.
- [StevensND](https://github.com/StevensND) —
  [nfsmw-nx](https://github.com/StevensND/nfsmw-nx) and its installer, the
  underlying renderer and browser packaging design.
- [Tom Clay / ReXGlue](https://github.com/rexglue/rexglue-sdk) and the
  [Xenia](https://github.com/xenia-project/xenia) contributors.
- [danfromtico / mesa-switch](https://github.com/danfromtico/mesa-switch),
  NaGaa95 and the Mesa/NVK/NAK contributors.
- [devkitPro](https://devkitpro.org/) and [switchbrew/libnx](https://github.com/switchbrew/libnx).
- [Wemino / EchoPatch](https://github.com/Wemino/EchoPatch) — the reference
  for aim-only gyro camera integration.
- [The Simpsons NX](https://github.com/R-drg/the-simpsons-game-nx) — an early
  reference for the Switch port structure.

## License

This is an unofficial fan project. F.E.A.R. and its marks belong to their
respective owners. Original game data, `default.xex`, disc images and generated
game source are not included in this repository. The downloadable NRO contains
the recompiled program and requires the supported game files supplied locally.

Project and adapted renderer sources are GPL-3.0-or-later; upstream components
retain their notices and licenses. See [LICENSE](LICENSE), [NOTICE](NOTICE),
[LICENSES](LICENSES/) and the [installer notices](installer/NOTICES.md).
