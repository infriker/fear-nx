# Local Eden CLI build for F.E.A.R. NX testing

> Snapshot notice (2026-10-20): the runtime observations below are historical.
> See `docs/status.md` at the repository root for the latest user-tested Switch
> result. Complete final dependency changes, including unfinished Eden
> diagnostics, are preserved in `patches/checkpoint-20261020/` as alternative
> patches against their exact bases. Do not apply both patch workflows.

This guide builds a separate Windows `eden-cli.exe` from Eden `v0.2.1` for testing the Horizon compatibility probe. It does not replace or modify an installed Eden copy. It does not include the game, and a successful compile is not evidence that the custom emulator patch works at runtime.

## Pinned inputs

- Eden source: [`eden-emulator/mirror`](https://github.com/eden-emulator/mirror), tag `v0.2.1`, commit `58c1e20ee58efa3900ba616207d460886214480b`.
- Local patch series: [`patches/eden/series`](../patches/eden/series), currently containing `0001-horizon-homebrew-user-exceptions.patch`. It is an experiment for 64-bit homebrew user-exception delivery, not an upstream patch or a complete user-exception implementation. Its limitations are in [`patches/eden/README.md`](../patches/eden/README.md).
- Host shader compiler: official Khronos [`glslang`](https://github.com/KhronosGroup/glslang) tag `16.6.0`, commit `e1b562a8bed273a02f30b59b66a5d499793cede5`. Eden configure needs `glslangValidator`; this build uses that standalone tool and does not install a full Vulkan SDK.

## Requirements and compiler selection

Use a 64-bit Windows host with Git, Visual Studio Build Tools, CMake, Ninja, and Python available. Set the tool paths below to the versions installed on the host. The verified build used MSVC 14.44, CMake 4.3.1, Ninja 1.13.2, and Python 3.12.14.

The build must use MSVC `cl.exe` from an initialized Visual Studio environment. The initial default VS environment selected Clang and its CMake compiler test failed with missing `oldnames.lib` and `msvcrtd.lib`. Calling `VsDevCmd.bat` with `-vcvars_ver=14.44 -host_arch=amd64 -arch=amd64` and passing the matching `cl.exe` path fixed compiler detection and linking. Clang-cl was not validated by this build.

CMake 4.3.1 also captured the localized MSVC `/showIncludes` prefix with the wrong encoding, causing Ninja to record `#deps 0` for compiled objects. Setting `VSLANG=1033` did not change MSVC's output language. The override below supplies the exact prefix observed from this compiler. If another MSVC installation emits a different `/showIncludes` label, update the three values to match its output exactly.

## Choose local paths

Run the commands from the F.E.A.R. NX repository root. Change the tool paths for the local installation. Keep source and build directories separate from the installed Eden application and from any game-data directory.

```powershell
$ProjectRoot = (Resolve-Path '.').Path
$ThirdParty = Join-Path $ProjectRoot 'third_party'
$Out = Join-Path $ProjectRoot 'out'
$EdenSource = Join-Path $ThirdParty 'eden'
$EdenBuild = Join-Path $Out 'eden-windows'
$GlslangSource = Join-Path $ThirdParty 'glslang'
$GlslangBuild = Join-Path $Out 'glslang'

# Set these paths to the installed tools on this Windows host.
$VsDevCmd = 'C:\path\to\BuildTools\Common7\Tools\VsDevCmd.bat'
$MsvcVersion = '14.44'
$Cl = 'C:\path\to\MSVC\14.44.x\bin\Hostx64\x64\cl.exe'
$CMake = 'C:\path\to\cmake.exe'
$Ninja = 'C:\path\to\ninja.exe'
$Python = 'C:\path\to\python.exe'
```

The clone/build folders below are expected to be new. If they already exist, inspect them and keep any local changes; these commands do not reset or clean existing source checkouts.

## Fetch and apply the pinned sources

```powershell
git clone --depth 1 --branch v0.2.1 https://github.com/eden-emulator/mirror.git $EdenSource
$actual = (git -C $EdenSource rev-parse HEAD).Trim()
if ($actual -ne '58c1e20ee58efa3900ba616207d460886214480b') { throw "Unexpected Eden revision: $actual" }
$patch = Join-Path $ProjectRoot 'patches\eden\0001-horizon-homebrew-user-exceptions.patch'
git -C $EdenSource apply --check $patch
git -C $EdenSource apply $patch

git clone --depth 1 --branch 16.6.0 https://github.com/KhronosGroup/glslang.git $GlslangSource
$actual = (git -C $GlslangSource rev-parse HEAD).Trim()
if ($actual -ne 'e1b562a8bed273a02f30b59b66a5d499793cede5') { throw "Unexpected glslang revision: $actual" }
```

Apply the Eden patch only to the separate source clone above. Do not apply it to an installed Eden folder. When the patch series changes, apply each entry in `patches/eden/series` in order.

## Build the standalone glslang tool

```powershell
$configure = 'call "' + $VsDevCmd + '" -no_logo -vcvars_ver=' + $MsvcVersion + ' -host_arch=amd64 -arch=amd64 && "' + $CMake + '" --fresh -S "' + $GlslangSource + '" -B "' + $GlslangBuild + '" -G Ninja "-DCMAKE_MAKE_PROGRAM=' + $Ninja + '" "-DCMAKE_C_COMPILER=' + $Cl + '" "-DCMAKE_CXX_COMPILER=' + $Cl + '" "-DPython3_EXECUTABLE=' + $Python + '" -DCMAKE_BUILD_TYPE=Release -DBUILD_EXTERNAL=OFF -DBUILD_TESTING=OFF -DGLSLANG_TESTS=OFF -DGLSLANG_ENABLE_INSTALL=OFF -DENABLE_OPT=OFF -DENABLE_HLSL=OFF -DENABLE_GLSLANG_BINARIES=ON'
cmd.exe /d /c $configure
if ($LASTEXITCODE -ne 0) { throw "glslang configure failed: $LASTEXITCODE" }

$build = 'call "' + $VsDevCmd + '" -no_logo -vcvars_ver=' + $MsvcVersion + ' -host_arch=amd64 -arch=amd64 && "' + $CMake + '" --build "' + $GlslangBuild + '" --target glslang-standalone --parallel 2'
cmd.exe /d /c $build
if ($LASTEXITCODE -ne 0) { throw "glslang build failed: $LASTEXITCODE" }
$GlslangValidator = Join-Path $GlslangBuild 'StandAlone\glslangValidator.exe'
if (-not (Test-Path -LiteralPath $GlslangValidator)) { throw "Missing $GlslangValidator" }
```

## Configure and build Eden's CLI target

```powershell
$RulesOverride = Join-Path $Out 'eden-msvc-showincludes-override.cmake'
@(
    'set(CMAKE_C_CL_SHOWINCLUDES_PREFIX "Примечание: включение файла:  ")',
    'set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "Примечание: включение файла:  ")',
    'set(CMAKE_CL_SHOWINCLUDES_PREFIX "Примечание: включение файла:  ")'
) | Set-Content -LiteralPath $RulesOverride -Encoding utf8

$configure = 'call "' + $VsDevCmd + '" -no_logo -vcvars_ver=' + $MsvcVersion + ' -host_arch=amd64 -arch=amd64 && "' + $CMake + '" -S "' + $EdenSource + '" -B "' + $EdenBuild + '" -G Ninja "-DCMAKE_MAKE_PROGRAM=' + $Ninja + '" "-DCMAKE_C_COMPILER=' + $Cl + '" "-DCMAKE_CXX_COMPILER=' + $Cl + '" "-DPython3_EXECUTABLE=' + $Python + '" "-DPython_EXECUTABLE=' + $Python + '" "-DGLSLANGVALIDATOR=' + $GlslangValidator + '" "-DCMAKE_USER_MAKE_RULES_OVERRIDE=' + $RulesOverride + '" -DCMAKE_BUILD_TYPE=Release -DENABLE_QT=OFF -DYUZU_CMD=ON -DYUZU_TESTS=OFF -DDYNARMIC_TESTS=OFF -DYUZU_ROOM=OFF -DYUZU_ROOM_STANDALONE=OFF'
cmd.exe /d /c $configure
if ($LASTEXITCODE -ne 0) { throw "Eden configure failed: $LASTEXITCODE" }

$build = 'call "' + $VsDevCmd + '" -no_logo -vcvars_ver=' + $MsvcVersion + ' -host_arch=amd64 -arch=amd64 && "' + $CMake + '" --build "' + $EdenBuild + '" --target yuzu-cmd --parallel 4'
cmd.exe /d /c $build
if ($LASTEXITCODE -ne 0) { throw "Eden CLI build failed: $LASTEXITCODE" }
```

The generated executable is `${EdenBuild}\bin\eden-cli.exe`. CPM downloads are kept inside the Eden source clone under `.cache/cpm`. This config disables Qt, tests, and room targets; it builds the SDL command-line frontend target `yuzu-cmd`, whose output file is named `eden-cli.exe`.

For runtime tests, keep this executable, its `user` profile, and test logs in their own portable directory under the build output. Do not share the installed Eden user's profile. Use only the asset-free Horizon compatibility probe while investigating emulator support; game data is not needed for this build step.

## Verified result and current limit

The isolated Windows CLI configure and build succeeded with the tool versions above; the target completed with exit code 0 and produced a 32.7 MB `eden-cli.exe`. The build was repeated incrementally after an NRO loader change and relinked successfully. `dumpbin /dependents` showed Windows system DLLs and the MSVC runtime; no extra third-party runtime DLLs were present in the output folder.

The isolated Horizon probe has now been runtime-validated against the local custom Eden build. Probe 3 exited with status 0 and logged `RESULT PASS`: the self-process handle and memory aliases passed, then mode 2 failed its first FP-state check (FPSR `0x1` was observed as `0x11`). The SDK's trap-resume fallback passed its 32-vector FPCR/FPSR checks across 16 scheduled exceptions, and the custom DataAbort handler passed its resume-PC and X0 writeback checks. This validates the probe's HBL64 exception path using the fallback; it does not establish that mode 2 works or that Eden's full exception ABI is implemented. Nonzero ReturnFromException results and KDebug replay remain unsupported.

## Full-game progress after the Horizon probe

Test 9 traced the host crash to the unbound copy engine on subchannel 4 at
method `0x1C0`; Mesa patch `0006` binds requested engines before the first
client packet and was included in the subsequent NRO link. Test 10 progressed
through GPU-ring and render-target setup, then logged GPU macro-JIT parameter
overruns and a Maxwell constant-buffer assertion. Test 11 continued swapping
at about 30 frames per second with no skipped draws while the window stayed
black; the saved thread data indicates guest execution was active, but no
visible game frame was proven. Its Eden app log was stale Test 10 data, and
the reason was not established.

In the captured Test 12 interpreter-mode logs, Eden reports invalid Maxwell
shader instruction words and missing pipeline support for `IMUL (imm)` in
graphics shaders and `IMUL (cbuf)` in compute shaders. The capture contains no
macro-JIT parameter-overrun or Maxwell constant-buffer assertion, but does not
show a rendered menu. The tested NRO SHA-256 is
`BE081311DB314FF16E27A682250EA4E28395C82E837986FE62C1046CB9C52FA1`; the
Eden host executable SHA-256 is
`BE4B615148F4C5DDA71571C78E060849F753D0260420D272DD114C808214F933`.
Point-in-time logs: [`eden-12-runtime.log`](../logs/eden-12-runtime.log) and
[`fear-nx-eden-12.log`](../logs/fear-nx-eden-12.log). See
[validation status](status.md) for the detailed test sequence. The original
F.E.A.R. menu, gameplay, and controller mapping remain unverified. No physical
Switch test is covered by this guide.

