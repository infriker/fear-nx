# Mesa/NVK patches

Base: `danfromtico/mesa-switch` at
`d4a00ea0ab3f59afb967cc5d779e4263d237bd77` (Mesa 26.2.3).

Patches 0001–0004 originate from Project8 SW: descriptor binding, shader
scheduling, direct draws and the Horizon build script. Patches 0005–0006 add
initial GPU engine bindings. Patch 0007 backports upstream commit
`f68a41350dbc0b380dcb1462898d8bed48dfdb5e`: account for actual GPFIFO submission
capacity and completion entries while retaining the one-time engine binding.

Patch 0008 carries the depth/stencil-memory fix from
`a72f9551e79e7881ba67940f41df2b115d360600`: images with a stencil-copy plane
use independent memory mappings for the required PTE kinds. This patch is
included in the v0.1.1 driver and the preceding weapon-screen test NROs.
It may disable compression for affected images; compression is not globally
disabled. No matched hardware FPS measurement isolates this patch.

Apply all eight patches in filename order to the pinned base above.

See [driver build instructions](../../docs/driver-build.md). The public release
targets physical Switch hardware; Eden support is experimental.
