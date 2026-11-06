# deko3d known issues

- Switch FPS for the full-frame deko3d build still needs a matched hardware
  run. The earlier October build reached 20–30 FPS with overclocking and
  frequent drops. See the [test history](deko3d-history.md).
- First use of an unknown shader variant can stall. Caches and PC preparation
  reduce repeated compilation, not the CPU and synchronization cost of a warmed scene.
- Gameplay testing has not covered every mission and resource combination.
  Multi-level testing once reported invalid memory accesses with a black world
  and visible HUD; that report does not establish an out-of-memory diagnosis
  or a general fix. Preserve a reproducible save and relevant log when reporting.
- Only the supported Xbox 360 executable is accepted; use title takeover mode.

Report the branch/build, console model, clocks, configuration, level/checkpoint
and reproduction steps in a [GitHub issue](https://github.com/infriker/fear-nx/issues).
Remove personal information from logs and do not upload original game assets.
