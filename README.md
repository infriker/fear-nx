# F.E.A.R. NX

Experimental Nintendo Switch / Horizon port of the Xbox 360 version of
F.E.A.R. (2005), using static PowerPC recompilation and a native Xenos-to-Vulkan
renderer. The original menu and campaign run on a physical Switch; renderer
correctness and performance work remain unfinished.

This is a separate project from the working Windows x64 recompilation. It uses
the **same ReXGlue 0.10 source base** (`c94f5ebdcb3c9d1a460ca48e04f9758448f8d518`),
plus the Project8 Switch platform patch series and small documented build fixes.
There is no SDK downgrade to 0.8.

## Download the saved Switch build

[Private checkpoint release](https://github.com/infriker/fear-nx/releases/tag/checkpoint-20261020)
contains the last existing hardware NRO, its configuration, and the original ZIP.
It is the pass-sync build, not the separate Eden 2 GiB diagnostic variant.

Copy `fear_nx.nro` and `fear-nx.toml` to `sdmc:/switch/fear-nx/`, retaining your
existing game data, saves and caches. Launch through the full-memory homebrew
method used for the previous working build. MSAA is disabled; the profile does
not request an overclock. The checkpoint was not rebuilt.

See [the exact binary manifest](release/checkpoint-20261020.json),
[its configuration](release/fear-nx-switch-latest.toml), and
[complete dependency snapshots](patches/checkpoint-20261020/README.md).
Game files, generated source and local diagnostic logs remain excluded from Git.

## Architecture

- ReXGlue translates the original PowerPC functions to C++; devkitA64 compiles
  them to native AArch64 code. `src/fear_nx_pch.h` provides GCC-compatible weak
  aliases while preserving the validated 0.10 translation.
- The Horizon runtime supplies guest memory, threading, filesystem, audio, and
  Xbox kernel interfaces through libnx.
- The app-owned renderer consumes the original Xenos command ring, translates
  shaders to SPIR-V, and uses Vulkan through Mesa/NVK for Switch.
- libnx controller state becomes the Xbox input state expected by the original
  game. The default mapping follows Nintendo button letters; a configuration
  option selects Xbox physical positions.
- Game files, saves, caches, and configuration are external to the NRO.

See [building instructions](docs/building.md),
[renderer notes](docs/renderer-port.md), [runtime notes](docs/runtime-port.md),
and [driver build](docs/driver-build.md).

## Local game data

Supply an extracted copy of the supported Xbox 360 game. The expected
`default.xex` SHA-256 is:

```
e7fccdf6dd57bb58d10efbdd0b2ca567b8cece5b6a3fc696c7fca879aaea2792
```

Game assets, generated translations, SDK checkouts, build outputs, and emulator
profiles are ignored by Git. This repository contains no game executable or
game data.

## Current status — paused snapshot, 2026-10-20

The latest packaged build tested on physical Switch is the local archive
`dist/fear-nx-switch-pass-sync-20261020.zip` (NRO SHA-256
`C1C96F8D54278AA4C29D926C8D469FB8F735960A9FE892C427160256A5899585`). The user
confirmed fullscreen output, the original menu, and campaign gameplay. Lighting
and occlusion artifacts remain. The user noticed a slight performance improvement,
but no matched benchmark was run. Eden still shows intermittent pauses of about
three seconds, and Windows Mesa has not been built or tested. See
[validation status](docs/status.md) for the test boundaries and earlier results.
## Attribution

The native renderer is adapted from
[Project8 SW](https://github.com/R-drg/project8-sw), which provides an existing
Switch port on ReXGlue 0.10. The
[Simpsons NX](https://github.com/R-drg/the-simpsons-game-nx) port was the initial
architectural reference. See `NOTICE` and `LICENSES/` for the individual source
licenses. The project wrapper and adapted renderer are GPL-3.0-or-later;
upstream components retain their own licenses.
