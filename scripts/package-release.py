#!/usr/bin/env python3
"""Package an existing Switch NRO, settings and notices without game assets."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--nro', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    nro = args.nro.resolve()
    with nro.open('rb') as stream:
        header = stream.read(0x80)
        if len(header) != 0x80 or header[0x10:0x14] != b'NRO0':
            parser.error('Input is not a Switch NRO')
        image_size = struct.unpack_from('<I', header, 0x18)[0]
        stream.seek(image_size)
        if stream.read(4) != b'ASET':
            parser.error('NRO is missing its application assets')
    output = args.output.resolve()
    if output == ROOT or ROOT.is_relative_to(output):
        parser.error('Choose a dedicated output directory')
    output.mkdir(parents=True, exist_ok=True)
    target = output / 'fear_nx.nro'
    if nro != target:
        shutil.copyfile(nro, target)
    shutil.copyfile(ROOT / 'release/fear-nx.toml', output / 'fear-nx.toml')
    texts = ['F.E.A.R. NX - licenses and notices\n']
    for file in [ROOT / 'LICENSE', ROOT / 'NOTICE', *sorted((ROOT / 'LICENSES').rglob('*'))]:
        if file.is_file():
            texts.append(f'\n===== {file.relative_to(ROOT).as_posix()} =====\n\n')
            texts.append(file.read_text(encoding='utf-8'))
    (output / 'LICENSES.txt').write_text(''.join(texts), encoding='utf-8', newline='\n')
    names = ['fear_nx.nro', 'fear-nx.toml', 'LICENSES.txt']
    with zipfile.ZipFile(output / 'fear-nx-update.zip', 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for name in names:
            archive.write(output / name, f'fear-nx/{name}')
    names.append('fear-nx-update.zip')
    hashes = {name: hashlib.sha256((output / name).read_bytes()).hexdigest() for name in names}
    (output / 'SHA256SUMS.txt').write_text(''.join(f'{digest}  {name}\n' for name, digest in hashes.items()), encoding='ascii')
    print(json.dumps({'output': str(output), 'sha256': hashes}, indent=2))


if __name__ == '__main__':
    main()
