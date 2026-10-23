# deko3d test and optimization history

Updated 2026-10-23. This document consolidates the renderer experiments into
one history. It records unsuccessful changes as well as improvements.

**Current Switch report: 20–30 FPS with overclocking, with frequent drops.
Vulkan currently provides more stable performance.** Exact overclock values
were not supplied for this report. This is not a stock-clock benchmark.

## How to read the results

`E###` identifies an Eden application log/run, `S###` a physical Switch run,
and `P###` an explicitly numbered source patch. These counters are independent:
for example, P061 was tested in E064. A missing number is not a missing Git
commit. Early tests were recorded before the branch was curated.

Unless marked **Switch**, numeric timings below come from Eden diagnostics.
They do not predict Switch FPS. Runs used different routes, cameras and combat;
even equal draw counts do not imply identical shaders or geometry. Timings are
frame-weighted CPU wall scopes, including waits/preemption. Nested times must
not be added to their parents. Capture replay GPU timings exclude game CPU
work and much of the live scheduling cost.

The evidence is the development run summaries, capture comparisons and tester
feedback. Raw logs, captures, personal launchers and imported patch bundles
remain local. Where no comparable measurement survives, no isolated gain is
claimed. The source reflects the final retained behavior, not every intermediate
candidate below.

## Bring-up and validation costs — 22 October

| Test | Change or investigation | Observed result / decision |
|---|---|---|
| E006 | First deko3d startup and filesystem investigation. | Startup failed. Missing optional library-directory messages alone did not establish the crash cause. |
| E008 | Startup/pipeline follow-up. | Still crashed; no performance conclusion. |
| S040–041 | Compare physical-console faults with emulator startup. | Hardware fault information informed bring-up; emulator acceptance was not proof of valid hardware commands. |
| E009 | Heap/allocation and startup follow-up. | Reached intros, then froze with stuttering audio. |
| E011 | Texture-copy byte layout and compute-queue investigation. | Intros and menus ran; text/images were corrupted and level loading hung. Corrected byte-stride handling and compute support. |
| E012 | First gameplay rendering checks. | Initially black world/HUD, later visible 3D. Gameplay ran but sustained performance was around 2 FPS. |
| S044, S046 | Diagnose startup GPU faults on physical Switch. | Hardware still failed although the emulator could run; diagnostic commands isolated the hardware-only failure. |
| S047 and follow-up | Restrict the sample mask to valid hardware bits. | Physical Switch booted and gameplay worked. Its low FPS remained a separate problem. |
| E017 | Disk shader cache, exact-variant warmup and optional PC precompilation. | Steady slow intervals had zero UAM compilations. Caching worked; the sustained 2 FPS was not explained by guest shader compilation. |
| E020 | Add render-phase, validation and upload measurements. | Full resource checking/hashing was a major CPU cost; optimize this before assuming a slow shader. |
| E021 | Bounded validation memoization. | A steady interval reached about 4.4 swaps/s; full checking still read about 665 MiB/frame. |
| E022 | Persistent geometry with sampled checks and training. | Regression: one guard miss disabled the broad fast path. A steady interval fell to about 1.8 swaps/s and roughly 2 GiB of full checks/frame. |
| E023 | Localize strict fallback to affected byte ranges. | Global caches stayed active. Tester reported around 10 FPS initially, falling during shooting; dynamic geometry became the next target. |
| E024 | Reuse dynamic geometry uploads. | Regression in the tested path; window identity and repeated copying needed correction. This result was not accepted as an optimization. |
| E025 | Stable geometry windows and dynamic reuse fix. | Tester reported recovery; retained the corrected identity/lifetime rules. No isolated FPS gain assigned. |
| E026 | Indexed cache lookup, reuse index uploads and constants. | Caches worked; selected gameplay still spent 116.63 ms/frame in Packet, 35.72 ms on indices and 27.74 ms on constants. |
| E027 | Short launch of the next candidate. | Not enough gameplay for the main timing comparison; use E028. |
| E028 | Cache CPU index conversions and reduce repeated preparation. | Versus E026: index conversion 17.78 → 0.15 ms/frame; indices 35.72 → 18.62 ms. Draw load rose from 1398 to 1647/frame; GPU uploads still cost time. |
| E029 | GPU index residency, constant deltas and texture binding reuse. | Tester reported a smaller improvement; caches retained. Remaining command/constant work motivated the next change. |
| E030 | Reuse constant slots and skip redundant draw command groups. | Tester reported further improvement; no isolated FPS multiplier. |
| E032 / S056 | Share geometry across work slots, share compatible constant layouts and reuse complete texture sets. | Performance improved, including on Switch. Eden intermittently displayed a shifted/dark image; tester did not see that symptom on Switch. |
| E033 | Correct the storage-buffer descriptor extent. | Tester confirmed the image was fixed. Cache counters remained active. Packet time was worse in this different, heavier run (54.20 → 118.36 ms); this was not evidence that caches should be removed. |
| E034 | First 16-bit index path, deferred full clears and compressed 3D copies. | Large improvement reported with no visible defects: about 25 FPS initially, around 10 in combat. Selected Packet 118.36 → 58.43 ms; fence 8.45 → 0.67 ms. This bundled change does not isolate compression alone. |
| E035 | Finer image barriers and deferred rectangular/masked clears. | Full barriers 75.82 → 15.08/frame. Clear commands rose 15.79 → 18.45 because regions fragmented. Packet 58.43 → 55.58 ms with fewer draws; no direct vertex-upload speedup established. |

## Uploads, cache dependencies and CPU work — 23 October

| Test | Change or investigation | Observed result / decision |
|---|---|---|
| E036 | Retain initial constant storage and coalesce clear regions. | Constants 11.19 → 3.96 ms/frame; initial CPU writes rounded to zero. Clears 18.45 → 16.21/frame. Vertex copying became dominant. |
| E037 | Narrow vertex windows and batch small index uploads. | Vertex bytes 1.240 → 0.727 MiB/frame, but vertex copy time stayed about 14.51 ms. Index upload 4.00 → 1.80 ms. Tester moved through more geometry. |
| E038 | Batch small vertex writes, cache validated index bounds and expand compact 16-bit indices. | Complete vertex copy work 14.51 → 8.77 ms. Same-session payloads required 73.6% fewer physical writes. Index upload rose 1.80 → 2.69 ms. Multi-level testing also produced black-world/HUD output and unmapped-memory errors; not proven to be out of memory. |
| E039 | Align/directly write indices and batch newly admitted resident blocks. | Index upload 2.69 → 1.48 ms; physical index writes 34.66 → 15.21/frame; Packet 46.73 → 44.92 ms with similar draw counts. |
| E040 | Sequential NEON vertex writes and resident upload batches. | Complete vertex writes 9.77 → 8.39 ms despite more draws/bytes; index uploads became relatively expensive again (1.48 → 2.96 ms). |
| E041 | Order and combine resident index writes. | Index upload 2.96 → 0.43 ms; physical writes 16.37 → 4.43/frame. Aggregate Packet 49.52 → 42.10 ms, but draws also fell about 20%. Do not attribute that full difference to the patch. |
| E042 | Deferred color-frontbuffer candidate and bounded game CPU adapter. | Different workload: Packet 42.10 → 48.06 ms while draws rose about 15%. No isolated CPU-adapter or presentation gain established. Deferred-frame work was not used as a reason to remove conservative preservation. |
| E043 | NFSMW comparison: fast geometry lookup, NEON comparisons, constant and texture preparation. | Tester reported improvement. Still expensive: dynamic uploads, constants and texture lookup; continue by measuring those paths. |
| E044 | Fast local vertex lookup, draw-setup cache, constant/version tracking and per-image dependencies. | Draw setup 1.70 → 0.65 ms; setup cache hit about 76%. Texture hashing grew 0.77 → 3.18 ms, so total Packet stayed about 48.5 ms. |
| E045 | Scheduled texture checks and fewer geometry checks per work. | Texture hash in heavy intervals 5.05 → 0.44 ms. But dynamic reuse fell to zero: dynamic copies 0.33 → 7.69 MiB/frame, all vertex copy work 15.65 → 30.39 ms. Keep the texture change; reject the geometry tradeoff. |
| E046 | Short launch after restoring geometry validation. | Menu/loading only; gameplay comparison uses E047. |
| E047 | Restore per-use checks for volatile geometry while retaining scheduled texture checks. | Heavy-scene dynamic reuse returned to 95.94%; dynamic copy volume 7.69 → 0.65 MiB/frame. Packet 84.87 → 77.37 ms at about 2700 draws/frame. |
| E048 | Exact dynamic windows, independent local cache, RAM/upload timing probes. | Heavy vertex copy work 19.17 → 9.04 ms, bytes 1.559 → 0.527 MiB. But invalidations rose 17 → 1331/frame and hashing 4.26 → 7.77 ms: ownership and validation epochs were coupled too broadly. |
| E049 | Separate vertex ownership changes from global content invalidation. | Identified the remaining broad invalidation: 94.56% of global requests in a busy interval came from 12-byte extent-result writes. |
| E050 | Invalidate only resources intersecting each 12-byte extent write. | Extent writes no longer trigger global invalidation; exact overlap queries and bounded history replace blanket expiry. Other write/wait/guard boundaries remain. |
| Subsequent imported candidates, before P058 | Sequential resident allocation, bounded extent-journal eviction, direct reuse of contiguous index snapshots, command-based geometry uploads and CPU/packet hot-path work. | These changes are present in the retained source. Complete per-build result correspondence for numbers 051–057 was not preserved in the local summaries; do not invent separate FPS gains for those numbers. By P058, measured CPU vertex write/command preparation was about 0.14 ms in the heavy selection, with GPU execution measured separately. |
| P058 / E058 | Visibility-probe handling and counters. | No speedup: VIZ, ZPD, query-enable and discarded-probe counts were zero in 3244 frames. This did not establish a missing occlusion-culling speedup for that scene. |
| P059 / E059 | Reuse prepared texture answers, group constant versions/packing, reduce repeated idle queue work. | At roughly 3000 draws/frame, textures 12.67 → 8.43 ms; constant packing 2.01 → 1.63 ms. FPS remained about 13; fence wait 14.21 → 17.89 ms. Similar draw counts still had different geometry upload volume. |
| P060 / E061 | Isolate constant storage from geometry buffers. | Heavy captures: buffer copies 2885 → 21, render passes 2933 → 173, barriers 8719 → 231, despite more draws. Removed the repeated constant-page copy pattern. Live queue waits still reached 29–36 ms. |
| P061 / E064 | Bound partial queue submissions, narrow constant delta scans, enlarge texture-set cache and reuse validated bindings. | Queue waits and preparation decreased; see the final comparison below. Caches stayed active and the repeated constant copies did not return. |

Early startup numbers without an identifiable implementation/result pair and
short intervening launches are not promoted to independent benchmarks. S###
hardware logs must not be confused with E### logs with the same number.

## Latest comparison: P060 → P061

Heavy intervals, weighted by guest swaps. P060: 194 frames; P061: 443 frames.
The newer selection has about 20% fewer draws, so the FPS difference is not a
controlled measurement of the patch alone.

| Metric | P060 | P061 |
|---|---:|---:|
| Actual draws/frame | 3759 | 2993 |
| Eden FPS from swaps | 9.70 | 14.75 |
| Packet wall time, including waits (ms/frame) | 94.23 | 64.07 |
| Work-slot fence wait (ms/frame) | 32.06 | 11.44 |
| Constants, inclusive (ms/frame) | 10.26 | 7.28 |
| Constant delta scan, nested (ms/frame) | 3.35 | 1.91 |
| Textures, inclusive (ms/frame) | 9.19 | 6.91 |
| Guest texture lookup/preparation, nested (ms/frame) | 4.28 | 2.10 |
| Resolve, inclusive CPU scope (ms/frame) | 6.09 | 7.07 |
| Vertices, inclusive (ms/frame) | 5.96 | 4.88 |
| Indices, inclusive (ms/frame) | 4.73 | 3.62 |
| Texture binding cache hit rate | 75.37% | 99.26% |

Per 1000 draws, constant delta scanning decreased about 28%, guest texture
lookup about 38%, constants overall about 11%, and textures overall about 6%.
Constant packing did not improve after this normalization. A closer-load
comparison (2182 → 2393 draws/frame) also reduced fence wait (15.39 → 3.78 ms)
and constants (6.19 → 5.76 ms), but textures increased with more uploads.

Nine new captures contained 8–10 buffer-copy commands; the previous heavy
captures had 21. Global geometry/texture caches were not disabled. Four ranges
entered local strict validation after guard misses; other ranges retained their
fast paths. Heavy intervals compiled zero UAM shaders.

## Retained behavior and next step

- Persistent geometry, local dynamic reuse, NEON upload batching and compact
  indices remain. Correctness boundaries and GPU lifetime tracking remain.
- Textures use dependency-aware reuse and bounded validation. Sampling is an
  experimental performance/correctness tradeoff, not complete write tracking.
- Constants use retained storage and versioned packing/delta updates.
- Image dependencies, deferred clears and compressed 3D copies remain.
- The CPU adapter is limited to verified guest regions and bounded waits;
  it does not remove process-wide synchronization indiscriminately.

The next planned stage is **optimization of the game's CPU work**, with separate
measurements of guest execution, waits, render preparation and GPU completion.
Remaining queue waits, texture/constant work and resolve costs need attribution;
the evidence does not identify one uniquely slow car shader. Vulkan remains
the more stable performance option in the user's current hardware comparison.

Implementation references:
[NFSMW deko3d](https://github.com/StevensND/nfsmw-nx/tree/266f943263993148e49e968208a4f784ddd8d88d),
[Dante's Inferno NX](https://github.com/VasilkovND/dantes-inferno-nx/tree/01f513cdecded5f904e6f72a9ad655f6762c7554),
[ReXGlue NX renderer](https://github.com/nx-mod/rexglue-sdk-nx/tree/f961303fe3c6e3a90905b379ff4bf3de9a35a757).
Their game-specific performance numbers are not F.E.A.R. measurements.
