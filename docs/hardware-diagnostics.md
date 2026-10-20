# Switch Lite incident, 20 October 2026

The user confirmed that the original menu and controller navigation worked, but
Chapter 01 loading froze and HOME stopped responding. The displayed image was
cropped on the right. This does not establish gameplay compatibility.

The supplied `fear-nx_001.1.log` and `rex/` logs were preserved locally under
`logs/switch-first-run/20261020-182411/`, with SHA-256 hashes. They came from NRO
`BE081311DB314FF16E27A682250EA4E28395C82E837986FE62C1046CB9C52FA1`.

## Evidence

- At 18:24:57.025, the first of 14 guest-memory commit failures reports
  `0xCE01`, with 365 MiB backing and 418 MiB mapped across guest views.
  Subsequent small requests also fail; counters reach 371/423 MiB.
- In libnx, `0xCE01` is module 1, description 103,
  `KernelError_ResourceExhausted`. It is different from `OutOfMemory` (104).
  The existing `AllocFixed` message does not identify which mapping SVC failed.
- The final renderer interval reports 5 shaders and 4 pipelines total.
  Slow-frame counters report no new shaders, pipelines or texture uploads.
  The captured evidence therefore does not support an active shader-compilation
  stall as the explanation for this incident.
- The real console validates the loader process handle and passes the mode-2
  exception probe, including FPCR/FPSR preservation. The earlier Eden fallback
  result is not the real-console result.
- The display, swapchain and reported frontbuffer are 1280x720. Smaller offscreen
  render targets are not by themselves evidence that the display resolution is
  wrong. The original log lacks active viewport and resolve rectangles.
- The process memory figures are 3185/3189 MiB. Under a hbloader heap override,
  `UsedMemorySize` includes the preallocated heap; it is not malloc's live usage
  or a measurement of remaining heap space.
- The old profiler requested `InfoType_ResourceLimit` with a process handle
  instead of the required zero handle, so its `0/0` field was not valid evidence.

The exact exhausted resource and the reason HOME became unresponsive remain
unconfirmed. Atmosphere's mapping implementation can return description 103
when allocating memory-block metadata or page tables. This makes those resources
worth measuring; it does not prove they are the cause in this run.

## Diagnostic changes

SDK patch 0020 records the exact commit stage and result, chunk size distribution,
mapped-view counts, and `SystemResourceSizeUsed/Total` results at failure.
SDK patch 0021 corrects the resource-limit query, closes the returned handle,
and reports resource query results explicitly in the periodic profile.

`nx.native_trace_output = true` adds bounded viewport/scissor and swap/resolve
geometry records. It leaves rendering behavior unchanged. No thread-suspension
diagnostic is enabled by this profile.

The SDK remains based on ReXGlue 0.10.0. The allocator strategy, generated game
code, Mesa driver and render scaling remain unchanged. This is a diagnostic
build, not a confirmed fix.

## Sources

- [libnx result definitions](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/result.h)
- [libnx heap initialization](https://github.com/switchbrew/libnx/blob/master/nx/source/runtime/init.c)
- [Atmosphere page-table mapping implementation](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/kern_k_page_table_base.cpp)
- [Atmosphere memory-block allocator](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/include/mesosphere/kern_k_memory_block_manager.hpp)
- [NFS Switch platform notes](https://github.com/StevensND/nfsmw-nx/blob/main/docs/platform-notes.md)

NFS's shader pretranslation workflow is a useful later reference. A FEAR shader
pack must be generated from FEAR's own shaders and the matching translator;
it cannot replace the investigation of the allocation failures above.


## Second hardware run: 18:53:05, 20 October

The user tested diagnostic NRO
`E80D3646267920567FEC4BAB7439AB3FE5DB8CC80321F00A89F14DFA3B05340E`
and again reported a campaign-loading freeze. The supplied `fear-nx_002.log`,
`rex_perfil.log` and `rex_stderr.log` are preserved with SHA-256 hashes under
`logs/switch-first-run/20261020-185305/`. Original supplied files were not changed.

The added telemetry narrows the allocation failure:

- Nine failures identify stage `map-code-memory`, result `0xCE01`, module 1,
  description 103. Heap allocation had already succeeded, and permission setup
  had not yet run.
- The first failure requests 8 KiB at guest offset `0x112736000`, in the physical
  aperture. At that point there are 1,770 chunks, 385,740,800 committed bytes,
  440,500,224 mapped-view bytes, and 1,775 mapped view/chunk pairs.
- The last failure reaches 1,821 chunks, 390,844,416 committed bytes and
  445,542,400 mapped-view bytes. Every recorded failing offset is in the physical
  aperture `[0x100000000, 0x120000000)`.
- The corrected resource-limit query succeeds: current 3,339,763,712 and maximum
  3,343,908,864 bytes. These values remain unchanged across the samples.
  The preallocated hbloader heap is included; the difference is not malloc free
  space. Raising the heap blindly is not justified.
- SystemResourceSizeUsed and Total queries return success and zero. These are
  process kernel-management resource counters, not free system DRAM. Their zero
  values do not identify the exhausted resource or establish a free-memory budget.
- Renderer totals remain 5 shaders and 4 pipelines. The captured interval does
  not support shader compilation as the explanation for this freeze.
- The real-console process handle and mode-2 SIMD/FP exception probe still pass.

The previous allocator requested a new random code address for every chunk.
The second argument to `virtmemFindCodeMemory(size, guard_size)` is a guard size,
not an alignment. Thus even a 2 MiB-aligned backing allocation did not imply a
2 MiB-aligned shadow; small allocations could occupy many different page tables.

Patch 0022 instead reserves a single virtual arena and maps code shadows at
`arena + guest_offset`. It keeps backing sizes, view aliasing, protection policy,
and lazy commitment unchanged. The 4.5 GiB arena reserves address space, not
4.5 GiB of RAM. This is a candidate fix for mapping resource pressure, not proof
of the exact kernel allocation that failed. In particular it does not coalesce
the chunk records or reduce the number of mapping calls.

The small opaque suffix after the final textual log line is not sufficient
evidence of game memory corruption.

### Output crop

The second trace confirms a mismatch: swap packet and fetch are 1280x720, but
the selected resolve allocation, last resolve rectangle and output are 768x720.
The draw trace also contains a 1280x720 viewport with a 768x720 scissor.
This is now evidence in the render/resolve path, not a DPI setting.

The next build adds at most 24 unique resolve records including raw copy,
scissor, window-offset and destination-pitch registers. It compares the native
calculation with the SDK's reference resolve dimensions, tiled destination
offsets and EDRAM span. Repeated identical records allocate no new key storage
and do not call the reference calculation again. Rendering behavior and scale
are unchanged; stretching 768 pixels would not recover potentially missing data.

### Targeted validation

The asset-free Horizon probe now reserves the full guest address window and
exercises 2,600 isolated physical-aperture commits, each with two aliases and
uncommitted holes. It checks retained data, view-local protection, decommit and
zeroed recommit, byte accounting, plus the existing exception-resume tests.
Its Eden result must be recorded separately from a physical console result:
emulator success cannot prove that Horizon's kernel resource pressure is fixed.

Additional primary references:

- [libnx virtual-memory API](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/kernel/virtmem.h)
- [libnx random address search](https://github.com/switchbrew/libnx/blob/master/nx/source/kernel/virtmem.c)

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

## Eden campaign run: 19:19, 20 October

The user launched the B3CCF37C build as fear_nx_memoryfix.nro. A live snapshot
was read without stopping the emulator and preserved under
logs/eden-campaign-memoryfix-20261020-191912/ with hashes. The main log is
fear-nx_012.log. rex_crash.log is append-only and includes earlier probe crashes;
only records with the current image base 0x80B7C000 belong to this game run.

At 19:20:03.185 (main log line 859), memalign failed for 0x810000 bytes (8.0625 MiB)
at guest offset 0x45070000. The SDK result 0xFFFF0001 is a synthetic heap failure,
not a kernel result; its decoded module/description fields are not meaningful.
Committed backing was 314994688 bytes, mapped views 369229824 bytes, chunks 1138.
The retry also failed and line 865 reports an unhandled guest write to address 0.
Afterwards audio dispatches continue but graphics records zero packets/draws/swaps.
Waiting for shader compilation will not recover that guest fault.

The runtime declares a 1 GiB newlib heap in src/ui/runtime_switch.cpp:106.
Eden reports 1162506240 used bytes at startup and throughout the run, including
that reserved heap, image and other process memory. The value is not live malloc
usage. The failed memalign proves lack of a suitable allocation in this run,
not whether exhaustion or fragmentation dominates. Capture allocator statistics
and test a bounded heap increase before using an uncapped heap. This is separate
from the physical Switch's MapProcessCodeMemory CE01; this run has no CE01 and
stopped earlier, so it does not establish that the physical failure was fixed.

The resolve trace also pinpoints the crop. Frontbuffer 0x1EB30000 receives a
768x720 resolve; immediately afterwards, 0x1EB44000 receives 640x720 with a
1280-pixel destination pitch. The delta 0x14000 equals the Xenos tiled offset
for x=640, y=0, pitch=1280, log2(bytes-per-pixel)=2. Another pair repeats at
0x1E798000 / 0x1E7AC000. The second pass's scissor is x=640..1280 and the signed
window offset is -640. The native texture cache treats these as separate images,
and Swap retrieves only the exact frontbuffer base, hence the missing right
segment. Correct reconstruction must account for tiled subregion offsets in
resolve/texture storage (including subsequent texture sampling), not stretch
the first 768 pixels. No crop or flicker behavior was changed during this review.

## Third hardware run: campaign success, 19:25:49–19:30:18

The user reports that the B3CCF37C memory-fix NRO now loads Chapter 01 and
provided a courtyard gameplay photo. Original evidence is preserved under
logs/switch-campaign-success-20261020/ with snapshot-sha256.json; the supplied
files were not modified.

The log explicitly identifies the compact shadow arena (base 0xDA9C00000,
size 0x120001000, alignment 0x200000). There are no recorded guest commit,
AllocFixed or BaseHeap allocation failures. Backing/mapped-view totals grow
from 183/161 MiB to 517/663 MiB, beyond the prior mapping-failure point.
The real process-handle and mode-2 SIMD/FP exception tests pass. Rendering
continues near the end (189 swaps in one ten-second interval). This is evidence
of one successful campaign startup and several minutes of rendering, not a
complete campaign or long-duration stability result.

### Performance

Some early stalls coincide with compilation: one interval adds 57 shaders and
27 pipelines; another adds 48 pipelines with 82 frames over 100 ms. Later
intervals remain slow with no new shaders or pipelines: about 12.6 FPS at
19:28:14, 11.4 FPS at 19:29:34, and 18.9 FPS near the end. The final renderer
sample reports 42.8 ms CPU / 32.3 ms GPU; these times overlap and must not be
added. A warm cache can reduce compilation hitches without removing sustained
rendering cost.

An 88 KB driver cache loaded at startup. Source SaveDriverCache is invoked
after five seconds without new pipelines and writes on a joined background
thread; saving does not require an orderly game exit. The supplied log does
not prove whether the latest cache file was successfully written to SD.

This run changes clocks: initially CPU 1020 / GPU 460.8 / RAM 1600 MHz, later
CPU 1963.5 / GPU 614.4 / RAM 2666 MHz. The whole run is not a stock-clock
benchmark. No clock settings have been changed in response.

### Rendering and MSAA

The split resolves again use 0x1EB30000 / 0x1EB44000 and
0x1E798000 / 0x1E7AC000, with a 0x14000 tiled address delta corresponding to
x=640 at a 1280-pixel pitch and 32 bpp. The image needs composition at its
original coordinates, including offscreen sampling. Stretching the first
768 pixels or changing DPI cannot recover the missing segment.

Main-log line 532 separately warns:
`[native] Depth resolve not done: 768x720, 2x MSAA`.
This can affect depth-dependent effects, but the photo alone does not identify
which defect causes the lighting strip.

The existing nx.native_msaa=false path selects one host sample consistently
for render targets, graphics pipelines and resolve/copy code while retaining
the guest's EDRAM dimensions. It also selects the implemented single-sample
depth-copy path. The opt-in profile changes only this setting. Do not claim
either faster gameplay or corrected lighting until a same-scene test has run.
Resolution changes, dynamic resolution and Eden flicker changes are outside
the current user-authorized optimization scope.

### User follow-up: MSAA disabled

The user tested the configuration-only change and reported a small improvement,
then explicitly deferred performance work until the frame crop is fixed.
No same-scene timing capture is available for this comparison, so no percentage
gain or clock-normalized improvement is established. Keep MSAA off in the next
user test; do not change resolution or add other performance tweaks.

## Frame-composition candidate

NRO BF0868D8B44E82C4F0ADD25BF54A9B7AA707844861EF683D8C6F3DF96524B613 is packaged as
dist/fear-nx-switch-frame-fix-20261020.zip and staged separately as
dist/switch/fear-nx/fear_nx_framefix.nro. It composes the validated RGBA8 left
and right resolve regions into one image before texture reads or presentation.
There is no image stretch or resolution reduction. Configuration retains the
MSAA-off value already tested by the user.

The host test compares 32x32 tile origins against the compiled SDK
GetTiledOffset2D implementation, rejects unsupported small texel layouts and
out-of-bounds copies, tests the 128-pixel overlap, and checks composite-state
transitions. Build/test results are in logs/tiled-resolve-20261020/.
ARM64 compile/link and packaging checks passed.

No runtime test of this NRO was performed; the user chose to launch Eden
manually. Verify a full 1280x720 menu and then the same campaign view on Switch.
Trace output should include a tiled-composite root with extent 1280x720 and
swap output=1280x720. If lighting remains confined to an edge, preserve the
new depth records; depth resolve assembly is outside this narrow color patch.

## Eden run 014: frame composition partly improves output

The user tested BF0868D8 manually and closed Eden to flush the application log.
Evidence: logs/eden-framefix-014-20261020/manifest.json, fear-nx_014.log,
eden_log.txt, rex_perfil.log and menu-black-right.png. This supersedes the
candidate's earlier runtime-pending note above.

The user sees the left part at the intended scale, with the right side black.
At lines 179/181/183/185 the trace records a 768x720 root at 1EB30000,
a 640x720 segment at 1EB44000, composite placement at x=640 and 1280x720
output. The second framebuffer pair repeats the same pattern. This proves
selection/dimensions, not successful pixel copying. No copy validation error
is logged; VA-teardown errors do not establish a causal link to the black area.

Project 8's existing copy_color compute helper accepts both source/destination
origins; its comment explains avoiding NVK meta-copy overhead. The new
compositor was instead always using vkCmdCopyImage. Routing eligible copies
through the helper is a justified differential test, not yet a proven fix.
Bounded pixel readback is needed to distinguish a blank segment render/resolve
from a failed merge or downstream presentation error.
