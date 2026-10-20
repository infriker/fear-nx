# Patches against mesa-switch

> Snapshot notice (2026-10-20): the runtime observations below are historical.
> See `docs/status.md` at the repository root for the latest user-tested Switch
> result. Complete final dependency changes, including unfinished Eden
> diagnostics, are preserved in `patches/checkpoint-20261020/` as alternative
> patches against their exact bases. Do not apply both patch workflows.

The Switch build links NVK from https://github.com/danfromtico/mesa-switch at
commit `d4a00ea0ab3` (Mesa 26.2.3). These patches apply on top of it.

- `0001-nvk-switch-resolve-set-ubo-descriptors.patch`: before Turing, NVK binds
  a uniform buffer whose descriptor lives in a regular descriptor set through
  an indirect GPFIFO segment, which costs two GPFIFO entries and a
  no-prefetch front-end stall per binding. At about 1,200 draws a frame in
  Free Skate that nearly filled the 2,048-entry GPFIFO: every kickoff stalled
  on "native submission pressure" and the device was eventually lost. The
  patch reads the descriptor from the set's CPU mapping at bind time, which
  Vulkan permits for sets without update-after-bind. On hardware it removed
  the pressure warnings and the crash, and raised Free Skate from 2-3 to 7-8
  fps.
- `0002-nak-gm20b-latency-and-flattening.patch`: two NAK scheduling changes
  for pre-Volta GPUs taken from nfsmw-nx's Mesa patch (MIT, see
  https://github.com/StevensND/nfsmw-nx/tree/main/mesa): texture and global
  memory latency estimated at 200 cycles instead of 32, and
  `nir_opt_peephole_select` limit 8 instead of 0. It also adds a Switch
  revision bit to the compiler flags so pipeline caches built by an earlier
  driver are recompiled rather than reused.
- `0003-nvk-switch-direct-draws-and-memory-query.patch`: two CPU costs of
  the driver on the Switch. Before Pascal every draw goes through a draw
  macro that writes four draw parameters into a constant buffer; when they
  are all zero and the previous draw of the command buffer left them zero,
  the draw is issued directly (`BEGIN`, draw, `END`), as the nouveau GL
  driver does on these GPUs. And `os_get_available_system_memory` walked the
  whole newlib heap through `mallinfo` under its lock each time the memory
  allocator asked for its budget, which it does every few dozen allocations;
  the value is now reused for up to 100 ms.
- `0004-switch-build-script.patch`: the script that builds the driver
  archives this port links (`build-switch-p8.sh`, run from the mesa-switch
  checkout), the Meson native file it uses, and a fix to the Rust compiler
  wrapper: procedural macros run inside the compiler on the build machine, so
  they must never be cross-compiled for the Switch.
- `0005-horizon-bind-3d-before-builtin-commands.patch`: local `fear-nx` patch,
  not part of Project8. It adds a one-time `SET_OBJECT` binding of Maxwell 3D
  (`0xB197`) to subchannel 0 before NVK sends its built-in cache-acquire/fence
  commands. Eden did not infer this initial binding from `AllocObjCtx` and crashed
  on method `0x4A2`; the patch targets that null subchannel. The four Project8
  patches above remain unchanged. Test 8 progressed into guest GPU ring and
  render-target setup, but the host emulator crashed before a menu frame was
  verified; see the Runtime validation section below.

- `0006-bind-requested-engines-before-first-submit.patch`: local `fear-nx` patch.
  Horizon receives NVK's requested engine mask and packs `SET_OBJECT` bindings
  for 3D and requested engines into one lazy bootstrap GPFIFO entry. It binds
  GM20B copy class `0xB0B5` on subchannel 4 (NVK's `NV90B5` mapping), plus other
  requested classes on their NVK subchannels. Test 9 recorded an unbound
  subchannel 4 at method `0x1C0` (`NVA0B5_SET_REMAP_CONST_A`), which is the
  evidence this patch addresses. Mesa compiled and staged 1,557 AArch64 objects,
  but the NRO has not yet been retested with this patch; runtime behavior and
  menu rendering remain unverified.

## Runtime validation

On 2026-10-20, the full-game Test 8 run progressed past the startup probe in
the local patched Eden build. The original guest main thread started; NVK's
renderer initialized its GPU ring, the game catalog was read, and several
Xenos render targets plus a 1280x720 frontbuffer were created. Eden then
crashed on the host with an access violation at
`eden-cli.exe+0x6c6e9d`. The crash cause is still under investigation, and no
menu frame or controller behavior was verified. This is beyond the earlier
Test 7 stop at exception delivery, but is not proof of a rendered game frame.

Test 9 diagnostics captured the pre-fix copy-engine failure as
`subchannel=4 method=0x1c0 argument=0 count=1 dma_get=0x16eea90000 word_offset=0x28`.
The patch adds the requested `0xB0B5` binding before the first client packet;
this diagnostic alone does not show that the fix works.

The asset-free probe explains the exception-path result: stock Eden still
delivered zero handler entries, while the patched HBL64 test build passed its
trap-resume fallback and custom DataAbort test. Its mode-2 FP-state trial
failed (FPSR expected `0x1`, observed `0x11`), so mode 2 is not claimed to
work. The custom patch remains limited: nonzero `ReturnFromException` replay
and KDebug exception replay are unsupported. No physical Switch run has been
performed.

Apply all six patches in order with `git apply` on a checkout of that commit, then run
`./build-switch-p8.sh`. It builds the driver's static libraries in
`builddir-switch/`. Merge them into one `lib/libvulkan.a`, check and index it
with `tools/switch/stage-mesa-archive.py`, and put `libnak_rs.a` next to it;
`REXGLUE_SWITCH_NVK_SDK` points at the folder that holds `lib/`.
