# macOS

A native app for Apple silicon Macs (macOS 14.4 or later) that draws with
Metal.

## Play

1. Put the game's `maps` folder (from your own disc image, as for the other
   ports: [Game data](../../README.md#game-data)) in `~/Games/Halo`, so that
   `~/Games/Halo/maps/ui.map` exists. `HALO_DATA_ROOT` names another folder.
2. Open `OpenCE.app`. A notarized build (`OpenCE-macos.dmg`, see
   "Signing and notarization") opens as it is. One signed only ad hoc (the
   workflow's artifact, without the signing secrets) is quarantined by
   macOS once downloaded: take that off once with
   `xattr -dr com.apple.quarantine OpenCE.app`, or right-click it and choose
   Open.

The game starts in a window. **Command-F** (or F11, or the window's green
button) switches to fullscreen and back, and **Command-Q** quits.
`config.toml`, `debug.txt` and the other files are in the data folder; the
saves are in `~/Library/Application Support/Halo`. The settings, controls
and console are the other desktop builds' ([port/linux/README.md](../linux/README.md)),
with `display.fullscreen` off by default here.

## How it works

The game's code assumes 32-bit pointers, which a 64-bit macOS process cannot
give native code. So the game is compiled to WebAssembly (wasm32, with
wasi-libc), and wabt's `wasm2c` turns that into C, which the Mac's clang
compiles to arm64 code in which every pointer is an offset into one block of
memory. The game is the guest, `port/macos/guest`; the program around it,
`port/macos/host`, makes its memory and threads and gives it the system's
calls, SDL 3 and Metal.

- **Memory.** 2.5 GB, of which the top 512 MB is the Xbox's memory window,
  as on the other desktop builds. wasm2c reserves guard pages after it, so a
  32-bit offset needs no bounds check.
- **Threads.** Each of the game's threads runs on a host thread; the atomics
  stay atomic, and waits go to `os_sync_wait_on_address`.
- **Drawing.** `guest/d3d8_metal.c` is the game's Direct3D 8 device, as
  `port/linux/src/d3d8_gl.c` is on OpenGL; it hands draws, textures and
  state to `host/host_metal.m`, which owns every Metal object. The game's
  vertex and pixel shaders are translated to Metal Shading Language by the
  same translators as the other ports' GLSL (`nv2a_vsh.c`, `nv2a_psh.c`,
  under `HALO_METAL`), and compiled when first drawn. Up to three frames are
  in flight.

On an M4 Mac mini, uncapped (`HALO_NO_VSYNC=1 HALO_MAX_FPS=-1`): Blood
Gulch at 6016x3384 about 234 frames a second, at 1080 lines about 550; The
Pillar of Autumn about 390.

## Build

You need the Command Line Tools (`xcode-select --install`) and
[uv](https://docs.astral.sh/uv/) or CMake and ninja. Then:

```sh
port/macos/tools/setup_toolchain.sh   # wasi-sdk and the patched wabt, in ~/toolchains
git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
~/emsdk/emsdk install 6.0.10 && ~/emsdk/emsdk activate 6.0.10   # its clang compiles the guest
python3 configure.py
ninja macos                           # build/macos/OpenCE.app
```

`HALO_TOOLCHAINS` and `EMSDK` name other folders for the tools. The build
downloads SDL 3's source into `build/macos/third_party`. The wabt patch
(`tools/wabt-1.0.42-opence.patch`) sends `memory.atomic.wait`/`notify` to the
host and adds `--unshared-memory`.

`python3 tools/ci_build.py macos release` builds the app into
`dist/halo-macos-release` and signs it ad hoc, and
`python3 tools/macos_check.py dist/halo-macos-release/OpenCE.app` checks it:
the bundle, and that the game starts, opens Metal and exits by itself
without game data. The GitHub workflow `.github/workflows/macos.yml` runs both on a standard `macos-15`
runner (free for public repositories) and uploads the app.

`tools/app_icon.py --macos` makes the app's icon, `OpenCE.icns`.

## Signing and notarization

An app signed ad hoc (as `ci_build.py` signs it) runs on the Mac that
built it, but one downloaded elsewhere is stopped by Gatekeeper until its
quarantine is taken off. To give others an app that opens with a
double-click, sign it with a Developer ID and have Apple notarize it:

```sh
python3 tools/macos_release.py dist/halo-macos-release/OpenCE.app
```

It signs the app under the hardened runtime with `OpenCE.entitlements`
(only the microphone, for voice chat: the game is compiled ahead of time
and needs no JIT), sends it to Apple's notary service, staples the ticket,
and packs `OpenCE-macos.dmg` (the app and a link to Applications), signed,
notarized and stapled too.

What it needs, from an [Apple Developer Program](https://developer.apple.com/programs/)
membership:

1. A **Developer ID Application** certificate (developer.apple.com, Certificates),
   in the keychain. `MACOS_SIGN_IDENTITY` names it when there are several.
2. Credentials for the notary service, either:
   - an App Store Connect API key (App Store Connect, Users and Access,
     Integrations, Team Keys, with the Developer role): `MACOS_NOTARY_KEY`
     (the `.p8` file, or its text), `MACOS_NOTARY_KEY_ID` and
     `MACOS_NOTARY_ISSUER`; or
   - a profile saved in the keychain with `xcrun notarytool store-credentials
     OpenCE --apple-id <id> --team-id <team>` (an app-specific password):
     `MACOS_NOTARY_PROFILE=OpenCE`.

Without notary credentials it signs and packs only; `--sign-only` does so
on purpose. `MACOS_SIGN_IDENTITY=-` signs ad hoc under the hardened
runtime, to check that the game runs under it.

The workflow does the same when the repository has these secrets:
`MACOS_CERTIFICATE` (the certificate with its private key, exported as a
`.p12`, base64: `base64 -i certificate.p12 | pbcopy`),
`MACOS_CERTIFICATE_PASSWORD` (the `.p12`'s), and `MACOS_NOTARY_KEY`,
`MACOS_NOTARY_KEY_ID` and `MACOS_NOTARY_ISSUER`; and, if needed, the
variable `MACOS_SIGN_IDENTITY`. It puts the certificate in a keychain of
its own and deletes it afterwards, and never signs a pull request's code.
Without the secrets it uploads the app signed ad hoc, as before.

## Debugging

| Variable | |
| --- | --- |
| `HALO_GPU_STATS=1` | draws, pipelines and uploads a frame, in `debug.txt` |
| `HALO_GPU_DUMP_SHADERS=<folder>` | writes each translated shader there |
| `HALO_SCREENSHOT_DIR`, `HALO_SCREENSHOT_EVERY=<n>` | a BMP of every n-th frame |
| `HALO_METAL_DEBUG=<bits>` | 1 no culling, 2 no depth test, 4 white, 8 the first texture, 16 the diffuse colour, 32 the uniforms |
| `HALO_METAL_NO_BC=1` | DXT textures converted as on a GPU without BC formats (the Apple TV), per `display.texture_format` |
| `HALO_METAL_SHADER_CACHE` | The shader cache's record: a file name in the app's caches, a path, or `0` for none (default `metal_shaders.manifest`); `HALO_METAL_SHADER_CACHE_RESET=1` empties it first |
| `HALO_METAL_SHADER_PACK` | The bundled shader pack to read instead of the bundle's `metal_shaders.pack` (`tools/shader_pack.py`), or `0` for none |
| `HALO_METAL_SHADER_PREPARE_JOBS` | Shaders a map's preparation compiles at once at its load (default 4) |
| `HALO_METAL_SHADER_PREPARE_NEXT` | Seconds into a map before the next map's shaders are compiled ahead at a low priority (default 120, `0` never) |
| `HALO_METAL_TEXTURE_SHARED=1` | the converted DXT textures in shared memory, written by the CPU, instead of the GPU's own (which it may compress losslessly) |
| `HALO_TELEMETRY=on` | the play telemetry (`host_telemetry.h`; on by default only on the TV): `HALO_TELEMETRY_DIR`, `HALO_TELEMETRY_HITCH_MS` (50), `HALO_TELEMETRY_KEEP` (40 sessions), `HALO_TELEMETRY_MAX_MB` (200) |
| `HALO_HALF_RES_CYCLE=<n>` | the half-resolution features (`display.half_res_*`) off and on in alternate blocks of n frames, whatever the settings, to compare them in one run (`tools/halfres_compare.py`) |
| `HALO_MEMORY_WARNING_AFTER=<s>` | a memory warning as the system's, once, that many seconds after the first frame: the trim and the telemetry's "memory" event (`host_lifecycle.h`; devicectl's `sendMemoryWarning` fails on the TV) |
| `MTL_DEBUG_LAYER=1` | Metal's validation |

## Not yet

Anti-aliasing (FXAA, SMAA, MSAA) and the screenshot key are not done on
Metal; Intel Macs and iOS are not built.
