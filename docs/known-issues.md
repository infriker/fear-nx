# Known issues

- The target is 30 FPS. Demanding scenes, especially slow motion, can fall to
  25–27 FPS with the GPU at 614 MHz.
- Asynchronous pipeline compilation can briefly skip newly encountered draws.
  Pipelines are cached for subsequent use. Changing renderer or driver versions
  may require new pipelines.
- Gyroscope aiming is not implemented. It is on the roadmap together with
  further performance work.
- Only the supported Xbox 360 executable revision can run. Other revisions,
  the PC release and expansion executables are not supported.
- The release targets physical Nintendo Switch hardware in title takeover
  mode. Applet mode and emulator compatibility are outside this release.

The menu and campaign are playable; this is not a claim that every mission,
cutscene or hardware configuration has been exhaustively tested.

## Reporting a problem

Open a [GitHub issue](https://github.com/infriker/fear-nx/issues) with the release
version, console model, clock settings, level/checkpoint and reproduction steps.
For a rendering issue, include a screenshot or short clip. Include relevant
log excerpts where useful, and remove personal paths or identifiers first.
Do not attach original game data, disc images or `default.xex`.
