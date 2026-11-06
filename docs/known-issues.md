# Known issues

Version 0.2.0 fixes the half-screen image at the start of level loading. The
earlier weapon, decal and screen-flicker fixes are included.

- Heavy firefights can reach 25 FPS at stock 1020 MHz CPU and 460 MHz GPU.
  The CPU is the main limit. See the [tested clock settings](performance.md#switch-performance-in-v020).
- Asynchronous pipeline compilation can briefly omit newly encountered draws.
- Only the supported Xbox 360 executable revision can run. The PC release and
  expansion executables are unsupported.
- Use title takeover mode. Album/applet mode is unsupported.

Gyroscope aiming is implemented and enabled by default during gameplay; see
[gyro settings](gyro-aiming.md). Vulkan currently has more stable performance
than the experimental deko3d branch. See [performance history](performance.md).

## Reporting a problem

Open a [GitHub issue](https://github.com/infriker/fear-nx/issues) with the branch,
version, console model, clock settings, level/checkpoint and reproduction steps.
For graphical defects, include a screenshot or short clip and whether it repeats
after loading the same save. Remove personal paths from log excerpts. Do not
attach original game data, disc images or `default.xex`.
