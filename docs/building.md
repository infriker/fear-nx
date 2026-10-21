# Reproducing the F.E.A.R. NX build

This guide creates translated C++ from a user's Xbox 360 executable and cross-compiles it for Horizon ARM64. The XEX and generated source are private build inputs and outputs; they are not included in this repository.

## Pinned inputs

- ReXGlue SDK source base: c94f5ebdcb3c9d1a460ca48e04f9758448f8d518 (v0.10.0).
- Project8 Switch patch provenance: R-drg/project8-sw commit 3393f2615deb7e8ac11ce51d2cd0e9defded5669; its first 16 SDK patches are included unchanged in patches/rexglue-sdk/.
- Patch 17 is local to F.E.A.R. NX. It adjusts the compile-only ARM64 configuration, libmspack source path, and removes a diagnostic tied to another game's address. The patch sequence is fixed in patches/rexglue-sdk/series.
- Patch 18 is a generic Switch startup guard for a missing or empty argv[0]. It prevents CLI11's argc parser from underflowing its argument-vector reserve when a Horizon launcher supplies argc=0.
- Patch 19 validates or acquires a real self-process handle through loopback HIPC for memory mapping and resume stubs. It also requires exactly one exception-handler entry for each startup UDF probe trial, preventing an emulator that skips exception delivery from reporting a false pass.
- Patches 20 and 21 add commit-stage diagnostics and valid resource-limit queries.
- Patch 22 reserves one aligned virtual address arena for code shadows, while preserving exact backing allocation sizes and lazy view mapping. It is a candidate fix for the real-console campaign-loading mapping failures; see docs/hardware-diagnostics.md.
- Supported default.xex SHA-256: E7FCCDF6DD57BB58D10EFBDD0B2CA567B8CECE5B6A3FC696C7FCA879AAEA2792.
- ReXGlue project name: fear_xbox360.

The Project8 patch series was authored against ReXGlue v0.10.0 commit f5337cd; the pinned c94f5eb tree was verified source-identical. The bootstrap script checks the full SHA, not the short identifier.

## Requirements

- Windows PowerShell 5.1 or PowerShell 7.
- Git for Windows with network access for the SDK and recursive submodules.
- The stock ReXGlue Windows CLI v0.10.0 (rexglue.exe). The CLI must report 0.10.0 for --version; the Switch patches are applied to source, not to this executable.
- Docker Desktop for the ARM64 cross-build.
- Your legally obtained Xbox 360 copy of F.E.A.R., extracted so you can point to its default.xex.

## Current native-renderer candidate

For the 2026-10-21 renderer candidate, use
`scripts/bootstrap-native-sdk.ps1` and the complete `patches/native-vulkan`
snapshot. It includes the depth-bias Vulkan device changes required by the
current renderer. Do not combine it with the older numbered patch workflow
below. See [candidate instructions](native-render-stable.md).

## 1. Prepare the pinned SDK (historical numbered-patch workflow)

From the repository root, run:

~~~powershell
.\scripts\bootstrap-sdk.ps1
~~~

The script clones ReXGlue only when third_party/rexglue-sdk does not exist, checks out the pinned commit, initializes all recursive submodules, and applies the 22 patches in order with git apply --index. It records a marker in the SDK's private Git metadata so reruns can verify the exact patchset.

If an SDK directory already exists, the script never resets or cleans it. A clean checkout at the pinned base can be patched. A previously bootstrapped checkout is accepted only when its recorded patchset still matches. If the patch series changed, `-AdoptVerified` can refresh the private marker only after proving that the SDK's complete tracked diff exactly matches all 22 patches. An unfamiliar or additionally edited checkout is left untouched and rejected; use a separate, nonexistent -SdkPath if you need a clean bootstrap.

For a pre-patched checkout without the private marker, or one whose marker records the previous patch-series digest, first run .\scripts\bootstrap-sdk.ps1 -AdoptVerified. This verification mode constructs the expected patch diff in a temporary Git index, compares its SHA-256 with the existing tracked diff, and checks recursive submodule revisions. It refuses non-ignored untracked files and any mismatch. On an exact match it writes or refreshes only a marker under the SDK Git metadata; it does not change SDK source files or the real Git index. Then run .\scripts\bootstrap-sdk.ps1 -VerifyOnly to confirm the marker. Do not use this option to bless arbitrary local edits.

The 16 Project8 patches and the six local F.E.A.R. NX patches are visible and reviewable under patches/rexglue-sdk/. The script does not clone Project8 as a build dependency.

## 2. Generate C++ from your own XEX

Download/use the stock Windows ReXGlue 0.10.0 release and run:

~~~powershell
.\scripts\generate.ps1 -GameXex 'D:\YourExtractedGame\default.xex' -ReXGlueExe 'C:\Tools\rexglue-sdk\win-amd64\bin\rexglue.exe'
~~~

The script checks the XEX SHA-256 and CLI version before starting code generation. It copies only default.xex into the ignored assets/ directory. It then copies the tracked fear_xbox360_manifest.toml to the ignored fear_xbox360_manifest.local.toml and calls the documented CLI form:

~~~text
rexglue.exe codegen fear_xbox360_manifest.local.toml
~~~

ReXGlue stamps the manifest it receives, so the local copy keeps the checked-in configuration unchanged. The manifest's project name and generated target remain fear_xbox360; output goes to generated/default/. The checked-in manifest contains the current function-start configuration, not game code.

Generation only needs the supported default.xex; this step does not copy the disc's DATA files. Generated C++ and the executable are derived from the game and must stay local. Do not commit or distribute assets/default.xex, generated/default/, or the local manifest.

Optional: pass -Force to skip ReXGlue's overwrite confirmation. The normal command does not force regeneration when output stamps already match.

## 3. Build the ARM64 application

Build the local Docker image once:

~~~powershell
docker build -t fear-nx-switch:devkitpro-20260219-clang19 -f tools/switch/Dockerfile tools/switch
~~~

To compile the generated game and runtime as ARM64 objects while the Vulkan/NVK archive is being prepared:

~~~powershell
.\scripts\build-switch.ps1 -CompileOnly
~~~

This uses the repository's pinned devkitA64 Docker base image. It is a compile check; it does not produce a runnable NRO.

Code generation places about 94,946 SetFunction registration calls in fear_xbox360_register.cpp. That unit contains startup metadata rather than translated game routines. In the constrained one-vCPU, 3 GB build container, GCC spent more than 11 minutes optimizing it at -O3. The build compiles only this registration unit at -O0 and skips its precompiled header; translated game function bodies remain at -O3. This keeps the metadata compile practical without changing optimization for game code.

A full NRO build uses:

~~~powershell
.\scripts\build-switch.ps1
~~~

The full build also requires the completed NVK SDK under `sdk/nvk`, including
`lib/libvulkan.a`. This repository's bootstrap script does not build Mesa/NVK.
The current paused snapshot is the Switch-tested archive
`dist/fear-nx-switch-pass-sync-20261020.zip`; its NRO SHA-256 is
`C1C96F8D54278AA4C29D926C8D469FB8F735960A9FE892C427160256A5899585`. The user
confirmed fullscreen output, the original menu, and campaign gameplay on the
physical Switch. Lighting and occlusion artifacts remain. A slight performance
improvement was reported subjectively, without a matched benchmark. Eden still
has intermittent pauses of about three seconds, and a Windows Mesa build has
not been tested. See [validation status](status.md) for historical diagnostics
and the limits of each test. This records observed behavior; it does not claim
a clean-checkout rebuild of the packaged NRO.

## Optional: build the Horizon compatibility probe without game inputs

The isolated probe needs the pinned SDK and the full Horizon/NVK build environment, but does not require `assets/default.xex`, `generated/default/`, or F.E.A.R. game data. Configure it in a separate build directory:

~~~powershell
$ProjectRoot = (Get-Location).Path
docker run --rm --mount "type=bind,source=$ProjectRoot,target=/project" fear-nx-switch:devkitpro-20260219-clang19 cmake -S . -B out/switch-probe -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=/project/tools/switch/toolchain.cmake -DREXSDK_DIR=/project/third_party/rexglue-sdk -DREXGLUE_SWITCH_NVK_SDK=/project/sdk/nvk -DREXGLUE_SWITCH_COMPILE_ONLY=OFF -DFEAR_NX_HORIZON_PROBE_ONLY=ON
docker run --rm --mount "type=bind,source=$ProjectRoot,target=/project" fear-nx-switch:devkitpro-20260219-clang19 cmake --build out/switch-probe --target fear_nx_horizon_probe --parallel 2
~~~

Copy `out/switch-probe/tools/switch/horizon-probe/fear_nx_horizon_probe.nro` to `sdmc:/switch/fear-nx/` and launch it in Eden. The probe writes `sdmc:/switch/fear-nx/horizon-probe.log`. See [the probe README](../tools/switch/horizon-probe/README.md) for the checks and current test result.

## Keep game-derived files private

The repository ignores assets/, generated/, build products, and the local manifest copy. Keep the XEX, any extracted disc data, and generated translation output on your machine. Do not add them to commits, issue attachments, releases, or public build artifacts.
