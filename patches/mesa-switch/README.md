# Mesa/NVK patches

Base: `danfromtico/mesa-switch` at
`d4a00ea0ab3f59afb967cc5d779e4263d237bd77` (Mesa 26.2.3).

Patches 0001–0004 originate from Project8 SW: descriptor binding, shader
scheduling, direct draws and the Horizon build script. Patches 0005–0006 add
initial GPU engine bindings. Apply this series using the pinned base above.

See [driver build instructions](../../docs/driver-build.md). The public release
targets physical Switch hardware; Eden support is experimental.
