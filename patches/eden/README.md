# Eden compatibility patches

> Snapshot notice (2026-10-20): the runtime observations below are historical.
> See `docs/status.md` at the repository root for the latest user-tested Switch
> result. Complete final dependency changes, including unfinished Eden
> diagnostics, are preserved in `patches/checkpoint-20261020/` as alternative
> patches against their exact bases. Do not apply both patch workflows.

This ordered local patch series targets the exact Eden `v0.2.1` base commit
`58c1e20ee58efa3900ba616207d460886214480b`. It is an experimental test
branch for the F.E.A.R. NX port, not an upstream patch set or a general Eden
release. Apply the entries in `series` order to a clean checkout of that base.

The series contains no game executable or game data. Keep the patched source,
build directory, portable user profile, and logs separate from a normal Eden
installation. The [parameterized Windows build guide](../../docs/eden-local-build.md)
shows how to build the isolated `eden-cli` target.

## Patch contents and limits

- `0001-horizon-homebrew-user-exceptions.patch` adds a narrow AArch64 HBL user
  exception path so the compatibility probe can exercise traps and memory
  faults. The local custom build passed trap/resume, 32-vector, FPCR/FPSR, and
  DataAbort checks and exited with status 0. It is not a complete Horizon
  exception ABI: only a handled exception with a zero `ReturnFromException`
  result resumes; nonzero results and KDebug exception replay remain
  unsupported. The separate stock Eden run still observed no handler entries.
  Mode-2 FP-state testing remains failing (FPSR expected `0x1`, observed
  `0x11`).
- `0002-fix-macro-jit-alu-and-immediates.patch` corrects x64 GPU-macro JIT
  handling of zero-register ALU operands, subtraction borrow state, immediate
  values 0/1, and redundant method-move elision. In Test16, the macro-JIT run
  matched interpreter startup progress through Sierra (~10 s), Day1 (~14 s),
  Monolith (~20 s), and `Menu.vid` background data loading (~30 s). No
  MacroJIT/ARGS/CB/InvalidInsn hits were recorded. This does not yet produce a
  visible menu.
- `0003-support-nvk-maxwell-shaders.patch` adds Maxwell IMUL and PRMT shader
  translation and lets the cache analyze NVK shaders without the proprietary
  driver's terminal self-branch. In shader test 13, the earlier IMUL and shader
  header errors were no longer reported. The PRMT implementation has not yet
  been verified at runtime. Test16 still reports a SUST `Non-full mask` error
  for compute pipelines `0x0905eecf7ad66533` and `0xbec2c0422b59c4e6`.

Through Test16, a visible game menu or rendered frame has **not** been
verified. The latest run reached background data loading for `Menu.vid`, but
the screen remained without a visible menu. The remaining compute-shader error
is under investigation. These tests show incremental compatibility progress,
not playability or correct rendering.

## Apply to a clean checkout

From the `fear-nx` project root, first check out the pinned base, then apply the
patches in order from the Eden directory:

```powershell
git -C third_party/eden checkout --detach 58c1e20ee58efa3900ba616207d460886214480b
git -C third_party/eden apply --check ../../patches/eden/0001-horizon-homebrew-user-exceptions.patch
git -C third_party/eden apply ../../patches/eden/0001-horizon-homebrew-user-exceptions.patch
git -C third_party/eden apply --check ../../patches/eden/0002-fix-macro-jit-alu-and-immediates.patch
git -C third_party/eden apply ../../patches/eden/0002-fix-macro-jit-alu-and-immediates.patch
git -C third_party/eden apply --check ../../patches/eden/0003-support-nvk-maxwell-shaders.patch
git -C third_party/eden apply ../../patches/eden/0003-support-nvk-maxwell-shaders.patch
```

To run the intended exception test, launch only the Horizon compatibility probe
NRO and inspect `sdmc:/switch/fear-nx/horizon-probe.log`. A successful build
alone does not prove exception delivery, menu rendering, or gameplay.