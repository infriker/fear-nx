# Installing the deko3d build

Follow the [branch README](../README.md#how-to-install). The browser installer
currently supplies Vulkan; it does not select deko3d from a Git branch.

Place `fear_nx_deko3d.nro` and `fear-nx.toml` in `sdmc:/switch/fear-nx/`.
Your own supported Xbox 360 files belong in `game/default.xex` and `game/Game/`
under that folder. Copy the entire extracted game; `Game/uberCatalog.cat` must
be present. The PC edition cannot be substituted.

Use title takeover mode (hold R when starting an installed game). Keep
`userdata/` when updating and back up custom settings before replacing the TOML.
Vulkan and deko3d NROs have distinct names and can coexist in this folder.
The profile does not force overclocking; the reported 20–30 FPS used overclocking.

See [known issues](known-issues.md) for performance and reporting limitations.
