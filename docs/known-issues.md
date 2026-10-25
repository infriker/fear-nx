# Known issues

The reported stale-texture artifact when selecting an affected weapon on
Switch is fixed in v0.1.3 by fresh vertex/index upload checks. The user confirmed
the preceding 0.1.2-vf1 build on Switch on 2026-10-25.
Weapon displays remain enabled. This confirmation does not establish that the
other symptoms below are resolved.

- Intermittent graphical defects remain: incomplete or half-screen rendering,
  especially around loading, and decal artifacts during shooting. **Their
  triggers and root causes have not yet been identified.** The current Vulkan
  fixes must not be read as confirmation that these symptoms are resolved.
- The target is 30 FPS. Earlier hardware tests at a reported 460 MHz GPU reached
  about 30 FPS in most gameplay, 25–27 in heavy firefights, and about 17 in some
  demanding areas. These are observations, not a full-campaign benchmark.
- Asynchronous pipeline compilation can briefly omit newly encountered draws.
  This is not an established explanation for the graphical defects above.
- Only the supported Xbox 360 executable revision can run. The PC release and
  expansion executables are unsupported.
- Use title takeover mode. Album/applet mode is unsupported.

Gyroscope aiming is implemented and enabled by default while aiming; see
[gyro settings](gyro-aiming.md). Vulkan currently has more stable performance
than the experimental deko3d branch. See [performance history](performance.md).

## Reporting a problem

Open a [GitHub issue](https://github.com/infriker/fear-nx/issues) with the branch,
version, console model, clock settings, level/checkpoint and reproduction steps.
For graphical defects, include a screenshot or short clip and whether it repeats
after loading the same save. Remove personal paths from log excerpts. Do not
attach original game data, disc images or `default.xex`.
