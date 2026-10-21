# F.E.A.R. NX — Nintendo Switch port

A native Nintendo Switch port of the Xbox 360 version of **F.E.A.R. (2005)**.
The game's PowerPC code is statically recompiled to AArch64 with
[ReXGlue](https://github.com/rexglue/rexglue-sdk). A native Vulkan renderer draws
the game through Mesa/NVK on Horizon.

The original menu and campaign are playable. At **614 MHz GPU**, gameplay is
generally around **30 FPS**, with drops to **25–27 FPS** in demanding scenes,
especially during slow motion. Performance work is ongoing.

## How to install

1. Open the **[F.E.A.R. NX installer](https://infriker.github.io/fear-nx/)**.
2. Select your Xbox 360 disc image (`.iso`) or extracted game folder containing
   `default.xex` and `Game/`.
3. Choose **Create fear-nx.zip** and extract it into `sdmc:/switch/`.
4. Launch `fear_nx.nro` through the Homebrew Menu in **title takeover mode**:
   hold **R** while launching an installed game. Album/applet mode is unsupported.

The installer checks your executable, downloads the port and packages everything
locally in your browser. Game files are never uploaded. You need your own copy
of the Xbox 360 game; the Windows PC release is not compatible.

```text
/switch/fear-nx/
  fear_nx.nro
  fear-nx.toml
  LICENSES.txt
  game/
    default.xex
    Game/
```

The port creates `userdata/`, `cache/`, `update/` and `metadata/` beside the NRO.
Keep `userdata/` when updating. **Create fear-nx-update.zip** produces only the
NRO, configuration and notices; it requires your supported `default.xex` but
does not copy the game again. Back up custom settings before replacing the TOML.

For manual installation, use the files in the [latest release](https://github.com/infriker/fear-nx/releases/latest).
See [installation and troubleshooting](docs/installing.md).

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
Gyroscope aiming is planned and is not available in this release.

## Settings and performance

Edit `fear-nx.toml` next to the NRO. The release uses the native renderer,
shared resolve resources, D24 depth, 2× anisotropic filtering and disabled MSAA.
The supplied profile leaves GPU clock selection to the system or your clock tool;
it does not automatically set 614 MHz.

Expect approximately 30 FPS at a 614 MHz GPU clock, with 25–27 FPS in heavier
scenes and slow motion. These figures are not a guarantee for every level or
hardware configuration. A comprehensive full-campaign benchmark is not available.
The first encounter with a shader can cause temporary missing effects or a hitch
while its pipeline compiles; compiled pipelines are cached on the SD card.

## Planned features

- [ ] Gyroscope aiming.
- [ ] Performance improvements, focusing on demanding scenes and slow motion.

## Known issues

- Heavy scenes and slow motion can drop below the 30 FPS target.
- Asynchronous pipeline compilation can briefly omit newly encountered draws.
- Only the executable revision listed below is supported.
- Applet mode and emulators are not supported release targets.

See [known issues and reporting](docs/known-issues.md).

## Build and documentation

Building requires your own game executable, ReXGlue 0.10.0, devkitA64/libnx,
CMake, Ninja and the patched Horizon Mesa/NVK driver. Docker provides the
cross-compilation environment.

| Document | Contents |
|---|---|
| [Building](docs/building.md) | SDK setup, code generation and NRO compilation |
| [Driver build](docs/driver-build.md) | Mesa/NVK archives for Horizon |
| [Native renderer](docs/native-renderer.md) | Renderer design and settings |
| [Relinking](docs/RELINKING.md) | Rebuilding with modified runtime libraries |
| [Installer](installer/README.md) | Browser installer and GitHub Pages deployment |

Supported `default.xex` SHA-256 (Xbox 360 Title ID `565507D9`):

```text
e7fccdf6dd57bb58d10efbdd0b2ca567b8cece5b6a3fc696c7fca879aaea2792
```

The installer and application check this hash. Other revisions are rejected.

## Credits

- The original F.E.A.R. development and publishing teams.
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
