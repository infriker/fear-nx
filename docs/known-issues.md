# deko3d known issues

- Current Switch testing reports **20–30 FPS with overclocking**, with frequent
  drops. Vulkan in `main` is currently more stable. Clock values and matched
  scene measurements are not available for that hardware report.
- First use of an unknown shader variant can stall. Caches and PC preparation
  reduce repeated compilation, not the CPU and synchronization cost of a warmed scene.
- Gameplay testing has not covered every mission and resource combination.
  Multi-level testing once reported invalid memory accesses with a black world
  and visible HUD; that report does not establish an out-of-memory diagnosis
  or a general fix. Preserve a reproducible save and relevant log when reporting.
- Only the supported Xbox 360 executable is accepted; use title takeover mode.

Vulkan's occasional incomplete/half-screen rendering and decal artifacts have
unidentified triggers. Those reports are tracked in the
[main branch](https://github.com/infriker/fear-nx/blob/main/docs/known-issues.md),
not asserted to be the cause of every deko3d issue.

Report the branch/build, console model, clocks, configuration, level/checkpoint
and reproduction steps in a [GitHub issue](https://github.com/infriker/fear-nx/issues).
Remove personal information from logs and do not upload original game assets.
