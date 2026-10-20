# Horizon compatibility probe

This NRO isolates the Horizon-facing ReXGlue startup path from F.E.A.R. assets
and the Xenos GPU plugin. It checks:

- whether RexGmProcessHandle returns a real handle for the current process;
- whether one backing page can be mapped into two views with bidirectional
  writes through the MapProcessCodeMemory / MapProcessMemory path;
- 2,600 isolated 4 KiB commits in the physical aperture, two views per page,
  untouched holes, retained data, view-local protection, decommit/recommit,
  zero initialization and committed/mapped byte accounting, under the full
  4.5 GiB virtual reservation (which does not preallocate that much RAM);
- the SDK's real ExceptionHandler::Install path, including its 16-trial SIMD,
  FPCR, FPSR, and actual exception-entry-count probe;
- after that startup probe succeeds, whether a custom read DataAbort callback
  can change X0 and the resume PC.

It does not create a Vulkan instance, graphics system, or GPU plugin. The SDK's
Switch target helper still attaches its common NVK link interface; no renderer
or GPU initialization is called by this program.

## Build without F.E.A.R. inputs

The root CMake option `FEAR_NX_HORIZON_PROBE_ONLY=ON` configures just the SDK
and this probe before the normal F.E.A.R. asset and generated-source checks.
It requires the normal Horizon SDK/toolchain setup, including the available
NVK link interface, but not `assets/default.xex`, `generated/default/`, or
game data. From the repository root in PowerShell:

    $ProjectRoot = (Get-Location).Path
    docker run --rm --mount "type=bind,source=$ProjectRoot,target=/project" fear-nx-switch:devkitpro-20260219-clang19 cmake -S . -B out/switch-probe -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=/project/tools/switch/toolchain.cmake -DREXSDK_DIR=/project/third_party/rexglue-sdk -DREXGLUE_SWITCH_NVK_SDK=/project/sdk/nvk -DREXGLUE_SWITCH_COMPILE_ONLY=OFF -DFEAR_NX_HORIZON_PROBE_ONLY=ON
    docker run --rm --mount "type=bind,source=$ProjectRoot,target=/project" fear-nx-switch:devkitpro-20260219-clang19 cmake --build out/switch-probe --target fear_nx_horizon_probe --parallel 2

The `fear_nx_horizon_probe` target is excluded from the default build. With an
existing compatible Switch build directory it can also be built explicitly:

    cmake --build out/switch --target fear_nx_horizon_probe --parallel 2

Copy the resulting
`out/switch-probe/tools/switch/horizon-probe/fear_nx_horizon_probe.nro` (or
the matching path under `out/switch`) to `sdmc:/switch/fear-nx/` and launch it
in Eden or on a Switch. Its log is
`sdmc:/switch/fear-nx/horizon-probe.log`; SDK exception-probe stderr is routed
there too. A crash may also produce the standard ReXGlue crash report under
`logs/rex/`.

## Earlier exception-probe results

The earlier stock Eden 0.2.1 run used a 4,544,709-byte probe NRO with SHA-256
`a44ae82f5a74ddeb095200caac02f9072a3fd69052c64484555fc8c94fce07c5`. It passed
the process-handle and memory-alias checks, then observed zero exception
handler entries at its first UDF trial; the custom DataAbort test was not
reached in stock Eden.

The probe also passed in a separate local Eden v0.2.1 build. The tested NRO
was SHA-256
`06DB376D0A9AD3A6963D3B0666E501FD808DD86A50AFDD0E9640E6D56C402D48`.
It passed the real self-process-handle and bidirectional memory-alias checks.
Resume-stub mode 2 did **not** pass its first FP-state trial (FPSR expected
`0x1`, observed `0x11`). The SDK then selected its trap-resume fallback, which
passed 32 vector registers and FPCR/FPSR across 16 scheduled exceptions. The
custom DataAbort handler passed its resume-PC and X0 writeback check; the
probe logged `RESULT PASS` and exited with status 0. Thus the probe's
HBL64 exception path works in this local build using the fallback. Do not
interpret this as a passing mode-2 trial or complete exception-ABI support.

The earlier stock Eden 0.2.1 run remains a separate result: it passed the
self-handle and memory-alias tests but received zero exception-handler entries
at the first UDF trial. The custom Eden change and test procedure are
documented in `patches/eden/README.md`. The separate asset-free probe has not
been tested on a physical Switch. The full game's startup probe did pass
mode 2 on a real Switch Lite, as recorded in docs/hardware-diagnostics.md.
## Memory-arena test result

The initial 2600-sparse case failed at index 2500 with CE01; it is preserved in
logs/memory-arena-probe-20261020/probe-2600-failed.log. Eden has a fixed app pool
of 20000 KMemoryBlock records, consistent with this fragmented workload, but
exact live pool usage was not instrumented. The revised test runs 2000 sparse
and 2600 dense commits. It passed both cases, protection/decommit/recommit and
accounting, then the existing exception tests via trap-resume fallback, ending
in RESULT PASS. Probe SHA-256 is
BCAA9104573082552633A44BC50F1D37030E23F61C7512659D969CA048B09401.
These are isolated Eden results, not physical-console or campaign validation.
