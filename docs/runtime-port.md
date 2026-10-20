# ReXGlue 0.10 Switch runtime audit for F.E.A.R.

This is a compatibility audit of the pinned ReXGlue 0.10.0 Switch port and its applied Project 8 patch series. It records which behaviors are generic Horizon/runtime support, which are title-specific diagnostics, and the current runtime limits for F.E.A.R.

## Current validation — 2026-10-20

A user-provided Eden screenshot confirms the original menu renders. Eden reports about 30 swaps per second, although displayed frames are intermittent. The user tested on a physical Switch Lite: the menu appeared and controller navigation reached Chapter 01 loading, but the campaign load fully froze. Gameplay is not confirmed. Tested NRO SHA-256: `BE081311DB314FF16E27A682250EA4E28395C82E837986FE62C1046CB9C52FA1`.

The latest Switch logs show 14 failed fixed-memory commits with result 0x0000CE01 (description 103, OutOfResource). Across those samples, backing use rose from 365 to 371 MiB and mapped usage including aliases rose from 418 to 423 MiB. These are allocation failures; they do not prove system RAM or a process quota was exhausted. The sample still reports 5 shaders and 4 pipelines, so a shader-compilation stall is not established. The earlier 0/0 MB resource-limit display came from an incorrect query argument. SDK patch 0021 now queries the current process resource-limit handle with INVALID_HANDLE and logs query return codes plus process and system memory byte values. It compiled and linked in the diagnostic NRO; device behavior remains untested.

Test 17's partial SUST change removed the two observed compute-pipeline “Non-full mask” failures; build SHA-256: `D79F14024483F361856083E36FB2163C1F101A9AF9C5E5CF461A19631D1C4C19`. A separate diagnostic NRO has compiled and linked, but has not been validated at runtime. The earlier Test 8 host crash is retained as historical diagnostic evidence and does not describe the current result.

## Game-specific code found

The only hardcoded Project 8 guest address found in the audited runtime path is in `third_party/rexglue-sdk/src/core/exception_handler_switch.cpp`, inside `EmulateViaShadow`. It matches both instruction word `0xB956BC24` and guest address `guest_base + 0x8201D6BC`, then calls `RecordImageRead` before and after the normal emulated access. It only records Project 8 image-read telemetry; it does not replace the load, alter the guest registers, or affect the access result. Remove it or put it behind a default-off diagnostic in a separate, reproducible SDK patch before treating the SDK fork as title-neutral. Do not change the generic access emulation around it.

The Project 8 annotations in `xam_content.cpp` are debug-level instrumentation; they do not change XAM content enumeration. `kernel_state.cpp` adds similar debug traces, and also resets an overlapped event before queuing a deferred operation. That reset is a real behavior change, but it implements the Windows contract that a pending overlapped operation's event is nonsignalled until completion; it is generic rather than a Project 8 address hook. Keep it for now and check FEAR's event reuse during I/O testing. The read-only trace in `xfile.cpp` only logs directory-query state. `xma_context.cpp` lowers one repetitive “no bits to copy” message to debug logging; it does not change XMA decoding or output samples.

The delete-on-close changes in `xboxkrnl_io.cpp` and `xfile.cpp` are generic Windows file-handle semantics: both `FILE_DELETE_ON_CLOSE` at open and `NtSetInformationFile(FileDispositionInformation)` set the VFS deletion flag, and the file entry is removed after its host handle closes. Keep this behavior unless a concrete FEAR trace shows a mismatch.

## Switch behavior that should be treated as a compatibility variable

`switch_poll_sleep_us` defaults to 100. On Horizon, after 32 consecutive zero-timeout polls close together, a guest thread sleeps for that duration instead of only yielding. This was measured against Project 8's spin loop and can change polling latency for another title. The first-run profile sets it to `0`, which keeps the ordinary yield behavior; tune only after FEAR is running and a real CPU-pressure issue is measured.

`audio_switch_bomba` defaults to `true`. That selects a 187.5 Hz guest audio-frame request pump with a queued-frame target, based on a different game's callback behavior. It can change guest mixer scheduling. The first-run profile sets it to `false`, so frame requests follow the SDK's audio-buffer consumption path while FEAR's XMA/mixer behavior is unknown. `audio_switch_tramas_en_cola` is not material when that pump is off.

`switch_unwatch_hot_reads` is a generic watched-page optimization and defaults to `false`; leave it off for compatibility bring-up. The default Switch performance profile requests 460 MHz GPU; the F.E.A.R. profile explicitly uses `switch.gpu_mhz = 0` (system default), disables the RAM 1600 MHz opt-in, and leaves NVMap overrides/A-B profiling at zero. No Project 8 guest hooks or address-based game overrides are enabled by the profile.

## Generic Horizon startup and input path

Use the SDK's normal `REX_DEFINE_APP` entry and `rexglue_switch_configure_target`/`rexglue_switch_add_nro` CMake helpers. `windowed_app_main_switch.cpp` owns libnx startup, app lifecycle, and the clean application exit path; the title should not add a second `main` or duplicate libnx setup.

The app's Switch path hook changes the package defaults to sibling directories: `game/` for `default.xex` and game assets, `userdata/` for saves/user files, `update/` for update data, `cache/` for shader/runtime cache, and `metadata/` for metadata. These are beside the NRO, so game files and writable runtime state stay outside the NRO. `EnsureDirectory` creates writable roots after the final paths are selected.

The SDK calls `OnConfigurePaths` inside `SetupEnvironment` before loading the TOML, then calls `OnFinalizePaths` later from `OnInitialize`, after `LoadConfig` has run. The SDK itself does not re-resolve path cvars after loading TOML: its default `OnFinalizePaths` simply returns the earlier `PathConfig`. The F.E.A.R. app therefore reapplies nonempty `game_data_root`, `user_data_root`, `update_data_root`, `cache_root`, and `metadata_root` cvars in its `OnFinalizePaths` override, then creates the chosen writable directories and validates `game/default.xex`. A path cvar in TOML must be at the TOML root (for example, the key `user_data_root`), because `[runtime] user_data_root` flattens to `runtime_user_data_root`, which is not a registered cvar. Command-line path overrides are already present before `OnConfigurePaths` and remain honored as well.

SDK patch 19 validates the loader-provided process handle and, when it is absent or invalid, obtains a real handle to the current process through a local HIPC copy-handle request. The same cached handle is used by guest-memory mappings and exception resume stubs; the runtime never passes a pseudo-handle to process-memory mapping. The startup exception regression probe now counts actual entries into the libnx handler and requires one entry per UDF trial before checking saved registers.

The TOML loader recursively flattens table/key paths with underscores, then looks up the exact cvar registry name. Cvar categories such as `Kernel` are descriptive labels, not TOML namespaces; only the cvar identifier controls the key. Cvar names are exact, lowercase registry keys. For example, `[Audio] audio_switch_bomba = false` would form the unknown key `Audio_audio_switch_bomba`.

| TOML setting | Flattened registry name | Definition |
| --- | --- | --- |
| `[nx] renderer` | `nx_renderer` | `src/native/native_system.cpp` |
| `[input] xbox_layout` | `input_xbox_layout` | SDK `src/input/switch/switch_input_driver.cpp` |
| `[audio] switch_bomba` | `audio_switch_bomba` | SDK `src/audio/switch/switch_audio_system.cpp` |
| `[switch] poll_sleep_us` | `switch_poll_sleep_us` | SDK `src/system/xobject.cpp` |
| `[switch] gpu_mhz` | `switch_gpu_mhz` | SDK `src/ui/rex_app.cpp` |
| `[switch] allow_ram_1600` | `switch_allow_ram_1600` | SDK `src/ui/rex_app.cpp` |
| `[switch] nvmap_mb` | `switch_nvmap_mb` | SDK `src/ui/rex_app.cpp` |
| `[switch] ab_lap_after_s` | `switch_ab_lap_after_s` | SDK `src/ui/rex_app.cpp` |
| `[log] level` | `log_level` | SDK `src/core/logging.cpp` |
| `[log] flush_level` | `log_flush_level` | SDK `src/core/logging.cpp` |
| `[log] flush_interval` | `log_flush_interval` | SDK `src/core/logging.cpp` |

Run `scripts/validate-config.ps1` to flatten the shipped profile and check every key against `REXCVAR_DEFINE_*` identifiers found in the app and pinned SDK sources. The validator deliberately supports the simple table/scalar syntax used by this profile; it does not attempt to be a general TOML linter.

The SDK's `SwitchInputDriver` is selected by `InputSystem::CreateDefaultInputSystem()` on Switch and implements the XamInput path. It translates Joy-Con/Pro Controller buttons and sticks into the XInput state consumed by the recompiled game. `input.xbox_layout = false` means Nintendo button lettering; `true` means Xbox button positions. The user confirmed controller navigation on a physical Switch Lite reached Chapter 01 loading. The campaign then froze, so input beyond the menu and gameplay remain unverified.

The Switch startup patch also installs a Horizon application title type, a declared 1 GiB newlib heap request, the Switch audio/window lifecycle, custom terminate logging, and per-thread exception handling. libnx initialization checks for a homebrew-loader heap override first and uses its supplied address and size, so the declaration is ignored when an override is present; the effective heap size for this launch has not been measured. These settings are platform-wide resource/lifecycle decisions, not FEAR guest-address patches. The 460 MHz GPU profile is an SDK default borrowed from another port; the supplied F.E.A.R. config opts out pending measured need.

## ARM64 memory and exception risks

The guest is translated to host ARM64 code and uses the SDK's PPC-generated runtime; this is not x86-on-ARM instruction emulation. The Switch exception path must preserve the host AArch64 integer, SIMD, FPCR, and FPSR state before it can resume a faulting generated instruction. The patched exception entry/return path saves and restores that state and maintains multiple per-thread fault slots.

When a guest-memory mapping fault reaches the Switch handler, the current access decoder emulates supported integer/NEON loads and stores through the shadow mapping. An unrecognized instruction, an access crossing shadow-backed chunks, or a failure to resolve backing cannot be resumed through that path. Atomics/exclusive accesses and some vector/register-list forms are known coverage risks. Faults are captured to the Switch crash log; the Horizon adapter does not provide Windows-style catchable guest SEH, so faults that would rely on guest SEH remain a runtime limitation. Eden must be used to exercise the actual generated ARM64 code and Vulkan path; static compilation alone does not prove these recovery paths.

## First-run profile

`release/fear-nx.toml` selects the native renderer, Nintendo-letter input prompts, legacy audio request pacing, zero-timeout yield behavior, the system default GPU clock, and info-level logging flushed once per second for the first diagnostic run. It is a conservative compatibility baseline, not a performance-tuned release profile.

`scripts/stage-eden.ps1` infers `RepoRoot` from the script location and requires explicit `-EdenRoot` and `-GameSource` arguments. With `-CopyGameData`, it copies the validated extracted game tree into a new package directory; it creates `user/sdmc/switch/fear-nx` as a junction only when that title path is absent. Existing conflicting paths are preserved and cause the script to stop. Example: `./scripts/stage-eden.ps1 -EdenRoot 'X:\Eden' -GameSource 'X:\F.E.A.R.\assets' -CopyGameData`. `scripts/run-eden.ps1` requires `-EdenRoot`, derives Eden's per-title SD path from it, verifies the NRO, config, and fixed `default.xex` SHA-256, and then opens the staged NRO; example: `./scripts/run-eden.ps1 -EdenRoot 'X:\Eden'`.

## Verification status

The asset-free Horizon compatibility probe passed in the locally patched Eden build using the trap-resume fallback; its mode-2 FP-state trial failed. This validates the probe path only.

The full game now renders its original menu in Eden, and a physical Switch Lite test confirmed menu navigation to Chapter 01 loading. That campaign load froze, so gameplay is not confirmed. Eden displayed frames intermittently despite about 30 swaps per second. Test 17's partial SUST change removed the two observed compute-pipeline mask failures. The tested NRO SHA-256 is `BE081311DB314FF16E27A682250EA4E28395C82E837986FE62C1046CB9C52FA1`; the Test 17 build SHA-256 is `D79F14024483F361856083E36FB2163C1F101A9AF9C5E5CF461A19631D1C4C19`.

The earlier Test 8 Eden host crash is historical and superseded by later menu-rendering runs. A separate diagnostic NRO has compiled and linked, but has not been validated at runtime. Stable presentation, campaign loading, and gameplay remain unresolved.