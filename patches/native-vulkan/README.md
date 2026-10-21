# Native Vulkan SDK snapshot — 2026-10-21

This complete patch restores the exact ReXGlue 0.10.0 source used by both
Windows and Switch native renderers. It includes the earlier Horizon patches,
the existing runtime corrections, Windows libmspack source-path compatibility,
and optional VK_EXT_depth_bias_control feature negotiation and command loading.
The SDK base remains c94f5ebdcb3c9d1a460ca48e04f9758448f8d518.

Use `scripts/bootstrap-native-sdk.ps1` for this snapshot. It is an alternative
to the numbered SDK patch series and the 2026-10-20 snapshot: do not apply them
on top of one another. Existing modified SDKs are only accepted when their
source tree and submodule revisions match; they are never reset or cleaned.

The manifest records the patch SHA-256, expected patched tree and recursive
submodule revisions. A temporary-index restore was compared with the actual
SDK working sources. Game inputs, generated game code and build outputs are
excluded. The runtime's real Git index was left untouched.
