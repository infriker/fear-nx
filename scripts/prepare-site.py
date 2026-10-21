#!/usr/bin/env python3
"""Assemble GitHub Pages from the installer and a verified release bundle."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--release', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
manifest = json.loads((ROOT / 'installer/release/manifest.json').read_text(encoding='utf-8'))
for build in manifest['builds']:
    binary = args.release / build['nro']
    if hashlib.sha256(binary.read_bytes()).hexdigest() != build['nro_sha256']:
        raise SystemExit(f"Wrong SHA-256: {build['nro']}")
output = args.output.resolve()
if output.exists() and any(output.iterdir()):
    raise SystemExit('Choose an empty output directory to avoid deploying stale files')
output.mkdir(parents=True, exist_ok=True)
for name in ['index.html', 'style.css', 'icon.jpg', 'app.js', 'worker.js', 'sw.js', 'LICENSE', 'NOTICES.md']:
    shutil.copyfile(ROOT / 'installer' / name, output / name)
shutil.copytree(ROOT / 'installer/lib', output / 'lib')
(output / 'release').mkdir()
shutil.copyfile(ROOT / 'installer/release/manifest.json', output / 'release/manifest.json')
for name in {'LICENSES.txt', *(b['nro'] for b in manifest['builds']), *(b['toml'] for b in manifest['builds'])}:
    shutil.copyfile(args.release / name, output / 'release' / name)
(output / '.nojekyll').touch()
print(f"Installer {manifest['version']} prepared in {output}")
