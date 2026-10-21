// Builds the fear-nx.zip package from the player's own game files.
//
// The shape follows nfsmw-nx-installer (StevensND, GPL-3.0): identify the
// executable by its SHA-256, download the build made for it, check that too,
// and stream the zip to disk. Unlike nfsmw-nx, fear-nx translates the
// game's shaders on the console, so nothing is made from the disc here: the
// package is the program, its settings, its licences and the disc's files.

import { ZipWriter, zipSize } from './zip.js';
import { t } from './i18n.js';

const CHUNK = 16 * 1024 * 1024;
export const ROOT = 'fear-nx';
// The title ID in the executable's header: the same game in a release this
// build was not compiled from (a different hash) still carries it.
export const TITLE_ID = 0x565507d9;

export async function sha256(bytes) {
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
  return [...digest].map((b) => b.toString(16).padStart(2, '0')).join('');
}

async function* chunksOf(file) {
  for (let offset = 0; offset < file.size; offset += CHUNK) {
    yield await file.read(offset, Math.min(CHUNK, file.size - offset));
  }
}

// The title ID from an XEX2 header, or null when it cannot be read. The
// execution info (optional header 0x00040006) holds media ID, version, base
// version and then the title ID.
export function readTitleId(xex) {
  if (xex.length < 24 || String.fromCharCode(...xex.subarray(0, 4)) !== 'XEX2') {
    return null;
  }
  const view = new DataView(xex.buffer, xex.byteOffset, xex.byteLength);
  const count = view.getUint32(0x14);
  for (let i = 0; i < count && 0x18 + i * 8 + 8 <= xex.length; i++) {
    const key = view.getUint32(0x18 + i * 8);
    const value = view.getUint32(0x18 + i * 8 + 4);
    if (key === 0x00040006 && value + 16 <= xex.length) {
      return view.getUint32(value + 12);
    }
  }
  return null;
}

// Files that came with a dump but are not the game: the system update
// partition ($SYSTEMUPDATE), and notes, checksums and thumbnails that dumping
// tools and file managers leave at the top or anywhere.
function isGameFile(path) {
  if (path.startsWith('/') || path.includes('\\') || path.split('/').some((p) => !p || p === '.' || p === '..')) return false;
  if (!/^default\.xex$/i.test(path) && !/^game\//i.test(path)) return false;
  const parts = path.split('/');
  if (parts[0].startsWith('$')) {
    return false;
  }
  const name = parts[parts.length - 1].toLowerCase();
  if (name === 'thumbs.db' || name === 'desktop.ini' || name === '.ds_store') {
    return false;
  }
  return parts.length > 1 || !/\.(nfo|txt|sfv|md5|sha1|url|jpg|png)$/i.test(name);
}

// Where the program is downloaded from. Its SHA-256 is part of the address,
// so a copy kept by the browser is never mistaken for a newer version.
export function programPath(build) {
  return `./release/${build.nro}?sha256=${build.nro_sha256}`;
}

// A folder can be chosen one level too high (the folder that holds the game
// folder): the game starts where its default.xex is, the shallowest one.
export function rootAtExecutable(files) {
  const xex = files
    .filter((f) => /(^|\/)default\.xex$/i.test(f.path))
    .sort((a, b) => a.path.split('/').length - b.path.split('/').length)[0];
  if (!xex) {
    return files;
  }
  const prefix = xex.path.slice(0, xex.path.length - 'default.xex'.length);
  return files
    .filter((f) => f.path.startsWith(prefix))
    .map((f) => ({ ...f, path: f.path.slice(prefix.length) }));
}

// Identifies the game. files: [{ path, size, read(offset, length) }] relative
// to the game's root. Returns { xexHash, titleId, build (null when this exact
// executable has no build), complete (default.xex and Game/uberCatalog.cat are both there),
// game (the files that go into the package, in package order) }.
export async function identifyGame(files, manifest) {
  const xexFile = files.find((f) => f.path.toLowerCase() === 'default.xex');
  if (!xexFile) {
    const error = new Error(t('noExecutable'));
    error.code = 'noExecutable';
    throw error;
  }
  const xex = await xexFile.read(0, xexFile.size);
  const xexHash = await sha256(xex);
  const build = manifest.builds.find((b) => b.xex_sha256 === xexHash) || null;
  const complete = files.some((f) => /^game\/ubercatalog\.cat$/i.test(f.path) && f.size > 0);
  const game = files
    .filter((f) => isGameFile(f.path))
    .sort((a, b) => a.path.toLowerCase().localeCompare(b.path.toLowerCase()));
  return { xexHash, xexSize: xex.length, titleId: readTitleId(xex), build, complete, game };
}

// release: { manifest, nro, toml, licenses } (Uint8Arrays apart from manifest)
// sink: { write(Uint8Array), close() } receiving the zip file
// onSize(bytes) is called with the exact size of the zip before the first byte.
// update: only the program, its settings and its licences, for a folder that
// is already on the SD card; the disc is identified but none of it is copied.
export async function createPackage(files, release, sink, log = () => {}, progress = () => {}, onSize = () => {},
  { update = false } = {}) {
  const { xexHash, build, complete, game } = await identifyGame(files, release.manifest);
  if (!build) {
    throw new Error(t('notSupported', { hash: xexHash }));
  }
  if (!complete && !update) {
    throw new Error(t('incomplete'));
  }
  log(t('logRelease', { edition: build.edition }));
  if (update) {
    log(t('logUpdate'));
  }

  const copies = update ? [] : game;
  const fixed = [
    { path: `${ROOT}/fear_nx.nro`, bytes: release.nro },
    { path: `${ROOT}/fear-nx.toml`, bytes: release.toml },
    { path: `${ROOT}/LICENSES.txt`, bytes: release.licenses },
  ];
  onSize(zipSize([
    ...fixed.map((f) => ({ path: f.path, size: f.bytes.length })),
    ...copies.map((f) => ({ path: `${ROOT}/game/${f.path}`, size: f.size })),
  ]));
  const zip = new ZipWriter(sink);
  for (const f of fixed) {
    await zip.addBytes(f.path, f.bytes);
  }
  const total = copies.reduce((sum, f) => sum + f.size, 0);
  let done = 0;
  for (const f of copies) {
    await zip.addFile(`${ROOT}/game/${f.path}`, f.size, chunksOf(f), (chunk) => {
      done += chunk.length;
      progress(total ? done / total : 1);
    });
    log(t('copied', { path: f.path }));
  }
  await zip.finish();
  progress(1);
  log(t('done'));
  return { edition: build.edition, files: copies.length };
}
