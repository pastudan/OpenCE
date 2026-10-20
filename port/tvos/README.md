# Apple TV development port — feasibility stage

**There is not a playable tvOS build yet.** This branch first checks that
our native runtime/Metal assumptions and the hybrid torrent dependencies
can be built for Apple TV. Do not mistake the probe app for OpenCE.

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
- Local device probe compilation also passes. Development provisioning,
  physical TV runtime checks and gameplay validation are still pending.

## Remaining gates

- Native WebRTC-only interoperability with the existing WebTorrent seeder,
  ordinary peer tests on tvOS, and both transports sharing one store.
- The game's AppleTVOS target, UIKit/SDL lifecycle, sandboxed paths and
  memory budgets, including a capability-gated BC texture fallback.
- Shared integration of the bounded image-source API (`import/image_source.c`
  has synthetic bounds/name/cycle/header tests, but is not wired into the
  existing importer yet), HTTP range and verified-piece adapters, import UI,
  cancellation, resume and cache/save recovery.
- Development signing and physical Apple TV gameplay tests.

The macOS branch and its notarization work remain independent. No store
submission or upstream pull request is performed by this feasibility work.
