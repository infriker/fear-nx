// Runs the page's identification and packaging code in Node on a synthetic
// game (a fake default.xex and a few small files, no game data), writes the
// zips to a temporary folder and checks them with Python's zipfile.
// Usage: node test/package_node.mjs [path to a real default.xex, to check its
// title ID and whether the manifest knows it]
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { createPackage, identifyGame, readTitleId, rootAtExecutable, sha256, TITLE_ID } from '../lib/installer.js';

let failed = 0;
function expect(condition, what) {
  console.log(`${condition ? 'ok  ' : 'FAIL'} ${what}`);
  if (!condition) {
    failed++;
  }
}

// An XEX2 header with one optional header, the execution info, whose title ID
// is at +12.
function fakeXex(titleId, salt) {
  const bytes = new Uint8Array(4096);
  const view = new DataView(bytes.buffer);
  bytes.set([0x58, 0x45, 0x58, 0x32]);
  view.setUint32(0x14, 1);
  view.setUint32(0x18, 0x00040006);
  view.setUint32(0x1c, 0x100);
  view.setUint32(0x100 + 12, titleId);
  bytes[4000] = salt;
  return bytes;
}

function memoryFile(filePath, bytes) {
  return { path: filePath, size: bytes.length, read: async (o, n) => bytes.subarray(o, o + n) };
}

function fileSink(target) {
  const fd = fs.openSync(target, 'w');
  return {
    async write(bytes) {
      fs.writeSync(fd, bytes);
    },
    async close() {
      fs.closeSync(fd);
    },
  };
}

function zipEntries(zip) {
  const out = execFileSync(process.env.PYTHON || 'python3', ['-c', `
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
bad = z.testzip()
print("BAD " + bad if bad else "CRC-OK")
for i in z.infolist(): print(i.filename, i.file_size)
`, zip]).toString().trim().split(/\r?\n/);
  return { crcOk: out[0] === 'CRC-OK', entries: out.slice(1) };
}

const xex = fakeXex(TITLE_ID, 1);
const big = new Uint8Array(40 * 1024 * 1024).map((_, i) => (i * 7) & 0xff);
const game = [
  memoryFile('default.xex', xex),
  memoryFile('Game/uberCatalog.cat', new Uint8Array([1, 2, 3])),
  memoryFile('Game/videos/intro.bik', big),
  memoryFile('$SYSTEMUPDATE/su20076000_00000000', new Uint8Array(10)),
  memoryFile('dump.nfo', new Uint8Array(5)),
];
const manifest = {
  version: 'test',
  builds: [{ edition: 'test disc', xex_sha256: await sha256(xex), nro: 'fear_nx.nro', nro_sha256: 'x', toml: 'fear-nx.toml' }],
};
const release = {
  manifest,
  nro: new Uint8Array([0x4e, 0x52, 0x4f, 0x30]),
  toml: new TextEncoder().encode('fake_renderer = "native"\n'),
  licenses: new TextEncoder().encode('licences\n'),
};
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fearnx-test-'));

expect(readTitleId(xex) === TITLE_ID, 'title ID read from an XEX2 header');
const id = await identifyGame(game, manifest);
expect(id.build && id.complete, 'a complete supported game is recognised');
expect(!id.game.some((f) => f.path.startsWith('$') || f.path === 'dump.nfo'), 'the update partition and notes are left out');

const nested = rootAtExecutable(game.map((f) => ({ ...f, path: `My Games/FEAR/${f.path}` })));
expect(nested.some((f) => f.path === 'default.xex') && nested.some((f) => f.path === 'Game/uberCatalog.cat'),
  'a folder chosen one level too high is rooted at default.xex');

const unsafe = await identifyGame([...game, memoryFile('Game/../escape', xex), memoryFile('/Game/unsafe', xex)], manifest);
expect(!unsafe.game.some(f => f.path.includes('..') || f.path.startsWith('/')), 'unsafe archive paths are excluded');
const oldLayout = await identifyGame([game[0], memoryFile('DATA/uberCatalog.cat', xex)], manifest);
expect(!oldLayout.complete, 'Project8 DATA layout is not accepted as FEAR data');

const other = await identifyGame([memoryFile('default.xex', fakeXex(TITLE_ID, 2))], manifest);
expect(!other.build && other.titleId === TITLE_ID, 'another release of the game is told apart from another game');
const notGame = await identifyGame([memoryFile('default.xex', fakeXex(0x4d5307e6, 3))], manifest);
expect(!notGame.build && notGame.titleId !== TITLE_ID, 'another game is not mistaken for this one');

let size = 0;
const full = path.join(dir, 'fear-nx.zip');
await createPackage(game, release, fileSink(full), () => {}, () => {}, (s) => (size = s));
const fullZip = zipEntries(full);
expect(fullZip.crcOk, 'full package: every CRC checks');
expect(fs.statSync(full).size === size, `full package: announced size is exact (${size})`);
expect(JSON.stringify(fullZip.entries) === JSON.stringify([
  'fear-nx/fear_nx.nro 4',
  'fear-nx/fear-nx.toml 25',
  'fear-nx/LICENSES.txt 9',
  'fear-nx/game/default.xex 4096',
  'fear-nx/game/Game/uberCatalog.cat 3',
  'fear-nx/game/Game/videos/intro.bik 41943040',
]), 'full package: the expected files, in order');

const upd = path.join(dir, 'fear-nx-update.zip');
await createPackage([game[0]], release, fileSink(upd), () => {}, () => {}, (s) => (size = s), { update: true });
const updZip = zipEntries(upd);
expect(updZip.crcOk && fs.statSync(upd).size === size && updZip.entries.length === 3,
  'update package: program, settings and licences only, from default.xex alone');

let refused = false;
try {
  await createPackage([game[0]], release, fileSink(path.join(dir, 'x.zip')));
} catch {
  refused = true;
}
expect(refused, 'a full package needs Game/uberCatalog.cat');

const real = process.argv[2];
if (real) {
  const bytes = new Uint8Array(fs.readFileSync(real));
  const known = JSON.parse(fs.readFileSync(new URL('../release/manifest.json', import.meta.url)));
  const hash = await sha256(bytes);
  expect(readTitleId(bytes) === TITLE_ID, `real default.xex: title ID 0x${(readTitleId(bytes) >>> 0).toString(16)}`);
  expect(known.builds.some((b) => b.xex_sha256 === hash), 'real default.xex: known to release/manifest.json');
}

fs.rmSync(dir, { recursive: true, force: true });
console.log(failed ? `${failed} failed` : 'all passed');
process.exit(failed ? 1 : 0);
