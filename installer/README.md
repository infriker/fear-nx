# F.E.A.R. NX browser installer

Hosted at https://infriker.github.io/fear-nx/. Select an Xbox 360 ISO or extracted
folder. Identification uses the pinned XEX SHA-256; full installation requires
`Game/uberCatalog.cat`. An update needs only the supported `default.xex`.
Game files are processed locally and streamed into a ZIP with bounded memory.

The installer is adapted from Project8 SW and nfsmw-nx-installer; see NOTICES.md.

## Local development

From the repository root:

```sh
node installer/test/package_node.mjs
node installer/test/iso_node.mjs
python scripts/package-release.py --nro out/switch/fear_nx.nro --output dist/public-release
python scripts/prepare-site.py --release dist/public-release --output dist/site
python installer/serve.py 8195 dist/site
```

Open `http://127.0.0.1:8195/`. Use a new empty site output directory when
repeating the preparation. Optional: pass a path to your own `default.xex`
to the packaging test. Set `PYTHON` if the Python executable is not called
`python3`. Generated binaries and game data must remain outside Git.

## Deployment

The GitHub Pages workflow downloads the NRO from the pinned release tag in
`release/manifest.json`, verifies its checksum and publishes the installer at
the repository's Pages root. The binary is a release asset, not a Git source
file. Update the manifest and release assets together for a new version.
The workflow also packages the root release TOML and license notices.
