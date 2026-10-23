# Building F.E.A.R. NX

Build from your own supported Xbox 360 `default.xex`. Original game data and
generated translations stay outside Git. The release installer is the simpler
route if you only want to play.

## Requirements

- Windows with Git and Python 3 available on `PATH`.
- Docker Desktop with its Linux engine running.
- ReXGlue Windows CLI **0.10.0** for code generation. Add its directory to
  `PATH`, or replace `rexglue` below with the full executable path.
- Your extracted Xbox 360 game, including the supported `default.xex`.
- Disk space for generated C++, dependencies and ARM64 build output.

Run the commands below from the repository root. Stop if a command fails.
The Docker examples use `D:/fear-nx` as the absolute host checkout path;
replace it with your own path. Paths inside the container remain `/project`.

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
pinned ReXGlue base. The numbered patches are retained as source history. Do not apply them
on top of this snapshot.

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

## 3. Prepare the toolchain and driver

```text
docker build -t fear-nx-switch:devkitpro-20260219-clang19 -f tools/switch/Dockerfile tools/switch
```

Build the driver using [driver-build.md](driver-build.md). The full build needs
`sdk/nvk/lib/libvulkan.a` and `sdk/nvk/lib/libnak_rs.a`.

## 4. Compile and package the NRO

```text
docker run --rm --mount "type=bind,source=D:/fear-nx,target=/project" -w /project fear-nx-switch:devkitpro-20260219-clang19 cmake -S . -B out/switch -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=/project/tools/switch/toolchain.cmake -DREXSDK_DIR=/project/third_party/rexglue-sdk -DREXGLUE_SWITCH_NVK_SDK=/project/sdk/nvk -DREXGLUE_SWITCH_COMPILE_ONLY=OFF
docker run --rm --mount "type=bind,source=D:/fear-nx,target=/project" -w /project fear-nx-switch:devkitpro-20260219-clang19 cmake --build out/switch --parallel 4 --target fear_nx
```

Outputs include `out/switch/fear_nx` (AArch64 ELF), `fear_nx.nacp` and
`fear_nx.nro`. For configuration only, stop after the first command.

Copy the NRO and `release/fear-nx.toml` into `sdmc:/switch/fear-nx/`, then place
your executable and extracted `Game/` under its `game/` subdirectory.
See [installation](installing.md) for the full layout.

## Publishing a build

`scripts/package-release.py` packages an existing NRO with the configuration
and notices. It never reads or packages the original game files.

```text
python scripts/package-release.py --nro out/switch/fear_nx.nro --output dist/public-release
```

For a new binary, update the SHA-256 and version in
`installer/release/manifest.json`, upload the release assets, and run the
GitHub Pages workflow. The workflow verifies the hashes before deploying.
