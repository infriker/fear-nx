// Exercise the actual ISO reader on a small XDVDFS image; no game data.
import assert from 'node:assert/strict';
import { listIsoFiles } from '../lib/iso.js';
import { identifyGame, sha256, TITLE_ID } from '../lib/installer.js';

const sector = 2048;
const image = new Uint8Array(44 * sector);
const view = new DataView(image.buffer);
const encode = (value) => new TextEncoder().encode(value);
image.set(encode('MICROSOFT*XBOX*MEDIA'), 32 * sector);
view.setUint32(32 * sector + 0x14, 33, true);
view.setUint32(32 * sector + 0x18, sector, true);
function entry(at, name, dataSector, size, directory = false, right = 0) {
  view.setUint16(at + 2, right, true);
  view.setUint32(at + 4, dataSector, true);
  view.setUint32(at + 8, size, true);
  image[at + 12] = directory ? 0x10 : 0;
  image[at + 13] = name.length;
  image.set(encode(name), at + 14);
}
entry(33 * sector, 'default.xex', 36, 4096, false, 8);
entry(33 * sector + 32, 'Game', 34, sector, true);
entry(34 * sector, 'uberCatalog.cat', 38, 3, false, 8);
entry(34 * sector + 32, 'videos', 35, sector, true);
entry(35 * sector, 'intro.bik', 39, 4);
image.set(encode('XEX2'), 36 * sector);
view.setUint32(36 * sector + 0x14, 1);
view.setUint32(36 * sector + 0x18, 0x00040006);
view.setUint32(36 * sector + 0x1c, 0x100);
view.setUint32(36 * sector + 0x10c, TITLE_ID);
image.set([1, 2, 3], 38 * sector);
image.set([4, 5, 6, 7], 39 * sector);
const files = await listIsoFiles(new Blob([image]));
assert.deepEqual(files.map(f => f.path), ['default.xex', 'Game/uberCatalog.cat', 'Game/videos/intro.bik']);
assert.deepEqual([...await files[2].read(1, 2)], [5, 6]);
const xex = await files[0].read(0, files[0].size);
const manifest = { builds: [{ xex_sha256: await sha256(xex) }] };
const identified = await identifyGame(files, manifest);
assert.ok(identified.build && identified.complete && identified.titleId === TITLE_ID);
await assert.rejects(listIsoFiles(new Blob([new Uint8Array(100)])), /no Xbox 360/);
console.log('all passed: XDVDFS directory tree, ranged reads, executable detection, invalid ISO');
