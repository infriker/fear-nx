# Validation status — 2026-10-20 (paused snapshot)

## Current hardware-tested build

Archive: `dist/fear-nx-switch-pass-sync-20261020.zip`.
NRO SHA-256: `C1C96F8D54278AA4C29D926C8D469FB8F735960A9FE892C427160256A5899585`.

The user confirmed fullscreen output, the original menu, and campaign gameplay on
the physical Switch. Lighting and occlusion artifacts remain. The user reported
a slight performance improvement, but there is no matched benchmark, so no
performance gain is established. Eden still has intermittent pauses of about
three seconds; this is not a reliable runtime pass. A Windows Mesa build/test
was not performed. Development is paused at this snapshot.

## Historical test record

The notes below preserve results from earlier builds and diagnostic runs. They
are not claims about the current package unless explicitly identified above.

An earlier test recorded Chapter 01 loading and gameplay rendering on Switch Lite
with compact-shadow-arena NRO B3CCF37C. The third hardware run continues for about
269 seconds, reaching 517 MiB backing / 663 MiB mapped views without any recorded
guest commit or fixed-allocation failure. This supersedes the two earlier
campaign-loading freezes, but does not prove full-campaign stability.

Output remains cropped and the user reports lighting confined to the right
edge. The trace proves a split 1280x720 resolve is stored as two independent
images (768 and 640 pixels); Swap selects only the first. A separate warning
reports unsupported 2x MSAA depth resolve. These are distinct correctness issues.
See hardware-diagnostics.md for evidence and test boundaries.

### Historical Eden candidate: tiled frame composition

NRO SHA-256: BF0868D8B44E82C4F0ADD25BF54A9B7AA707844861EF683D8C6F3DF96524B613.
Archive: dist/fear-nx-switch-frame-fix-20261020.zip.
Manual Eden test: dist/switch/fear-nx/fear_nx_framefix.nro.
The shared staged profile now has native_msaa=false; its previous version is
in logs/tiled-resolve-20261020/staged-config-before-framefix.toml.
The B3CCF37C memory-fix NRO and ZIP remain unchanged.

Validated RGBA8 tile segments are composed at their original coordinates into
one full-size image used by both texture reads and presentation. Matching
requires exact tiled address offsets, common pitch/height/format/swizzle,
same-frame root metadata and matching scissor/window-offset evidence.
Recognized root writes refresh the existing composite, including its retained
unwritten regions; unsupported writes invalidate it. Composition is recorded
after the producing resolve in the draw command buffer, with transfer/shader
dependencies. Retained composites are capped at 32.

Host layout/bounds/state tests passed, including comparison with the SDK's
GetTiledOffset2D for 4- and 8-byte formats. The runtime hook is deliberately
limited to the observed 4-byte RGBA8 color case. ARM64 compilation/linking,
NRO structure and ZIP integrity checks passed. ReXGlue 0.10.0, all 22 existing
SDK patches and Mesa/NVK are unchanged.

The user manually tested BF0868D8 in Eden (run 014): partial improvement,
not a passed frame-correctness test. The left part has the intended scale;
the right part remains black. Logs confirm both resolve segments, active
1280x720 composition and 1280x720 Swap, but do not establish pixel content.
Evidence including the screenshot and exact binary hash is preserved in
logs/eden-framefix-014-20261020/. No physical Switch test of this NRO is recorded.
Full lighting correctness is not established: depth composition is not
generalized by this patch. Eden flicker and performance work remain deferred.

The earlier B3CCF37C Eden campaign run failed at 19:20:03 on an 8.0625 MiB
heap allocation (no CE01). Its subsequent physical Switch run reached
Chapter 01 successfully. Those are distinct runtime results, not evidence
that the newer frame-fix binary has been tested.

### Historical memory-probe result

Probe SHA-256: BCAA9104573082552633A44BC50F1D37030E23F61C7512659D969CA048B09401.
Local Eden logged RESULT PASS for 2000 sparse and 2600 dense physical-aperture
commits, two aliases, retained data, protection, decommit/recommit and accounting.
Exception tests passed using the existing trap-resume fallback; mode 2 still
failed the first FP-state trial in this emulator. Real Switch startup had
previously passed mode 2, as recorded in hardware-diagnostics.md.

The initial 2600-sparse test failed at index 2500 with CE01. Eden's fixed app
pool of 20000 KMemoryBlock records is consistent with that ceiling; exact live
pool usage was not captured. The failure and successful tests are both preserved
under logs/memory-arena-probe-20261020/. Success below the sparse ceiling does
not prove removal of all mapping resource limits or a campaign-load fix.
### Historical hardware run and MSAA-off test profile

Evidence is preserved under logs/switch-campaign-success-20261020/ with hashes:
fear-nx_003.log, rex_perfil.log, rex_stderr.log and the user's gameplay photo.
The previous memory-fix ZIP and NRO remain available unchanged.

Steady sections remain about 10–21 FPS even without new shaders or pipelines.
The run includes both stock-like and raised clocks; compare profiles only at
the same CPU/GPU/RAM frequencies. New-pipeline hitches and sustained rendering
cost must be measured separately.

release/fear-nx-no-msaa.toml differs from the diagnostic profile only by
nx.native_msaa=false. It uses the existing renderer option and works with the
already-built B3CCF37C NRO; no SDK, guest code, resolution or clock change is
required. The configuration-only package is dist/fear-nx-msaa-off-20261020.zip,
including the original diagnostic configuration for rollback. All 14 keys
validate, and parsed configurations differ by that one value only.
The user subsequently tested MSAA-off on Switch and reported a small improvement.
No matched quantitative benchmark or full effect-correctness check was supplied.
The user deferred further performance work and requested the frame-crop fix first.
