# Shared resolve resources experiment

This separate build tests writing validated F.E.A.R. tiled resolves directly into
one destination image for each guest surface. It does not call the legacy
`UpdateTiledResolve` segment compositor while `nx.native_shared_resolves` is true.
It still performs the resolves requested by the game from render targets into
texture memory. Render targets and sampled resolve images remain separate.

The verified 32-bit tiled layout maps the physical destination address and
window/scissor origin to a common base address and full pitch/height. Each write
updates only its destination rectangle, in command order. Overlapping pixels
therefore follow last-writer-wins semantics. The first write may come from either
part of the surface. Validated single-sample depth resolves use the same origin
mapping for float depth and the packed depth/stencil companion when requested.
Unsupported layouts use an independent resolve destination; they are not guessed
or assembled by the legacy compositor in this mode.

Offset texture reads may require a cropped sampling image, refreshed from the
common resource. This is a derived view of one resource, not a final frame made
by combining independent segment textures. This experiment is not a complete
replacement for general Xenos memory alias tracking across every pixel format.

## Hardware test

1. Keep your previous NRO and configuration for rollback.
2. Copy both files from the experimental ZIP's `switch/fear-nx/` directory to
   `sd:/switch/fear-nx/`, replacing `fear_nx.nro` and `fear-nx.toml`.
3. Keep existing game files, saves and caches. Launch through the same full-memory
   homebrew method used for the previous build.
4. Check the whole menu, campaign loading, the first rendered scene, and the
   same lighting problem while rotating the camera. Include a transition or a
   second level load to exercise destination address reuse.
5. The log identifies this path as `[native] Resolve resource mode:
   shared-direct-v1`; bounded `[native][shared] direct` records include root,
   alias and source/destination rectangles.

The bundled profile preserves the previous MSAA-off, depth24, anisotropy and
clock settings. No Eden-specific workaround, SDK update, driver update, or
occlusion change is part of this experiment. The existing composition code is
retained for comparison with `native_shared_resolves = false`; it is bypassed
in the supplied profile. Restore the previous package for an exact rollback.

## Validation scope

Host tests check tile addressing, destination bounds and write ordering using
the observed 1280x720 layout (768-pixel root write and 640-pixel write at x=640).
They do not prove GPU synchronization or correct gameplay rendering. A successful
NRO build likewise does not prove the lighting defect is fixed. Hardware testing
is intentionally left to the user; no game or emulator is launched for this task.
