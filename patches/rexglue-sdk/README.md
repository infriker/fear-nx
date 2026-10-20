# ReXGlue SDK patch set

The first 16 patches are copied unchanged from
[Project8Recomp/project8-sw](https://github.com/R-drg/project8-sw), commit
`3393f2615deb7e8ac11ce51d2cd0e9defded5669`, under
`patches/rexglue-sdk/`. They were authored for ReXGlue v0.10.0 at commit
`f5337cd`; the pinned SDK commit in this project is
`c94f5ebdcb3c9d1a460ca48e04f9758448f8d518`, verified by the maintainers to
have the same source tree.

`0017-fear-build-compat.patch` is a F.E.A.R. NX project patch. It keeps the
compile-only ARM64 build usable without a finished NVK archive, points
libmspack at its real source directory, and removes an upstream-project
diagnostic tied to another game's guest address.

`0018-switch-zero-argc-cvar-init.patch` gives CLI11 a synthetic program-name
argument when a Horizon launcher calls `rex::cvar::Init` with no usable
`argv[0]`. CLI11's `parse(argc, argv)` subtracts one from `argc` to size its
argument vector, so an `argc` of zero underflows and throws `std::length_error`
before the application can initialize. The guard is generic and leaves valid
argument vectors unchanged.

`0019-switch-process-handle-and-exception-probe.patch` handles launchers that
do not supply HBABI's process handle. It validates a loader handle against
the current PID, or acquires a real self handle with loopback HIPC, then shares
it between guest mappings and exception resume stubs. It accepts the expected
closed-session result only after validating the copied handle. The exception
probe also requires one actual handler entry per trap, preventing a false
pass if an emulator silently skips an undefined instruction. This patch
does not supply missing emulator exception handling.

Apply in `series` order with `git apply --index`. Do not reset an existing
SDK checkout: the bootstrap script checks the pinned base and recognizes the
fully applied patch set, otherwise it refuses to modify the checkout.

`0020-guest-memory-commit-diagnostics.patch` identifies the failing commit
stage, clears stale last-result state and records chunk/view counts plus
kernel system-resource queries on failure. Allocation and protection
behavior are unchanged.

`0021-switch-resource-telemetry.patch` uses the zero handle required by
InfoType_ResourceLimit, closes the returned handle, and reports query return
codes alongside process and system-resource byte values. These counters
do not by themselves identify the resource that caused a mapping failure.


`0022-compact-shadow-arena.patch` reserves a single code virtual-address arena
at startup and puts each committed chunk's shadow at `arena + guest_offset`.
The usable base is aligned to 2 MiB inside a padded reservation. In libnx,
`virtmemFindCodeMemory` takes a guard size, not an alignment argument; choosing
a random address for each small allocation scattered the previous shadows.
This change reserves addresses only: each backing allocation keeps its original
size, and the normal view mappings remain lazy. Init failure rolls back both
reservations; commit bounds reject overflow/out-of-window ranges. The arena is
released after the existing chunk cleanup on shutdown. As before, shutdown does
not propagate unmap errors; this patch does not change that cleanup policy.

Real-console logs identify `MapProcessCodeMemory` failure with description 103,
but do not identify the exhausted kernel allocator. Compact placement is intended
to reduce page-table fragmentation; it does not reduce the number of committed
chunks or guarantee that all metadata pressure is removed. Hardware retesting
is required before calling the campaign freeze fixed.
