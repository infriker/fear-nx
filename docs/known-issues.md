# Known issues

The user confirmed that the reported weapon, decal and screen-flicker defects
are resolved in the current release line. Weapon displays remain enabled.
Version 0.1.4 promotes the FX/particle profile accepted in the preceding
0.1.3-fx3 Switch build on 2026-10-25; bloom retains its original game behavior.

- A temporary half-screen scene may appear at the start of level loading.
  This is the remaining reported visual defect; its cause is still under investigation.
- The target is 30 FPS. Earlier hardware tests at a reported 460 MHz GPU reached
  about 30 FPS in most gameplay, 25–27 in heavy firefights, and about 17 in some
  demanding areas. These are observations, not a full-campaign benchmark.
- Asynchronous pipeline compilation can briefly omit newly encountered draws.
  This expected compilation behavior is separate from the level-loading issue.
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
