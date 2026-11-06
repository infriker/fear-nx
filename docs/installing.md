# Installation

Use the [browser installer](https://infriker.github.io/fear-nx/) with an Xbox 360
disc image or extracted folder. The extracted layout is `default.xex` beside
`Game/`; `Game/uberCatalog.cat` must be present. The installer checks the XEX
revision, but that check alone cannot prove that every data file is intact.

The full ZIP contains `fear-nx/`. Extract it into `sdmc:/switch/`.
The resulting executable is `sdmc:/switch/fear-nx/fear_nx.nro`; the game's
executable is `sdmc:/switch/fear-nx/game/default.xex` and its data are under
`sdmc:/switch/fear-nx/game/Game/`.

Launch through title takeover: hold R while starting an installed game, then
select F.E.A.R. NX in the Homebrew Menu. Album/applet mode does not provide the
required memory. The game uses the system or player-selected clock profile.
At 1020 MHz CPU and 460 MHz GPU, v0.2.0 typically reaches 30 FPS, with drops
to 25 FPS in heavy firefights. See [performance by clock setting](performance.md#switch-performance-in-v020).

## Updates

Select your supported game folder and choose **Create fear-nx-update.zip**.
For an update, `default.xex` alone in the selected folder is enough. The update
contains only `fear_nx.nro`, `fear-nx.toml` and `LICENSES.txt`.

Back up your TOML if you customized it. Keep `userdata/` (saves), `game/` and the
existing runtime directories when replacing the release files.

## Manual installation

Download `fear-nx-update.zip` from the [release page](https://github.com/infriker/fear-nx/releases/latest),
extract it into `sdmc:/switch/`, and copy your own `default.xex` and `Game/`
directory into `sdmc:/switch/fear-nx/game/`. The release contains no original
game assets.

## Troubleshooting

- **Unsupported executable:** the port needs the exact SHA-256 shown in the
  README. PC game files and other Xbox revisions cannot be substituted.
- **Incomplete folder:** choose the folder above `default.xex` and `Game/`,
  not `Game/` alone. Copy the whole extracted game.
- **No launch or loading failure:** check title takeover mode, package paths
  and the presence of your original game files.
- **Missing objects on first encounter:** allow asynchronous pipelines to
  compile; see [known issues](known-issues.md).
- **Installer download fails:** keep the page open until the browser download
  completes and use a current desktop browser with service workers enabled.

The installer reads files locally. Its network requests download the page and
port files; they do not upload your ISO, XEX or game data.
