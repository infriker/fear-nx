# Building F.E.A.R. NX

Build from your own supported Xbox 360 `default.xex`. Original game data and
generated translations stay outside Git. The release installer is the simpler
route if you only want to play.

## Requirements

- Windows with PowerShell 7 and Git.
- Docker Desktop with its Linux engine running.
- ReXGlue Windows CLI **0.10.0** for code generation.
- Your extracted Xbox 360 game, including the supported `default.xex`.
- Disk space for generated C++, dependencies and ARM64 build output.

## 1. Prepare the SDK

From the repository root:

```powershell
pwsh -File scripts/bootstrap-native-sdk.ps1
```

This restores the complete source snapshot in `patches/native-vulkan` onto
ReXGlue commit `c94f5ebdcb3c9d1a460ca48e04f9758448f8d518` and initializes the
recorded submodules. An existing edited SDK is accepted only if it matches the
snapshot; the script does not reset it. Use `-SdkPath` for another destination
and `-VerifyOnly` to check an existing checkout.

The numbered patches and the older checkpoint SDK snapshot are retained as
source history. Do not apply them on top of this complete snapshot.

## 2. Generate the game translation

```powershell
pwsh -File scripts/generate.ps1 -GameXex 'D:/MyGame/default.xex' -ReXGlueExe 'C:/Tools/rexglue/bin/rexglue.exe'
```

The script validates the XEX and CLI version, stages the executable in
`assets/default.xex`, and generates `generated/default/`. Code generation must
finish before CMake configuration. It does not copy the `Game/` data directory.

## 3. Prepare the toolchain and driver

```powershell
docker build -t fear-nx-switch:devkitpro-20260219-clang19 -f tools/switch/Dockerfile tools/switch
```

Build the driver using [driver-build.md](driver-build.md). The full build needs
`sdk/nvk/lib/libvulkan.a` and `sdk/nvk/lib/libnak_rs.a`.

## 4. Compile and package the NRO

```powershell
pwsh -File scripts/build-switch.ps1 -Jobs 4
```

Outputs include `out/switch/fear_nx` (AArch64 ELF), `fear_nx.nacp` and
`fear_nx.nro`. `-ConfigureOnly` stops after configuration; `-CompileOnly` builds
objects without producing a runnable NRO. Both options are for development.

Copy the NRO and `release/fear-nx.toml` into `sdmc:/switch/fear-nx/`, then place
your executable and extracted `Game/` under its `game/` subdirectory.
See [installation](installing.md) for the full layout.

## Publishing a build

`scripts/package-release.py` packages an existing NRO with the configuration
and notices. It never reads or packages the original game files.

```powershell
python scripts/package-release.py --nro out/switch/fear_nx.nro --output dist/public-release
```

For a new binary, update the SHA-256 and version in
`installer/release/manifest.json`, upload the release assets, and run the
GitHub Pages workflow. The workflow verifies the hashes before deploying.
