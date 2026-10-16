/*
APP.JS

The launcher page (index.html): it checks that the browser can run the game,
copies the game data in from the player's disc image (xiso-worker.js), and
starts the game (halo.js and halo.wasm: tools/web_build.py), whose canvas
then fills the page.

- The game's threads need SharedArrayBuffer, so a cross-origin isolated
  page: GitHub Pages cannot send the headers for it, the service worker
  (sw.js) adds them, and the first visit loads the page once more under it.
- The game reads its maps, and keeps its saved games, config.toml and
  debug.txt, in the Origin Private File System (port/web/src/web_main.c
  mounts it at /data). Each origin has its own: the github.io address and
  the site's own domain do not share them.
- Query parameters, for testing: ?set=NAME=value (any number) passes a
  setting's environment variable (port/linux/README.md, the settings
  table), ?room=<secret> joins a room (net.js; ?brokers= names other
  message brokers, for internet play's signalling too), ?menu=<screen> opens
  a menu screen (debug.menu_open), ?map=<level> starts a level (a10, or
  levels\a10\a10: the init.txt the game reads, which the page removes again
  the next time), ?play starts the game at once when it can.
- ?join=<invite> (a halo://join/ link's code, which native builds and the
  game here make: port/linux/src/p2p.c) joins that internet game once the
  game starts.
- (local only) ?btih=<infohash> or ?torrent=<url> copies the maps in from a
  torrent of the player's own disc image, then starts the game; ?tr= and
  ?ws= add WebSocket trackers and web seeds (importTorrent, below).
*/

'use strict';

const $ = (id) => document.getElementById(id);
const params = new URLSearchParams(location.search);
// the Xbox window ends the game's memory: tools/web_build.py WEB_MEMORY_BYTES
const WEB_MEMORY_BYTES = 0x88000000;
const MAXIMUM_LOG_LINES = 20000;

// ---------- the log: the game's output, for the player to send

const log = [];

function logLine(text) {
  for (const line of String(text).replace(/\r/g, '').split('\n')) {
    if (line === '') continue;
    log.push(line);
  }
  if (log.length > MAXIMUM_LOG_LINES) log.splice(0, log.length - MAXIMUM_LOG_LINES);
}

function download(name, blob) {
  const link = document.createElement('a');
  link.href = URL.createObjectURL(blob);
  link.download = name;
  document.body.append(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(link.href), 10000);
}

async function readStorageText(name) {
  try {
    const root = await navigator.storage.getDirectory();
    const file = await (await root.getFileHandle(name)).getFile();
    return await file.text();
  } catch {
    return null;
  }
}

async function downloadLog() {
  let text = `OpenCE web ${JSON.stringify(stamp)}\n${navigator.userAgent}\n\n` + log.join('\n') + '\n';
  // (debug.txt, which the game writes, when it is not open: before or after
  // a game)
  if (!gameStarted || gameStopped) {
    const debug = await readStorageText('debug.txt');
    if (debug) text += '\n---------- debug.txt\n' + debug.slice(-2000000);
  }
  download('opence-web-log.txt', new Blob([text], { type: 'text/plain' }));
}

// ---------- errors: the game's own, not a browser extension's

function isExtensionNoise(message, source) {
  const text = String(message || '') + ' ' + String(source || '');
  return /chrome-extension:|moz-extension:|safari-extension:|MetaMask|ethereum/i.test(text);
}

window.addEventListener('error', (event) => {
  if (isExtensionNoise(event.message, event.filename)) return;
  logLine('page error: ' + (event.error && event.error.stack ? event.error.stack : event.message));
});
window.addEventListener('unhandledrejection', (event) => {
  const reason = event.reason;
  const text = reason && reason.stack ? reason.stack : String(reason);
  if (isExtensionNoise(text)) return;
  logLine('page error: ' + text);
});

// ---------- the build (version.json) and its updates

let stamp = {};

async function loadStamp() {
  try {
    stamp = await (await fetch('version.json', { cache: 'no-cache' })).json();
  } catch {
    stamp = {};
  }
  $('about-build').textContent = stamp.build ? `${stamp.build} (${stamp.flavor})` : `local (${stamp.flavor || 'unknown'})`;
  $('about-commit').textContent = stamp.commit ? stamp.commit.slice(0, 12) : '–';
  $('about-network').textContent = stamp.network_version != null ? String(stamp.network_version) : '–';
}

async function checkForUpdate() {
  if (!navigator.serviceWorker || !navigator.serviceWorker.controller || !stamp.version) return;
  try {
    const latest = await (await fetch('version.json?latest', { cache: 'no-store' })).json();
    if (latest.version && latest.version !== stamp.version) {
      $('update-text').textContent = latest.build ? `Build ${latest.build} is out.` : 'A new build is out.';
      $('update').hidden = false;
    }
  } catch {
    // offline: the build there is plays
  }
}

$('update-button').addEventListener('click', () => {
  $('update-button').disabled = true;
  $('update-text').textContent = 'Updating…';
  navigator.serviceWorker.addEventListener('message', (event) => {
    if (event.data === 'updated') location.reload();
    else if (event.data === 'update-failed') $('update-text').textContent = 'The update could not be downloaded.';
  });
  navigator.serviceWorker.controller.postMessage('update');
});

// ---------- cross-origin isolation (sw.js)

async function isolate() {
  if (window.crossOriginIsolated || !('serviceWorker' in navigator) || !window.isSecureContext) return;
  try {
    await navigator.serviceWorker.register('sw.js');
    await navigator.serviceWorker.ready;
  } catch (error) {
    logLine('service worker: ' + error);
    return;
  }
  // once: a page the worker still does not isolate stays as it is
  if (!sessionStorage.getItem('opence-isolating')) {
    sessionStorage.setItem('opence-isolating', '1');
    $('checks-note').textContent = 'Preparing the page (it loads once more)…';
    $('checks-note').hidden = false;
    location.reload();
    await new Promise(() => {});
  }
}

// ---------- the browser's checks

const results = {};

function addCheck(name, state, text, detail) {
  results[name] = state;
  const item = document.createElement('li');
  item.className = state;
  item.append(text);
  if (detail) {
    const span = document.createElement('span');
    span.className = 'detail';
    span.textContent = ' ' + detail;
    item.append(span);
  }
  $('checks').append(item);
}

function workerWebGL2() {
  return new Promise((resolve) => {
    const source = "try { postMessage(!!new OffscreenCanvas(1, 1).getContext('webgl2')); } catch (e) { postMessage(false); }";
    let worker;
    try {
      worker = new Worker(URL.createObjectURL(new Blob([source], { type: 'text/javascript' })));
    } catch {
      resolve(false);
      return;
    }
    const timer = setTimeout(() => { worker.terminate(); resolve(false); }, 5000);
    worker.onmessage = (event) => { clearTimeout(timer); worker.terminate(); resolve(event.data === true); };
    worker.onerror = () => { clearTimeout(timer); worker.terminate(); resolve(false); };
  });
}

function memoryAvailable() {
  try {
    const pages = WEB_MEMORY_BYTES / 65536;
    new WebAssembly.Memory({ initial: pages, maximum: pages, shared: true });
    return true;
  } catch {
    return false;
  }
}

async function runChecks() {
  $('checks').textContent = '';
  addCheck('secure', window.isSecureContext ? 'good' : 'bad', 'A secure page (https)');
  addCheck('isolated', window.crossOriginIsolated ? 'good' : 'bad', 'Threads (cross-origin isolation)',
    window.crossOriginIsolated ? '' : 'reload the page; a private window may not allow it');
  const webgl = typeof OffscreenCanvas !== 'undefined' && await workerWebGL2();
  addCheck('webgl', webgl ? 'good' : 'bad', 'WebGL 2 from a worker');
  const opfs = !!(navigator.storage && navigator.storage.getDirectory);
  addCheck('opfs', opfs ? 'good' : 'bad', "The site's private storage (OPFS)");
  const memory = window.crossOriginIsolated && memoryAvailable();
  addCheck('memory', memory ? 'good' : 'bad', 'Memory for the game (2.1 GB)',
    memory ? '' : 'this browser could not give the game that much: a 64-bit browser on a computer has it');
  const required = ['secure', 'isolated', 'webgl', 'opfs', 'memory'];
  return required.every((name) => results[name] === 'good');
}

// ---------- the game data (xiso-worker.js)

let dataReady = false;
// (every map in, .complete written: a streamed torrent's import is over)
let dataComplete = false;

function formatBytes(bytes) {
  return bytes >= 1e9 ? (bytes / 1e9).toFixed(2) + ' GB' : Math.round(bytes / 1e6) + ' MB';
}

async function refreshData() {
  dataReady = false;
  dataComplete = false;
  let text = 'No game data yet: choose your disc image of Halo: Combat Evolved for the Xbox.';
  try {
    const root = await navigator.storage.getDirectory();
    const maps = await root.getDirectoryHandle('maps');
    try {
      const marker = await (await maps.getFileHandle('.complete')).getFile();
      const complete = JSON.parse(await marker.text());
      text = `${complete.files.length} files of maps (${formatBytes(complete.bytes)}), ready.`;
      dataReady = true;
      dataComplete = true;
    } catch {
      // (a torrent's maps, streamed: the game starts once ui.map is in, the
      // others coming as it runs; xiso-worker.js streamCopy)
      let ready = 0;
      for await (const [name] of maps.entries()) if (name.startsWith('.ready-')) ready++;
      await maps.getFileHandle('.ready-ui.map');
      text = `${ready} of the maps ready; the others are fetched as the game runs.`;
      dataReady = true;
    }
  } catch {
    // none, or a copy that did not finish
  }
  $('data-status').textContent = text;
  $('delete-data').disabled = !dataReady;
  await refreshStorage();
  updatePlay();
}

async function refreshStorage() {
  if (!navigator.storage || !navigator.storage.estimate) return;
  const { usage, quota } = await navigator.storage.estimate();
  const persisted = navigator.storage.persisted ? await navigator.storage.persisted() : false;
  $('storage-status').textContent = `This site keeps ${formatBytes(usage || 0)} of the ${formatBytes(quota || 0)} ` +
    `the browser allows it. ${persisted ? 'The browser keeps it.' : 'The browser may clear it when space runs low.'}`;
  $('persist').disabled = persisted;
}

// copies the maps in (xiso-worker.js); resolves true once they are there.
// hooks: transfer (the message's transferables), onWant (a remote image's
// maps), onFileDone (one of them copied), onWorker (the worker, for bumps),
// status() (more to say beside the copy's progress)
function importData(message, hooks = {}) {
  return new Promise((resolve) => {
    const worker = new Worker('xiso-worker.js');
    if (hooks.onWorker) hooks.onWorker(worker);
    $('import-progress').hidden = false;
    $('import-bar').style.width = '0';
    $('import-text').textContent = hooks.status ? hooks.status() : 'Reading…';
    setImporting(true);
    // (asked before the copy: the maps are 2 GB the browser should not drop)
    if (navigator.storage && navigator.storage.persist) navigator.storage.persist().catch(() => {});
    const finish = async (ok) => {
      worker.terminate();
      setImporting(false);
      await refreshData();
      resolve(ok);
    };
    worker.onmessage = async (event) => {
      const data = event.data;
      if (data.type === 'progress') {
        $('import-bar').style.width = (100 * data.done / data.total).toFixed(1) + '%';
        $('import-text').textContent = `Copying ${data.file}: ${formatBytes(data.done)} of ${formatBytes(data.total)}` +
          (hooks.status ? ` (${hooks.status()})` : '');
        if (hooks.onProgress) hooks.onProgress(data);
        return;
      }
      if (data.type === 'want') {
        if (hooks.onWant) hooks.onWant(data.files);
        return;
      }
      if (data.type === 'file-done') {
        if (hooks.onFileDone) await hooks.onFileDone(data);
        return;
      }
      if (data.type === 'done') {
        $('import-text').textContent = `Copied ${data.files} files.`;
        logLine(`import: ${data.files} files, ${data.bytes} bytes`);
      } else {
        $('import-text').textContent = data.message;
        logLine('import failed: ' + data.message);
      }
      await finish(data.type === 'done');
    };
    worker.onerror = async (event) => {
      $('import-text').textContent = 'The copy failed: ' + event.message;
      await finish(false);
    };
    worker.postMessage(message, hooks.transfer || []);
  });
}

// ---------- the game data from a torrent of the player's own disc image
//
// (local only) ?btih=<infohash> (40 hex or 32 base32 characters) or
// ?torrent=<url of a .torrent> fetches the disc image's maps from peers:
// WebTorrent in the page (webtorrent.min.js), which reaches only WebRTC peers
// found through WebSocket trackers (?tr=wss://..., any number, besides
// TORRENT_TRACKERS) and HTTP web seeds (?ws=<url>). The importer reads the
// image through a port as if it were a file (xiso-worker.js's RemoteFile): its
// file system first, then only the maps' pieces, all asked for at once. The
// pieces wait in the site's storage until the copy is done, then go.

// the maps the importer has streamed in, and what the game waits for:
// web_js_post kind 4 names a map it wants (port/web/src/web_main.c
// web_map_ready), and a line over the canvas says how far it has come
const streaming = { ready: new Set(), count: 0, progress: null, waiting: null, want: () => false, status: null, onBootable: null };

function updateMapWait() {
  const line = $('map-wait');
  if (!line) return;
  const name = streaming.waiting;
  if (!name || streaming.ready.has(name)) {
    line.hidden = true;
    if (name) streaming.waiting = null;
    return;
  }
  const progress = streaming.progress;
  const share = progress && progress.file.toLowerCase() === name && progress.fileSize ?
    ` ${Math.floor(100 * progress.fileDone / progress.fileSize)}%` : '';
  line.textContent = `Fetching ${name}${share}` + (streaming.status ? ` (${streaming.status()})` : '');
  line.hidden = false;
}

// (a map asked for before the torrent knows its maps is asked for again
// once it does: importTorrent's onWant)
function mapWanted(name) {
  const base = String(name).toLowerCase().split(/[\\/]/).pop().replace(/\.map$/, '') + '.map';
  if (streaming.ready.has(base)) return;
  streaming.waiting = base;
  streaming.want(base);
  updateMapWait();
}

const TORRENT_TRACKERS = [
  'wss://tracker.webtorrent.dev',
  'wss://tracker.openwebtorrent.com',
  'wss://tracker.btorrent.xyz',
];

function torrentSource() {
  const btih = (params.get('btih') || '').trim();
  const url = (params.get('torrent') || '').trim();
  const trackers = [...TORRENT_TRACKERS, ...params.getAll('tr').filter((tracker) => /^wss?:\/\/\S+$/.test(tracker))];
  const webSeeds = params.getAll('ws').filter((seed) => /^https?:\/\/\S+$/.test(seed));
  if (/^([0-9a-f]{40}|[a-z2-7]{32})$/i.test(btih)) {
    const magnet = `magnet:?xt=urn:btih:${btih}` + trackers.map((tracker) => '&tr=' + encodeURIComponent(tracker)).join('') +
      webSeeds.map((seed) => '&ws=' + encodeURIComponent(seed)).join('');
    return { id: magnet, trackers, webSeeds, label: btih.slice(0, 12) };
  }
  if (/^https?:\/\/\S+$/.test(url) || /^[\w./-]+\.torrent$/.test(url)) return { id: url, trackers, webSeeds, label: url };
  return null;
}

async function importTorrent(source) {
  $('import-progress').hidden = false;
  $('import-bar').style.width = '0';
  $('import-text').textContent = 'Loading WebTorrent…';
  setImporting(true);
  let client = null;
  let torrent = null;
  try {
    const { default: WebTorrent } = await import('./webtorrent.min.js');
    client = new WebTorrent({ dht: false, lsd: false, tracker: { announce: source.trackers } });
    client.on('error', (error) => logLine('torrent: ' + (error && error.message ? error.message : error)));
    const added = Date.now();
    const status = () => torrent && torrent.ready ?
      `${torrent.numPeers} peer${torrent.numPeers === 1 ? '' : 's'}, ${formatBytes(torrent.downloaded)} fetched, ` +
        `${(torrent.downloadSpeed / 1e6).toFixed(1)} MB/s` :
      `Looking for peers of ${source.label}… ${torrent ? torrent.numPeers : 0} so far ` +
        `(${Math.round((Date.now() - added) / 1000)} s)`;
    const ticker = setInterval(() => {
      if ($('import-text').textContent.startsWith('Copying')) return;
      $('import-text').textContent = status();
    }, 500);
    try {
      torrent = client.add(source.id, {
        announce: source.trackers, urlList: source.webSeeds, deselect: true, destroyStoreOnDestroy: true,
      });
      torrent.on('warning', (warning) => logLine('torrent: ' + (warning && warning.message ? warning.message : warning)));
      // a WebRTC link that drops is not found again until the trackers' next
      // announce, minutes away: ask them again at once when the last peer goes
      // (and every 10 s while there is none)
      const reannounce = () => {
        if (torrent.destroyed || torrent.numPeers > 0 || !torrent.discovery || !torrent.discovery.tracker) return;
        torrent.discovery.tracker.update();
      };
      torrent.on('wire', (wire) => wire.once('close', () => setTimeout(reannounce, 500)));
      const announcer = setInterval(reannounce, 10000);
      torrent.once('close', () => clearInterval(announcer));
      await new Promise((resolve, reject) => {
        torrent.once('ready', resolve);
        torrent.once('error', reject);
      });
      const image = torrent.files.filter((file) => /\.(x?iso)$/i.test(file.name)).sort((a, b) => b.length - a.length)[0];
      if (!image) throw new Error(`The torrent has no disc image (.iso or .xiso) among its ${torrent.files.length} files.`);
      logLine(`torrent: ${torrent.infoHash}, ${image.name}, ${image.length} bytes, ${torrent.numPeers} peers`);
      // the importer's reads, answered from the torrent (end exclusive there,
      // inclusive here); a range not yet fetched is fetched first, at once
      const channel = new MessageChannel();
      channel.port1.onmessage = async (event) => {
        const { id, start, end } = event.data;
        try {
          const data = await image.arrayBuffer({ start, end: end - 1 });
          const bytes = data instanceof Uint8Array ? data : new Uint8Array(data);
          channel.port1.postMessage({ id, bytes }, [bytes.buffer]);
        } catch (error) {
          channel.port1.postMessage({ id, error: String(error && error.message ? error.message : error) });
        }
      };
      // each map's pieces of the torrent: the map's bytes in the disc image
      // (from its file system, read above) over the piece length. The maps
      // are selected in the order the importer copies them, most wanted
      // first; ui.map's are critical, and so are a map's the game waits for
      const maps = new Map();
      // the maps in the order they are to come; the one at the head (and the
      // next, to keep the peers busy) is the torrent's selection
      let order = [];
      let selected = [];
      const selectNext = () => {
        const want = order.filter((name) => !streaming.ready.has(name)).slice(0, 2);
        if (want.join() === selected.join()) return;
        for (const name of selected) {
          const map = maps.get(name);
          if (map && !want.includes(name)) torrent.deselect(map.from, map.to);
        }
        want.forEach((name, index) => {
          const map = maps.get(name);
          torrent.select(map.from, map.to, want.length - index);
          if (index === 0) torrent.critical(map.from, map.to);
        });
        selected = want;
      };
      const piecesOf = (file) => [
        Math.floor((image.offset + file.offset) / torrent.pieceLength),
        Math.floor((image.offset + file.offset + file.size - 1) / torrent.pieceLength),
      ];
      const onWant = async (files) => {
        const needed = files.reduce((sum, file) => sum + file.size, 0);
        if (navigator.storage && navigator.storage.estimate) {
          const { quota, usage } = await navigator.storage.estimate();
          // (the pieces and the copy, both, until the copy is done)
          if (quota && quota - usage < needed * 2) {
            logLine(`torrent: ${formatBytes(quota - usage)} of storage left, ${formatBytes(needed * 2)} wanted at most`);
          }
        }
        // (WebTorrent goes through its selections in order, most important
        // first; neighbouring maps share a piece, and an overlapping selection
        // is merged into one: so each map's pieces go in one at a time,
        // reselected in turn as each map arrives: selectNext)
        files.forEach((file) => {
          const [from, to] = piecesOf(file);
          maps.set(file.name.toLowerCase(), { ...file, from, to });
        });
        order = files.map((file) => file.name.toLowerCase());
        selectNext();
        logLine(`torrent: fetching ${formatBytes(needed)} of maps from ${torrent.numPeers} peers, ` +
          files.map((file) => `${file.name} ${piecesOf(file).join('-')}`).join(', '));
        if (streaming.waiting) streaming.want(streaming.waiting);
      };
      let worker = null;
      streaming.want = (name) => {
        const base = String(name).toLowerCase().split(/[\\/]/).pop().replace(/\.map$/, '') + '.map';
        const map = maps.get(base);
        if (!map || streaming.ready.has(base)) return false;
        order = [base, ...order.filter((name) => name !== base)];
        selectNext();
        if (worker) worker.postMessage({ type: 'bump', name: base });
        logLine(`torrent: the game wants ${base} now`);
        return true;
      };
      streaming.status = status;
      const onFileDone = async ({ name, ready, count }) => {
        streaming.ready.add(name.toLowerCase());
        streaming.count = count;
        selectNext();
        logLine(`torrent: ${name} ready (${ready} of ${count}) after ${Math.round((Date.now() - added) / 1000)} s`);
        if (name.toLowerCase() === 'ui.map' && !gameStarted) {
          await refreshData();
          if (streaming.onBootable) streaming.onBootable();
        }
        updateMapWait();
      };
      const ok = await importData({ type: 'remote', size: image.length, port: channel.port2 },
        { transfer: [channel.port2], onWant, onFileDone, onWorker: (w) => { worker = w; },
          onProgress: (progress) => { streaming.progress = progress; updateMapWait(); }, status });
      streaming.want = () => false;
      logLine(`torrent: ${ok ? 'done' : 'stopped'}, ${torrent.downloaded} bytes fetched`);
      return ok;
    } finally {
      clearInterval(ticker);
    }
  } catch (error) {
    $('import-text').textContent = 'The torrent could not be used: ' + (error && error.message ? error.message : error);
    logLine('torrent failed: ' + (error && error.stack ? error.stack : error));
    return false;
  } finally {
    setImporting(false);
    if (client) await new Promise((resolve) => client.destroy(() => resolve()));
    await removeTorrentPieces(torrent);
  }
}

// the pieces' folder (fsa-chunk-store names it "<name> - <first 8 of the
// infohash>"), if the torrent's own destroy left it
async function removeTorrentPieces(torrent) {
  if (!torrent || !torrent.infoHash) return;
  try {
    const root = await navigator.storage.getDirectory();
    const suffix = ' - ' + torrent.infoHash.slice(0, 8);
    for await (const [name] of root.entries()) {
      if (name.endsWith(suffix)) await root.removeEntry(name, { recursive: true }).catch(() => {});
    }
  } catch {
    // (nothing to remove)
  }
}

function setImporting(importing) {
  for (const id of ['image-label', 'folder-label']) $(id).classList.toggle('disabled', importing);
  $('image-input').disabled = importing;
  $('folder-input').disabled = importing;
  $('play').disabled = importing || !canPlay();
}

$('image-input').addEventListener('change', (event) => {
  const file = event.target.files[0];
  event.target.value = '';
  if (file) importData({ type: 'image', file });
});
$('folder-input').addEventListener('change', (event) => {
  const files = [...event.target.files];
  event.target.value = '';
  if (files.length) importData({ type: 'folder', files });
});

$('delete-data').addEventListener('click', async () => {
  if (!confirm('Delete the maps copied into this browser? Saved games stay.')) return;
  const root = await navigator.storage.getDirectory();
  await root.removeEntry('maps', { recursive: true }).catch(() => {});
  // (and the game's copies of them, save/z/cache000.map and on)
  try {
    const drive = await (await root.getDirectoryHandle('save')).getDirectoryHandle('z');
    for await (const [name] of drive.entries()) {
      if (/^cache\d+\.map$/i.test(name)) await drive.removeEntry(name).catch(() => {});
    }
  } catch {
    // no saved games yet
  }
  await refreshData();
});

$('persist').addEventListener('click', async () => {
  if (navigator.storage && navigator.storage.persist) await navigator.storage.persist();
  await refreshStorage();
});

// ---------- saved games out, as a zip (stored, not compressed)

const crcTable = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c >>> 0;
  }
  return table;
})();

function crc32(bytes) {
  let crc = 0xFFFFFFFF;
  for (let i = 0; i < bytes.length; i++) crc = crcTable[(crc ^ bytes[i]) & 0xFF] ^ (crc >>> 8);
  return (crc ^ 0xFFFFFFFF) >>> 0;
}

function zip(entries) {
  const encoder = new TextEncoder();
  const parts = [];
  const central = [];
  let offset = 0;
  for (const { name, data } of entries) {
    const nameBytes = encoder.encode(name);
    const crc = crc32(data);
    const local = new DataView(new ArrayBuffer(30));
    local.setUint32(0, 0x04034b50, true);
    local.setUint16(4, 20, true);
    local.setUint16(6, 0x0800, true);
    local.setUint32(14, crc, true);
    local.setUint32(18, data.length, true);
    local.setUint32(22, data.length, true);
    local.setUint16(26, nameBytes.length, true);
    const header = new DataView(new ArrayBuffer(46));
    header.setUint32(0, 0x02014b50, true);
    header.setUint16(4, 20, true);
    header.setUint16(6, 20, true);
    header.setUint16(8, 0x0800, true);
    header.setUint32(16, crc, true);
    header.setUint32(20, data.length, true);
    header.setUint32(24, data.length, true);
    header.setUint16(28, nameBytes.length, true);
    header.setUint32(42, offset, true);
    parts.push(local.buffer, nameBytes, data);
    central.push(header.buffer, nameBytes);
    offset += 30 + nameBytes.length + data.length;
  }
  const centralSize = central.reduce((sum, part) => sum + part.byteLength, 0);
  const end = new DataView(new ArrayBuffer(22));
  end.setUint32(0, 0x06054b50, true);
  end.setUint16(8, entries.length, true);
  end.setUint16(10, entries.length, true);
  end.setUint32(12, centralSize, true);
  end.setUint32(16, offset, true);
  return new Blob([...parts, ...central, end.buffer], { type: 'application/zip' });
}

// the saved games and profiles, not the maps the game keeps a copy of as the
// Xbox's hard disk did (save/z/cache000.map and on: hundreds of MB the game
// makes again)
async function collect(directory, prefix, entries) {
  for await (const [name, handle] of directory.entries()) {
    if (/^cache\d+\.map$/i.test(name) || (prefix === 'save/z/' && name === 'maps')) continue;
    if (handle.kind === 'directory') await collect(handle, prefix + name + '/', entries);
    else entries.push({ name: prefix + name, data: new Uint8Array(await (await handle.getFile()).arrayBuffer()) });
  }
}

$('export-saves').addEventListener('click', async () => {
  if (gameStarted && !gameStopped) {
    alert('Quit the game first: its files are open while it runs.');
    return;
  }
  const entries = [];
  const root = await navigator.storage.getDirectory();
  try {
    await collect(await root.getDirectoryHandle('save'), 'save/', entries);
  } catch {
    // no saved games yet
  }
  const config = await readStorageText('config.toml');
  if (config != null) entries.push({ name: 'config.toml', data: new TextEncoder().encode(config) });
  if (!entries.length) {
    alert('There are no saved games yet.');
    return;
  }
  download('opence-saves.zip', zip(entries));
});

$('download-log').addEventListener('click', downloadLog);
$('fatal-log-download').addEventListener('click', downloadLog);
$('fatal-reload').addEventListener('click', () => location.reload());

// ---------- the game

let checksPassed = false;
let gameStarted = false;
let gameStopped = false;
let wakeLock = null;

function canPlay() {
  return checksPassed && dataReady && !gameStarted;
}

function updatePlay() {
  $('play').disabled = !canPlay();
  $('play-note').textContent = !checksPassed ? 'This browser cannot run the game (above).' :
    !dataReady ? 'The game data is needed first.' : '';
}

// init.txt in the data folder: the console commands the game runs as it
// starts. ?map= writes one, and the page removes it on the next visit
// without ?map=, only if it wrote it.
const INIT_MARKER = 'opence-page-wrote-init';

async function prepareInit() {
  const root = await navigator.storage.getDirectory();
  let wrote = false;
  try {
    wrote = localStorage.getItem(INIT_MARKER) === '1';
  } catch {
    // (no localStorage: nothing to undo)
  }
  if (params.has('map')) {
    const name = params.get('map');
    const level = name.includes('\\') ? name : `levels\\${name}\\${name}`;
    if (!/^[A-Za-z0-9_\\]+$/.test(level)) return;
    const handle = await root.getFileHandle('init.txt', { create: true });
    const writable = await handle.createWritable();
    await writable.write(`map_name ${level}\r\n`);
    await writable.close();
    try {
      localStorage.setItem(INIT_MARKER, '1');
    } catch {
      // as above
    }
  } else if (wrote) {
    await root.removeEntry('init.txt').catch(() => {});
    try {
      localStorage.removeItem(INIT_MARKER);
    } catch {
      // as above
    }
  }
}

// ---------- rooms (net.js)

function roomLink(secret) {
  return `${location.origin}${location.pathname}?room=${secret}`;
}

function showRoom(status) {
  const inRoom = !!status;
  $('room-create').hidden = inRoom || gameStarted;
  $('room-copy').hidden = !inRoom;
  $('room-leave').hidden = !inRoom || gameStarted;
  if (!inRoom) return;
  const brokers = status.brokers ? '' : ' (finding the message brokers…)';
  $('room-status').textContent = status.peers ?
    `In a room with ${status.connected} of ${status.peers} other browsers connected${brokers}. Your address: ${status.address}.` :
    `In a room, alone so far${brokers}: send its link to your friends.`;
}

async function joinRoom(secret) {
  const brokers = (params.get('brokers') || '').split(',').filter((url) => /^wss?:\/\//.test(url));
  showRoom(await window.HaloNet.join(secret, brokers));
  history.replaceState(null, '', `?${new URLSearchParams({ ...Object.fromEntries(params), room: secret })}`);
}

window.HaloNet.onChange(showRoom);

$('room-create').addEventListener('click', () => joinRoom(window.HaloNet.newSecret()));
$('room-copy').addEventListener('click', async () => {
  const status = window.HaloNet.status();
  if (!status) return;
  try {
    await navigator.clipboard.writeText(roomLink(status.secret));
    $('room-copy').textContent = 'Copied';
    setTimeout(() => { $('room-copy').textContent = "Copy the room's link"; }, 1500);
  } catch {
    prompt("The room's link:", roomLink(status.secret));
  }
});
$('room-leave').addEventListener('click', () => {
  window.HaloNet.leave();
  params.delete('room');
  history.replaceState(null, '', params.toString() ? `?${params}` : location.pathname);
});

function gameArguments() {
  const args = [];
  // (the game's address in the room: port/web/src/web_net.c)
  if (window.HaloNet.address()) args.push('--HALO_WEB_ADDRESS=' + window.HaloNet.address());
  for (const value of params.getAll('set')) {
    if (/^[A-Z0-9_]+=/.test(value)) args.push('--' + value);
  }
  if (params.has('menu')) args.push('--HALO_MENU_OPEN=' + params.get('menu'));
  // internet play's brokers (port/linux/src/p2p_signal.c), if not its own
  const brokers = (params.get('brokers') || '').split(',').filter((url) => /^wss?:\/\/[^,\s]+$/.test(url));
  if (brokers.length) args.push('--HALO_WEB_BROKERS=' + brokers.join(','));
  // an invite to join (its code, or a whole link)
  const invite = /([0-9a-f]{64})\s*$/i.exec(params.get('join') || '');
  if (invite) args.push('halo://join/' + invite[1].toLowerCase());
  return args;
}

function showFatal(text) {
  gameStopped = true;
  $('fatal-text').textContent = text;
  $('fatal-log').textContent = log.slice(-60).join('\n');
  $('fatal').hidden = false;
  if (document.exitPointerLock) document.exitPointerLock();
}

// a game hosted for the internet: its invite as this page's address with
// ?join= (which native builds take too), and a button that copies it (a
// page may write the clipboard only when it is clicked)
function showInvite(text) {
  const code = /([0-9a-f]{64})/.exec(text);
  if (!code) return;
  $('invite-link').value = `${location.origin}${location.pathname}?join=${code[1]}`;
  $('invite-copy').textContent = 'Copy';
  $('invite').hidden = false;
}

$('invite-copy').addEventListener('click', async () => {
  try {
    await navigator.clipboard.writeText($('invite-link').value);
    $('invite-copy').textContent = 'Copied';
  } catch {
    $('invite-link').select();
  }
});
$('invite-close').addEventListener('click', () => { $('invite').hidden = true; });

function loadScript(src) {
  return new Promise((resolve, reject) => {
    const script = document.createElement('script');
    script.src = src;
    script.onload = resolve;
    script.onerror = () => reject(new Error('could not load ' + src));
    document.head.append(script);
  });
}

async function play() {
  if (!canPlay()) return;
  gameStarted = true;
  watchFrames();
  $('launcher').hidden = true;
  $('update').hidden = true;
  $('game').hidden = false;
  const canvas = $('canvas');
  canvas.focus();
  try {
    if (navigator.wakeLock) wakeLock = await navigator.wakeLock.request('screen');
  } catch {
    // (a phone may still dim)
  }
  try {
    await prepareInit();
    await loadScript('halo.js');
    const options = {
      canvas,
      arguments: gameArguments(),
      print: (text) => { logLine(text); console.log(text); },
      printErr: (text) => {
        logLine(text);
        console.warn(text);
        if (!$('starting').hidden && /data root:/.test(text)) $('starting-text').textContent = 'Loading the menus…';
      },
      haloNetSend: (address, reliable, bytes) => window.HaloNet.send(address, reliable, bytes),
      // internet play's WebRTC connections (p2p.js)
      haloP2P: (command, connection, bytes) => window.HaloP2P.command(command, connection, bytes),
      haloMessage: (kind, text) => {
        logLine(text);
        if (kind === 2) showInvite(text);
        if (kind === 3) showFatal(text);
        // (local only: a map the game waits for: port/web/src/web_main.c)
        if (kind === 4) mapWanted(text);
      },
      onAbort: (what) => showFatal('The game stopped: ' + what),
      onExit: (status) => {
        if (status) showFatal(`The game stopped (status ${status}). The log below says why.`);
        else location.reload();
      },
    };
    // (the module these options become, which p2p.js calls into as soon as
    // the game asks it for a connection)
    window.HaloP2P.attach(options);
    game = await window.createHalo(options);
    // the on-screen touch controls (touch.js), and the room's frames (net.js)
    window.HaloTouch.start(game, $('touch'));
    window.HaloNet.attach(game);
  } catch (error) {
    showFatal(String(error && error.message ? error.message : error));
  }
}

// While the game wants the mouse for the aim (its word in web_shared.h,
// port/web/src), a click on it takes the pointer lock back, from the click
// itself: the game's own request comes from its thread, after the click, and
// the browser refuses that once the player has let the lock go (Escape).
const MOUSE_WANTED_WORD = 48;

$('canvas').addEventListener('pointerdown', (event) => {
  const canvas = event.currentTarget;
  if (event.pointerType !== 'mouse' || !game || !game._web_state || document.pointerLockElement === canvas) return;
  if (!game.HEAPU32[(game._web_state() >> 2) + MOUSE_WANTED_WORD]) return;
  const request = canvas.requestPointerLock();
  // (refused for a moment after Escape: the next click asks again)
  if (request && request.catch) request.catch(() => {});
});

// The game counts the frames it shows in its memory (web_state,
// port/web/src/web_main.c), which the page reads: while the count stands
// still, the game is starting or loading a map, and the page says so.
let game = null;

function watchFrames() {
  let shown = -1;
  let changed = performance.now();
  const timer = setInterval(() => {
    if (gameStopped) {
      clearInterval(timer);
      $('starting').hidden = true;
      return;
    }
    if (!game || !game._web_state) return;
    const now = game.HEAPU32[game._web_state() >> 2];
    if (now !== shown) {
      if (shown < 0 && now === 0) return;
      shown = now;
      changed = performance.now();
      $('starting').hidden = true;
    } else if (performance.now() - changed > 600 && $('starting').hidden) {
      $('starting-text').textContent = 'Loading…';
      $('starting').hidden = false;
    }
  }, 200);
}

$('play').addEventListener('click', play);

$('fullscreen').addEventListener('click', async () => {
  try {
    if (document.fullscreenElement) await document.exitFullscreen();
    else await $('game').requestFullscreen({ navigationUI: 'hide' });
    // (Escape is the game's in fullscreen too, where the browser lets it)
    if (navigator.keyboard && navigator.keyboard.lock && document.fullscreenElement) {
      await navigator.keyboard.lock(['Escape']).catch(() => {});
    }
  } catch (error) {
    logLine('fullscreen: ' + error);
  }
});

$('leave').addEventListener('click', () => {
  if (confirm('Quit the game? Progress since the last checkpoint is lost.')) location.reload();
});

// Tab is the game's (switch weapons, the scoreboard), not the page's focus;
// the key still reaches the game (SDL listens on the window)
window.addEventListener('keydown', (event) => {
  if (gameStarted && !gameStopped && event.key === 'Tab') event.preventDefault();
}, true);

$('canvas').addEventListener('click', () => $('canvas').focus());
$('canvas').addEventListener('contextmenu', (event) => event.preventDefault());

document.addEventListener('visibilitychange', async () => {
  if (gameStarted && !gameStopped && document.visibilityState === 'visible' && navigator.wakeLock && !wakeLock) {
    try {
      wakeLock = await navigator.wakeLock.request('screen');
    } catch {
      // as before
    }
  }
  if (document.visibilityState === 'hidden') wakeLock = null;
});

// ---------- start

(async () => {
  await isolate();
  await loadStamp();
  checksPassed = await runChecks();
  if (!checksPassed) {
    $('checks-note').textContent = 'Desktop Chrome, Edge and Firefox run the game; Safari 17 or later may.';
    $('checks-note').hidden = false;
  }
  if (results.opfs === 'good') await refreshData();
  else updatePlay();
  checkForUpdate();
  if (/^[0-9a-f]{32}$/.test(params.get('room') || '')) await joinRoom(params.get('room'));
  // (local only: the maps from a torrent of the player's disc image, then
  // the game; once they are here, straight to the game)
  // the maps from a torrent come in as the game runs: it starts as soon as
  // ui.map is in (or at once, when an earlier visit left it), and the import
  // carries on behind it until .complete (xiso-worker.js streamCopy)
  const torrent = torrentSource();
  if (torrent && checksPassed) {
    if (!dataComplete) {
      streaming.onBootable = () => play();
      if (dataReady) play();
      await importTorrent(torrent);
    }
    if (dataReady) play();
  }
  if (params.has('play')) play();
})();
