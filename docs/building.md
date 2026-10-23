# Building F.E.A.R. NX

Build from your own supported Xbox 360 `default.xex`. Original game data and
generated translations stay outside Git. The browser installer currently supplies Vulkan; this branch builds deko3d.

## Requirements

- Windows with Git and Python 3 available on `PATH`.
- Docker Desktop with its Linux engine running.
- ReXGlue Windows CLI **0.10.0** for code generation. Add its directory to
  `PATH`, or replace `rexglue` below with the full executable path.
- Your extracted Xbox 360 game, including the supported `default.xex`.
- Disk space for generated C++, dependencies and ARM64 build output.

Run the commands below from the repository root. Stop if a command fails.
The Docker examples mount the current checkout at `/project`; no second
checkout or personal development directory is required.

## 1. Prepare the SDK

Use a new SDK checkout for these commands. Keep an existing modified SDK
separately rather than checking out another revision over your changes.

```text
python -c "import hashlib,json; from pathlib import Path; p=Path('patches/native-vulkan'); m=json.loads((p/'manifest.json').read_text()); assert hashlib.sha256((p/m['patch']).read_bytes()).hexdigest()==m['sha256'], 'SDK patch checksum mismatch'"
git clone --no-checkout https://github.com/rexglue/rexglue-sdk.git third_party/rexglue-sdk
git -C third_party/rexglue-sdk checkout --detach c94f5ebdcb3c9d1a460ca48e04f9758448f8d518
git -C third_party/rexglue-sdk apply --check ../../patches/native-vulkan/rexglue-sdk.patch
git -C third_party/rexglue-sdk apply --index ../../patches/native-vulkan/rexglue-sdk.patch
git -C third_party/rexglue-sdk submodule update --init --recursive
git -C third_party/rexglue-sdk write-tree
git -C third_party/rexglue-sdk submodule status --recursive
```

`write-tree` must print `c7fb3c0f1a588b268115bf0dceb24f0c26eb1ae0`.
Compare the submodule paths and commit IDs with `recursive_submodules` in
`patches/native-vulkan/manifest.json`. A `-`, `+` or `U` status prefix indicates
an uninitialized, mismatched or conflicted submodule; resolve it before building.

This applies the complete source snapshot in `patches/native-vulkan` to the
pinned ReXGlue base. The numbered patches are retained as source history. Do not apply them on top of this snapshot.

Then apply the focused gamma-presentation overlay (after verifying the snapshot
tree above):

```text
python -c "import hashlib,json; from pathlib import Path; p=Path('patches/native-vulkan'); m=json.loads((p/'gamma-present.json').read_text()); assert hashlib.sha256((p/m['patch']).read_bytes()).hexdigest()==m['sha256'], 'Gamma overlay checksum mismatch'"
git -C third_party/rexglue-sdk apply --check ../../patches/native-vulkan/gamma-present.patch
git -C third_party/rexglue-sdk apply --index ../../patches/native-vulkan/gamma-present.patch
```

This retains the pinned SDK and supplies the raw-frame snapshot API used by the
native renderer. For 8-bit RGBA/BGRA frontbuffers, gamma is applied per texel in
the final bilinear shader. A transfer copy still preserves the mailbox frame;
the separate gamma compute dispatch is removed. Other formats use the original
compute path. FidelityFX builds keep the original path as well.

`native_fused_gamma = true` in `[nx]` is the default. Set it to `false` and fully
restart for an A/B comparison. Keep `native_shared_resolves = true` and
`native_record_thread = false` fixed during this comparison. At 1:1 output the
new shader uses one source tap; resizing requires four gamma-corrected taps to
preserve gamma-before-filter ordering, so the FPS benefit needs device testing.

The Switch build now defaults to `FEAR_NX_RENDERER=DEKO3D`. During CMake
configuration, `tools/switch/prepare_deko3d.py` adds the pinned Deko3D provider,
UAM and SPIRV-Cross sources to the ignored SDK copy and applies patches 0023
through 0033. It preserves the existing runtime source and patches. No Vulkan
driver build is required for this backend. Set `FEAR_NX_RENDERER=VULKAN` to
build the existing Vulkan renderer instead.

The GLSL source is included in the overlay. To regenerate its two embedded
headers inside the Switch toolchain container, run from `/project`:

```text
glslangValidator -S frag -V --target-env vulkan1.1 --vn guest_output_bilinear_gamma_ps -o third_party/rexglue-sdk/src/ui/shaders/vulkan_spirv/guest_output_bilinear_gamma_ps.h third_party/rexglue-sdk/src/ui/shaders/guest_output_bilinear_gamma.frag.glsl
glslangValidator -S frag -V --target-env vulkan1.1 -DGAMMA_DITHER --vn guest_output_bilinear_gamma_dither_ps -o third_party/rexglue-sdk/src/ui/shaders/vulkan_spirv/guest_output_bilinear_gamma_dither_ps.h third_party/rexglue-sdk/src/ui/shaders/guest_output_bilinear_gamma.frag.glsl
```

## 2. Generate the game translation

Create the `assets/` directory and copy your supported Xbox 360 executable to
`assets/default.xex`. Validate it before generation:

```text
rexglue --version
python -c "import hashlib; from pathlib import Path; h=hashlib.sha256(Path('assets/default.xex').read_bytes()).hexdigest(); assert h=='e7fccdf6dd57bb58d10efbdd0b2ca567b8cece5b6a3fc696c7fca879aaea2792', 'Unsupported default.xex: '+h; print(h)"
```

The CLI version must be **0.10.0**. Generate using a local manifest copy because
ReXGlue writes SDK metadata into the manifest it receives:

```text
python -c "from pathlib import Path; Path('fear_xbox360_manifest.local.toml').write_bytes(Path('fear_xbox360_manifest.toml').read_bytes())"
rexglue codegen fear_xbox360_manifest.local.toml
```

Check that `generated/default/sources.cmake` exists. Code generation must finish
before CMake configuration. It does not copy the `Game/` data directory.

## 3. Prepare the toolchain

```text
docker build -t fear-nx-switch:devkitpro-20260219-clang19 -f tools/switch/Dockerfile tools/switch
```

The Deko3D build does not need the Vulkan driver. The Vulkan build still uses
the steps in [driver-build.md](driver-build.md).

## 4. Compile and package the NRO

```text
$ProjectRoot = (Get-Location).Path
docker run --rm --mount "type=bind,source=${ProjectRoot},target=/project" -w /project fear-nx-switch:devkitpro-20260219-clang19 cmake -S /project -B /project/out/switch-deko3d -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=/project/tools/switch/toolchain.cmake -DREXSDK_DIR=/project/third_party/rexglue-sdk -DREXGLUE_SWITCH_COMPILE_ONLY=OFF -DFEAR_GENERATED_DIR=/project/generated/default -DFEAR_ASSET_DIR=/project/assets -DFEAR_NX_RENDERER=DEKO3D
docker run --rm --mount "type=bind,source=${ProjectRoot},target=/project" -w /project fear-nx-switch:devkitpro-20260219-clang19 cmake --build /project/out/switch-deko3d --parallel 4 --target fear_nx
```

For the existing Vulkan path, configure a separate directory with
`-DFEAR_NX_RENDERER=VULKAN -DREXGLUE_SWITCH_NVK_SDK=/project/sdk/nvk`.
Generated sources and game assets are read from the ignored local
`generated/default` and `assets` directories prepared above. The Deko3D build writes an AArch64 ELF and
`fear_nx_deko3d.nro` under `out/switch-deko3d/`; the Vulkan build keeps its
`fear_nx.nro` name. The browser installer remains pinned to the separate Vulkan release. For configuration only, stop after the first command.

For a console GPU fault, add `-DREXGLUE_SWITCH_DEKO3D_DEBUG=ON` to the
Deko3D configure command. This retains Release game code but links
`libdeko3dd` at both SDK link sites, enabling API validation and GPU fault
details. The result is `fear_nx_deko3d_diag.nro`, titled **F.E.A.R. NX deko3d
diagnostic**, so packaging does not overwrite the regular NRO. Its startup
marker and deko3d warnings/errors are flushed to `rex_stderr.log`, including
warnings past the normal app log's rate limit. Set the option back to `OFF`
and rebuild for the normal output; diagnostic builds are not performance
baselines.

Copy the NRO and `release/fear-nx.toml` into `sdmc:/switch/fear-nx/`, then place
your executable and extracted `Game/` under its `game/` subdirectory.
See [installation](installing.md) for the full layout.

## Packaging a branch build

Copy `out/switch-deko3d/fear_nx_deko3d.nro` and `release/fear-nx.toml`
to the installation folder described above. Preserve the separate NRO name.
The inherited `scripts/package-release.py`, installer manifest and Pages
workflow target the Vulkan release in `main`; do not use them to silently
replace that release with this experimental backend. Publishing a separate
binary release requires explicit deko3d naming and its matching notices.

The CPU adapter is generated into the build directory by
`tools/switch/prepare_cpu_adapter.py`; original generated inputs are retained.
Optional host regression checks and cache preparation live in `tools/deko/`.
