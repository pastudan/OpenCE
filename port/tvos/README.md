# Apple TV development port — feasibility stage

**Development port, not ready for sideload play yet.** The initial game
host builds for device and simulator; the menu and a10 have run through
Metal in the tvOS simulator. Import/source-selection UI, completed lifecycle
and sandbox integration, provisioning and physical TV checks are unfinished.
The separate feasibility probe app is not the game.

## Prerequisites

- Full Xcode, including the tvOS device SDK and simulator runtime. Command
  Line Tools alone are insufficient. Check with:

  ```sh
  xcode-select -p
  xcrun --sdk appletvos --show-sdk-path
  xcrun --sdk appletvsimulator --show-sdk-path
  ```

  If Xcode is installed elsewhere, use `DEVELOPER_DIR` pointing at its
  `Contents/Developer` directory. Installing Xcode, accepting its license,
  and selecting a development team are actions for the machine's owner.
- An Apple TV paired in Xcode's Devices and Simulators window, Developer
  Mode enabled where required, and a development signing team/profile.
  A Mac Developer ID signature and notarized DMG do **not** sign a tvOS app.
- CMake, ninja, Python 3, Perl, make, git and Apple's clang.

## Initial game-host build

From the repository root, with the Mac guest toolchains already installed:

```sh
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
python3 tools/tvos_build.py --platform simulator
python3 tools/tvos_build.py --platform device
```

Outputs: `build/tvos/{simulator,device}/OpenCETV.app`. The device bundle is
unsigned and still needs Apple development provisioning. This reuses the
Mac generator's guest/wasm2c outputs but builds every native object and SDL
for the selected TV SDK, with explicit guest-memory bounds checks. It does
not link Mac host binaries. Metal decodes DXT textures to BGRA when BC
compression is unavailable.

A simulator-only smoke test can use the owner's existing extracted maps:

```sh
python3 tools/tvos_game_check.py build/tvos/simulator/OpenCETV.app \
  --data-root /path/holding/maps --map a10
```

This creates/removes only its own simulator, requires frame output and a
timed clean exit, and does not prove visual correctness or real controller
play. It temporarily uses a loopback debug console for the map request.
No maps are put into the app, repository or CI artifacts.

## Feasibility probes

```sh
python3 tools/tvos_probe.py --platform macos       # build + loopback TCP test
python3 tools/tvos_probe.py --platform tvos        # unsigned device probe
python3 tools/tvos_probe.py --platform simulator  # simulator probe
```

Sources are pinned in `tools/tvos_probe.py`, downloaded to
`build/tvos-probe/deps`, and built separately for each SDK. The TV probe
shows Metal device/buffer operation, an 8 GiB virtual reservation with one
Xbox-window page committed, atomic wait/wake, and a hash-verified generated
file transfer between two loopback-only torrent sessions. The tests use
no copyrighted game data and contact no public trackers/DHT/router.

The unsigned device app still needs development provisioning/signing
before installation. No signing secrets are used by the independent
`.github/workflows/tvos.yml` feasibility workflow. Its artifact is explicitly
named **not-the-game**.

An address-space reservation is **not** a physical-memory stress test.
Simulator success is **not** evidence of Apple TV GPU performance. The
TV probe does not yet test SDL lifecycle, the wasm2c fault handler,
gameplay, or WebTorrent peers on the TV itself. Those are separate gates.

The host-only WebRTC fixture uses the existing seeder's WebTorrent 3.0.21
and a private loopback WebSocket tracker. It disables ordinary TCP/uTP,
DHT and web-seed fallback, retrieves magnet metadata, verifies pieces and
compares payload bytes independently on both sides. Run it with:

```sh
npm ci --prefix port/tvos/probe/test-runtime
node port/tvos/probe/webtorrent_fixture.mjs \
  --modules port/tvos/probe/test-runtime \
  --client build/tvos-probe/macos/build/opence_rtc_probe
```

Both the ordinary TCP and WebRTC-only fixtures passed locally on macOS.
This is not yet a tvOS interoperability or simultaneous hybrid-swarm test.

## Network stack being evaluated

The preferred native engine is **libtorrent-rasterbar 2.1.2**, built with
its **WebTorrent support enabled**. It combines ordinary TCP/uTP peers
and WebRTC DataChannel peers through libdatachannel; no V8/Bun/Node runtime
is needed. Compilation alone does not prove either transport works on tvOS.

Relevant source/version/license pins:

| Dependency | Pin | License |
| --- | --- | --- |
| libtorrent-rasterbar | `6da363d2994f17c0b3c0450d124cf73a31a73847` (v2.1.2) | BSD-3-Clause |
| libdatachannel | `a02b751917ac8afc8c58dc6f4461d25ff9465d48` (pinned libtorrent submodule) | MPL-2.0 |
| OpenSSL | `286ddeaac037533bbdce65b3c689e3f7ffebf0f6` (3.5.6) | Apache-2.0 |
| Boost | 1.88.0, archive SHA-256 in the build script | Boost Software License 1.0 |

Further dependencies are the pinned recursive submodules (ICE/SCTP,
logging and headers); media is disabled. A complete shipped notices/source
offer inventory remains required before distributing the game. Downloads
are build dependencies, not game data, and are not committed to the repo.

The existing WebTorrent browser importer is on the separate
`web-torrent-import` branch. Its useful design is a size-aware remote
`read_at` source: first read the image directory, then request pieces
intersecting the maps. Its browser runtime/storage is not portable tvOS code.

## HUD, text and saved render settings

The TV app embeds the shared Xbox high-resolution HUD, controller glyphs,
fonts and menu titles, plus the gap-filling hint/weapon assets. No separate
TV asset import is required. Initial hardware tests explicitly disabled
`display.high_res_hud` and `display.high_res_text`; that was a test setting,
not an absent feature. Both are now enabled in the owner's device config
and 4K launch session. The device reports high-resolution HUD registration
and Overpass text, and continues rendering a10. Visual correctness and peak
memory still need validation.

Startup no longer supplies the initial `HALO_RESOLUTION_SCALING=720`
override: saved `display.resolution_scaling` is respected (explicit launch
overrides remain possible). High-resolution HUD/text keep their shared
config defaults and remain user-selectable rather than being forced on.

The newer upstream HUD changes reviewed here add Custom Edition bitmap
aliases and asynchronous PNG decoding, not replacement Xbox PNG artwork.
Those code changes have not been merged into this TV branch.

## Planned import and storage behavior

The eventual controller-friendly launcher will accept a user-supplied
HTTP(S) ISO/XISO URL or a BitTorrent magnet/BTIH with optional tracker URLs.
WebRTC peers need a compatible WebSocket tracker; a bare infohash does not
provide one. Multiple images must prompt for selection. Import only
hash-verified bytes; validate image/map bounds and stage updates safely.
No bundled game torrents and no hidden background seeding. The source UI
will identify BitTorrent and explain any uploading during import.

Bulk game data and local saves must use tvOS-approved locations and may
be purged by the OS. Detect eviction and offer reimport; durable save
backup/synchronization must be implemented or the limitation clearly shown.
Direct NFS/SMB support is deferred, not silently replaced with a companion
service. A normal HTTP server on a user's Mac/NAS is the first direct-URL
source.

## Validation so far

- macOS: native libtorrent with WebTorrent enabled builds; ordinary TCP
  and WebRTC-only generated-payload tests pass. The WebRTC peer is the same
  WebTorrent 3.0.21 version used by the existing seeder.
- 24 synthetic dependency/archive and image-reader tests pass locally.
  Read-at discovery of the user's local image found 24 map files using
  53,336 bytes of metadata/header reads; no game data is included here.
- GitHub AppleTVOS and simulator cross-builds pass, as does host WebRTC
  interoperability. The first execution check exposed a missing scene
  lifecycle; apps built with the new SDK must adopt it.
- With Xcode 27.1 and the tvOS 27.0 runtime installed locally, the scene-based
  probe passes Metal buffer work, address reservation, wait/wake and TCP
  transfer in the simulator. It reports no BC compression support and a
  256 MiB maximum Metal buffer. Those are simulator results, not physical
  Apple TV capabilities or gameplay measurements.
- Development signing/provisioning and installation succeeded on an Apple TV
  4K (2nd generation), tvOS 27.0. Its A12 GPU passes buffer work; atomic
  wait/wake and the verified TCP fixture pass. BC compression is unavailable;
  max Metal buffer is 1536 MiB. The Mac's 8 GiB guard reservation fails with
  ENOMEM, but the TV target's explicit-bounds 2.5 GiB reservation and Xbox
  offset page test pass. This does not measure peak resident memory.
- The signed game menu and a10 rendered on that physical TV, with timed
  exit code 0 and no skipped shaders after warm-up. Only the owner's ui.map
  and a10.map were copied into the app cache for this development test.
  The owner confirmed Xbox-mode 8BitDo input and 1080p work after excluding
  the Siri Remote from SDL's gamepad list and disabling touch-to-mouse
  synthesis. This is now set before SDL initialization on TV builds; Mac
  initialization is unchanged. Full controller mapping, the campaign and
  extended memory/stability checks remain incomplete.
- Live a10 samples on the A12 TV: about 59–60 fps at 1920x1080 and about
  35 fps at 3840x2160. These use 60-frame log increments over short live-play
  windows, not matching fixed scenes or percentile frame-time measurements.
  4K is a working experiment, not a demonstrated 60 fps setting.

## Opening fog and scope performance

See [PERFORMANCE.md](PERFORMANCE.md) for the opening-a10 investigation and
measurement limits, and its "Baseline (A12 device)" for fixed-scene frame
times on the TV (tools/tvos_deploy.py, tools/tvos_bench.py, HALO_FRAME_LOG). The owner's scope-pass optimization has been imported
without merging unrelated upstream work. Opt-in `HALO_METAL_PROFILE=1`
records effect invocation totals, fog/transparent draw attempts, feedback
snapshots, CPU shader/pipeline compilation, and sampled command-buffer GPU
time. It does not time individual fog phases or establish A12 gains from
simulator measurements. Duplicate target snapshots within a draw have been
reduced; the fog's appearance is unchanged. These changes are not yet
installed over the owner's running TV session.

## Remaining gates

- Native WebRTC-only interoperability with the existing WebTorrent seeder,
  ordinary peer tests on tvOS, and both transports sharing one store.
- Finish the game's UIKit/SDL lifecycle, restricted WASI preopens and
  physical-device memory budgets. Initial targets and BC decoding exist,
  but are not a completed TV product.
- Shared integration of the bounded image-source API (`import/image_source.c`
  has synthetic bounds/name/cycle/header tests, but is not wired into the
  existing importer yet), HTTP range and verified-piece adapters, import UI,
  cancellation, resume and cache/save recovery.
- Repeatable development-signing/install tooling, controller/visual checks,
  extended physical-TV gameplay and memory validation.

The macOS branch and its notarization work remain independent. No store
submission or upstream pull request is performed by this feasibility work.
