// Runs the page's identification and packaging code in Node on a synthetic
// game (a fake default.xbe and a few small files, no game data), writes the
// zips to a temporary folder and checks them with Python's zipfile.
// Usage: node test/package_node.mjs [path to a real default.xbe, to check its
// title ID and whether the manifest knows it]
// Adapted from defjam-sw-installer (R-drg), itself from the-simpsons-game-nx-installer.
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

// An XBE header: base address 0x10000, the certificate at base + 0x184, the
// title ID its third word -- the layout of the retail executable.
function fakeXbe(titleId, salt) {
  const bytes = new Uint8Array(4096);
  const view = new DataView(bytes.buffer);
  bytes.set([0x58, 0x42, 0x45, 0x48]);
  view.setUint32(0x104, 0x10000, true);
  view.setUint32(0x118, 0x10184, true);
  view.setUint32(0x184 + 8, titleId, true);
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
  const out = execFileSync('python3', ['-c', `
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
bad = z.testzip()
print("BAD " + bad if bad else "CRC-OK")
for i in z.infolist(): print(i.filename, i.file_size)
`, zip]).toString().trim().split('\n');
  return { crcOk: out[0] === 'CRC-OK', entries: out.slice(1) };
}

const xbe = fakeXbe(TITLE_ID, 1);
const big = new Uint8Array(40 * 1024 * 1024).map((_, i) => (i * 7) & 0xff);
const game = [
  memoryFile('default.xbe', xbe),
  memoryFile('data/audio/crowd.rsf', new Uint8Array([1, 2, 3])),
  memoryFile('data/fonts/fonts.big', new Uint8Array([4, 5])),
  memoryFile('data/femovies/intro.mad', big),
  memoryFile('update.xbe', new Uint8Array([6])),
  memoryFile('$SystemUpdate/xboxupd.xbe', new Uint8Array(10)),
  memoryFile('dump.nfo', new Uint8Array(5)),
];
const manifest = {
  version: 'test',
  builds: [{ edition: 'test disc', xbe_sha256: await sha256(xbe), nro: 'street.nro', nro_sha256: 'x', settings: 'street-settings.toml' }],
};
const release = {
  manifest,
  nro: new Uint8Array([0x4e, 0x52, 0x4f, 0x30]),
  settings: new TextEncoder().encode('fps_cap = 30\n'),
  licenses: new TextEncoder().encode('licences\n'),
};
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'street-nx-test-'));

expect(readTitleId(xbe) === TITLE_ID, 'title ID read from an XBE certificate');
const id = await identifyGame(game, manifest);
expect(id.build && id.tested && id.complete, 'a complete supported game is recognised');
expect(!id.game.some((f) => f.path.startsWith('$') || f.path === 'dump.nfo'), 'the update folder and notes are left out');

const nested = rootAtExecutable(game.map((f) => ({ ...f, path: `My Games/FIFA Street/${f.path}` })));
expect(nested.some((f) => f.path === 'default.xbe') && nested.some((f) => f.path === 'data/fonts/fonts.big'),
  'a folder chosen one level too high is rooted at default.xbe');

const other = await identifyGame([memoryFile('default.xbe', fakeXbe(TITLE_ID, 2))], manifest);
expect(other.build && !other.tested, 'another copy with the title ID is accepted, marked untested');
const notGame = await identifyGame([memoryFile('default.xbe', fakeXbe(0x4d530004, 3))], manifest);
expect(!notGame.build && notGame.titleId !== TITLE_ID, 'another game is not mistaken for this one');

let size = 0;
const full = path.join(dir, 'street-nx.zip');
await createPackage(game, release, fileSink(full), () => {}, () => {}, (s) => (size = s));
const fullZip = zipEntries(full);
expect(fullZip.crcOk, 'full package: every CRC checks');
expect(fs.statSync(full).size === size, `full package: announced size is exact (${size})`);
expect(JSON.stringify(fullZip.entries) === JSON.stringify([
  'street-nx/street.nro 4',
  'street-nx/street-settings.toml 13',
  'street-nx/LICENSES.txt 9',
  'street-nx/game/data/audio/crowd.rsf 3',
  'street-nx/game/data/femovies/intro.mad 41943040',
  'street-nx/game/data/fonts/fonts.big 2',
  'street-nx/game/default.xbe 4096',
  'street-nx/game/update.xbe 1',
]), 'full package: the expected files, in order');

const upd = path.join(dir, 'street-nx-update.zip');
await createPackage([game[0]], release, fileSink(upd), () => {}, () => {}, (s) => (size = s), { update: true });
const updZip = zipEntries(upd);
expect(updZip.crcOk && fs.statSync(upd).size === size && updZip.entries.length === 2
  && !updZip.entries.some((e) => e.includes('street-settings.toml')),
  'update package: program and licences only (settings kept), from default.xbe alone');

let refused = false;
try {
  await createPackage([game[0]], release, fileSink(path.join(dir, 'x.zip')));
} catch {
  refused = true;
}
expect(refused, 'a full package needs the disc\'s data folder');

const real = process.argv[2];
if (real) {
  const bytes = new Uint8Array(fs.readFileSync(real));
  const known = JSON.parse(fs.readFileSync(new URL('../release/manifest.json', import.meta.url)));
  const hash = await sha256(bytes);
  expect(readTitleId(bytes) === TITLE_ID, `real default.xbe: title ID 0x${(readTitleId(bytes) >>> 0).toString(16)}`);
  expect(known.builds.some((b) => b.xbe_sha256 === hash), 'real default.xbe: known to release/manifest.json');
}

fs.rmSync(dir, { recursive: true, force: true });
console.log(failed ? `${failed} failed` : 'all passed');
process.exit(failed ? 1 : 0);
