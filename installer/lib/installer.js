// Builds the street-nx.zip package from the player's own game files.
//
// Adapted from defjam-sw-installer (R-drg), whose shape follows
// the-simpsons-game-nx-installer and nfsmw-nx-installer (StevensND, GPL-3.0):
// identify the executable, download the build, check it, and stream the zip to
// disk. street-nx needs nothing made from the disc: the package is the program,
// its settings, its licences and the disc's files. Like the game itself, a copy
// is accepted by the title ID in its certificate; the SHA-256 only tells the
// copy the port was tested with from the others.

import { ZipWriter, zipSize } from './zip.js';
import { t } from './i18n.js';

const CHUNK = 16 * 1024 * 1024;
export const ROOT = 'street-nx';
export const EXECUTABLE = 'default.xbe';
// The title ID in the executable's certificate, the one street.nro accepts.
export const TITLE_ID = 0x45410072;
// Folders every complete copy of the disc has.
const DATA_FOLDERS = ['data'];

export async function sha256(bytes) {
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
  return [...digest].map((b) => b.toString(16).padStart(2, '0')).join('');
}

async function* chunksOf(file) {
  for (let offset = 0; offset < file.size; offset += CHUNK) {
    yield await file.read(offset, Math.min(CHUNK, file.size - offset));
  }
}

// The title ID from an XBE, or null when it cannot be read. The header holds
// the base address (0x104) and the certificate's address (0x118); the title
// ID is the certificate's third word.
export function readTitleId(xbe) {
  if (xbe.length < 0x120 || String.fromCharCode(...xbe.subarray(0, 4)) !== 'XBEH') {
    return null;
  }
  const view = new DataView(xbe.buffer, xbe.byteOffset, xbe.byteLength);
  const base = view.getUint32(0x104, true);
  const certificate = view.getUint32(0x118, true) - base;
  if (certificate < 0 || certificate + 12 > xbe.length) {
    return null;
  }
  return view.getUint32(certificate + 8, true);
}

// Files that came with a dump but are not the game: system update folders
// ($SystemUpdate), and notes, checksums and thumbnails that dumping tools and
// file managers leave at the top or anywhere.
function isGameFile(path) {
  const parts = path.split('/');
  if (parts[0].startsWith('$')) {
    return false;
  }
  const name = parts[parts.length - 1].toLowerCase();
  if (name === 'thumbs.db' || name === 'desktop.ini' || name === '.ds_store') {
    return false;
  }
  return parts.length > 1 || !/\.(nfo|txt|sfv|md5|sha1|url|jpg|png|iso)$/i.test(name);
}

// Where the program is downloaded from. Its SHA-256 is part of the address,
// so a copy kept by the browser is never mistaken for a newer version.
export function programPath(build) {
  return `./release/${build.nro}?sha256=${build.nro_sha256}`;
}

// A folder can be chosen one level too high (the folder that holds the game
// folder): the game starts where its default.xbe is, the shallowest one.
export function rootAtExecutable(files) {
  const xbe = files
    .filter((f) => /(^|\/)default\.xbe$/i.test(f.path))
    .sort((a, b) => a.path.split('/').length - b.path.split('/').length)[0];
  if (!xbe) {
    return files;
  }
  const prefix = xbe.path.slice(0, xbe.path.length - EXECUTABLE.length);
  return files
    .filter((f) => f.path.startsWith(prefix))
    .map((f) => ({ ...f, path: f.path.slice(prefix.length) }));
}

// Identifies the game. files: [{ path, size, read(offset, length) }] relative
// to the game's root. Returns { xbeHash, titleId, build (null when it is not
// this game), tested (the exact copy the build was tested with), complete
// (default.xbe and the disc's data folder are there), game (the files that go
// into the package, in package order) }.
export async function identifyGame(files, manifest) {
  const xbeFile = files.find((f) => f.path.toLowerCase() === EXECUTABLE);
  if (!xbeFile) {
    const error = new Error(t('noExecutable'));
    error.code = 'noExecutable';
    throw error;
  }
  const xbe = await xbeFile.read(0, xbeFile.size);
  const xbeHash = await sha256(xbe);
  const titleId = readTitleId(xbe);
  const exact = manifest.builds.find((b) => b.xbe_sha256 === xbeHash) || null;
  const build = exact || (titleId === TITLE_ID ? manifest.builds[0] : null);
  const complete = DATA_FOLDERS.every((dir) =>
    files.some((f) => f.path.toLowerCase().startsWith(`${dir}/`)));
  const game = files
    .filter((f) => isGameFile(f.path))
    .sort((a, b) => a.path.toLowerCase().localeCompare(b.path.toLowerCase()));
  return { xbeHash, xbeSize: xbe.length, titleId, build, tested: Boolean(exact), complete, game };
}

// release: { manifest, nro, settings, licenses } (Uint8Arrays apart from manifest)
// sink: { write(Uint8Array), close() } receiving the zip file
// onSize(bytes) is called with the exact size of the zip before the first byte.
// update: only the program and its licences, for a folder that is already on
// the SD card; the disc is identified but none of it is copied, and the
// settings file the player may have changed is left alone.
export async function createPackage(files, release, sink, log = () => {}, progress = () => {}, onSize = () => {},
  { update = false } = {}) {
  const { xbeHash, build, tested, complete, game } = await identifyGame(files, release.manifest);
  if (!build) {
    throw new Error(t('notSupported', { hash: xbeHash }));
  }
  if (!complete && !update) {
    throw new Error(t('incomplete'));
  }
  log(t('logRelease', { edition: tested ? build.edition : `title ID 45410072 (${xbeHash.slice(0, 16)}…)` }));
  if (update) {
    log(t('logUpdate'));
  }

  const copies = update ? [] : game;
  const fixed = [
    { path: `${ROOT}/street.nro`, bytes: release.nro },
    ...(update ? [] : [{ path: `${ROOT}/street-settings.toml`, bytes: release.settings }]),
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
