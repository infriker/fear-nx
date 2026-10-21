# Native Vulkan SDK snapshot — 2026-10-21

This complete patch restores the exact ReXGlue 0.10.0 source used by both
Windows and Switch native renderers. It includes the earlier Horizon patches,
the existing runtime corrections, Windows libmspack source-path compatibility,
and optional VK_EXT_depth_bias_control feature negotiation and command loading.
The SDK base remains c94f5ebdcb3c9d1a460ca48e04f9758448f8d518.

Follow the [SDK setup commands](../../docs/building.md#1-prepare-the-sdk)
for this snapshot. It is an alternative to the numbered SDK patch series
and the 2026-10-20 snapshot: do not apply them on top of one another.
Use a new checkout and preserve any existing local SDK modifications.

The manifest records the patch SHA-256, expected patched tree and recursive
submodule revisions. A temporary-index restore was compared with the actual
SDK working sources. Game inputs, generated game code and build outputs are
excluded. The manifest records the complete patched source tree.
