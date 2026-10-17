# macOS

A native app for Apple silicon Macs (macOS 14.4 or later) that draws with
Metal.

## Play

1. Put the game's `maps` folder (from your own disc image, as for the other
   ports: [Game data](../../README.md#game-data)) in `~/Games/Halo`, so that
   `~/Games/Halo/maps/ui.map` exists. `HALO_DATA_ROOT` names another folder.
2. Open `OpenCE.app`. A build downloaded from GitHub is signed only ad hoc,
   so macOS quarantines it: take that off once with
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

## Debugging

| Variable | |
| --- | --- |
| `HALO_GPU_STATS=1` | draws, pipelines and uploads a frame, in `debug.txt` |
| `HALO_GPU_DUMP_SHADERS=<folder>` | writes each translated shader there |
| `HALO_SCREENSHOT_DIR`, `HALO_SCREENSHOT_EVERY=<n>` | a BMP of every n-th frame |
| `HALO_METAL_DEBUG=<bits>` | 1 no culling, 2 no depth test, 4 white, 8 the first texture, 16 the diffuse colour, 32 the uniforms |
| `MTL_DEBUG_LAYER=1` | Metal's validation |

## Not yet

Anti-aliasing (FXAA, SMAA, MSAA) and the screenshot key are not done on
Metal; Intel Macs and iOS are not built.
