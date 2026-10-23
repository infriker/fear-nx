# F.E.A.R. NX — deko3d branch

A native Nintendo Switch port of the Xbox 360 version of **F.E.A.R. (2005)**.
ReXGlue statically recompiles the game's PowerPC code to AArch64. This branch
uses **deko3d** for rendering on Horizon and builds **`fear_nx_deko3d.nro`**.

The original menu, cutscenes and campaign are playable. This is an experimental
alternative to the [Vulkan/Mesa renderer in `main`](https://github.com/infriker/fear-nx/tree/main).

## Performance

**Current Switch testing: 20–30 FPS with overclocking, with frequent drops.**
**Vulkan currently provides more stable performance.** The overclock settings
were not recorded with this report, so these numbers are observations rather
than a reproducible benchmark. They are not stock-clock results or a claim
that every level maintains 20 FPS.

The current implementation includes persistent geometry and index caches,
16-bit indices, bounded texture validation, NEON uploads, retained constants,
deferred clears, 3D copies and finer image dependencies. Shader variants can
be cached and prepared on a PC. Sustained slow scenes in the recorded tests
also occurred with zero shader compilations; precompilation alone does not
remove those drops.

See the single [test and optimization history](docs/deko3d-history.md) for
what changed, what helped, and which attempts regressed or remain unproven.

## How to install

Use a deko3d NRO built from this branch or explicitly labelled as deko3d.
The [browser installer](https://infriker.github.io/fear-nx/) and its pinned
release still provide the **Vulkan build**; selecting this Git branch does
not change that download.

For an existing installation, add `fear_nx_deko3d.nro` beside `fear_nx.nro`
and launch the deko3d entry. Both use the same game and save layout. Back up
custom settings before applying this branch's `release/fear-nx.toml`.

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

Gyroscope aiming is **on by default** and works only while aiming. To turn it
off, open the port settings with **L + R + D-pad Right**, select **Input**,
uncheck **input_gyro_enabled**, then choose **Save to config**. Alternatively,
set `gyro_enabled = false` under `[input]` in `fear-nx.toml`. Existing saved
preferences are preserved. See [gyro settings](docs/gyro-aiming.md).

## Settings

Edit `fear-nx.toml` beside the NRO. Backend selection is made at build time;
`renderer = "native"` selects this branch's native deko3d renderer. The release
profile leaves clocks to the system/user and does not force overclocking.
It disables profiling and verbose tracing for normal play. User configurations
remain in effect when replacing only the NRO.

For diagnostics, enable `native_cpu_profile = true` and/or
`native_gpu_profile = true` under `[nx]`, and `level = "info"` under `[log]`.
Profiling affects measurements; use a separate run for clean FPS comparisons.
See [shader tools](tools/deko/README.md) for cache preparation.

## Planned work

- [ ] **Optimize CPU work in the game**, including guest execution, waits and
  synchronization, using measured hot paths.
- [ ] Reduce render-thread queue waits and remaining constant/texture work.
- [ ] Validate performance and image correctness across more levels on Switch.

The existing bounded CPU adapter is retained; it is not a completed optimization
of the game's CPU workload. Current caches and the separate Vulkan path remain.

## Known issues

- Frequent frame-rate drops, especially with complex scenes and combat.
- New shader variants may stall on first use; existing cache entries help only
  the variants already observed or prepared.
- Only the Xbox 360 executable revision below is supported.
- Menu and campaign testing does not establish that every mission is correct.

See [known issues](docs/known-issues.md). Vulkan's unresolved graphical defects
are tracked separately in `main`; their triggers have not yet been identified.

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
