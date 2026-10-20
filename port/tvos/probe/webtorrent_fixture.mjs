// Asset-free interoperability fixture. Match the existing seeder's
// WebTorrent 3.0.21 and tracker 11.2.3; no public tracker/DHT/UPnP or ISO.
// Install dependencies in scratch, pass their directory with --modules.
import { createRequire } from 'node:module';
import { pathToFileURL } from 'node:url';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';

const args = process.argv.slice(2);
const modules = args[args.indexOf('--modules') + 1];
const executable = args[args.indexOf('--client') + 1];
if (!args.includes('--modules') || !args.includes('--client')) {
  throw new Error('webtorrent_fixture.mjs --modules <package dir> --client <opence_rtc_probe>');
}
const require = createRequire(path.join(path.resolve(modules), 'package.json'));
const { default: WebTorrent } = await import(pathToFileURL(require.resolve('webtorrent')));
// This package exposes only ESM import conditions, not require.resolve.
const { Server } = await import(pathToFileURL(path.join(path.resolve(modules),
  'node_modules/bittorrent-tracker/index.js')));
const root = await fs.mkdtemp(path.join(os.tmpdir(), 'opence-rtc-'));
let client;
let server;
try {
  server = new Server({ udp: false, http: false, ws: true, stats: false });
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  const tracker = `ws://127.0.0.1:${server.http.address().port}`;
  const payload = Buffer.alloc(1024 * 1024 + 137);
  for (let i = 0; i < payload.length; ++i) payload[i] = (i * 31 + (i >>> 8) * 17) & 255;
  const filename = path.join(root, 'fixture.bin');
  await fs.writeFile(filename, payload);
  const download = path.join(root, 'download');
  await fs.mkdir(download);
  client = new WebTorrent({ dht: false, lsd: false, natUpnp: false, natPmp: false, utp: false,
    uploadLimit: 2 * 1024 * 1024, tracker: { rtcConfig: { iceServers: [] } } });
  client.on('error', error => console.error('WebTorrent:', error.message));
  const torrent = await new Promise((resolve, reject) => {
    client.once('error', reject);
    client.seed(filename, { announce: [tracker] }, resolve);
  });
  let rtcSeen = false;
  torrent.on('wire', wire => {
    console.log(`WebTorrent peer transport: ${wire.type}`);
    if (wire.type === 'webrtc') rtcSeen = true;
  });
  const magnet = `magnet:?xt=urn:btih:${torrent.infoHash}&tr=${encodeURIComponent(tracker)}`;
  const code = await new Promise((resolve, reject) => {
    const child = spawn(path.resolve(executable), [magnet, download, filename], { stdio: 'inherit' });
    const timer = setTimeout(() => child.kill('SIGKILL'), 90000);
    child.once('error', error => { clearTimeout(timer); reject(error); });
    child.once('exit', (code, signal) => { clearTimeout(timer); resolve(signal ? 1 : code); });
  });
  if (code !== 0 || !rtcSeen) throw new Error(`WebRTC fixture failed: client ${code}, RTC peer observed ${rtcSeen}`);
  const received = await fs.readFile(path.join(download, 'fixture.bin'));
  if (!received.equals(payload)) throw new Error('Independent Node payload comparison failed');
  console.log('PASS: native libtorrent ↔ WebTorrent 3.0.21, WebRTC-only verified transfer');
} finally {
  if (client) await new Promise(resolve => client.destroy(resolve));
  if (server) await new Promise(resolve => server.close(resolve));
  await fs.rm(root, { recursive: true, force: true });
}
