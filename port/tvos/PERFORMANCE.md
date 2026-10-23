# Opening a10 fog investigation

## Scope and provenance

Investigated on `tvos-metal` after the owner's A12 Apple TV tests: 4K looks
acceptable through most of a10, with a pronounced slowdown during the
opening fog sequence. Do not characterize the entire level as 35 fps.
The earlier ~35 fps at 4K and ~60 fps at 1080p were coarse live samples,
not matching fixed-scene benchmarks.

The owner's scope optimization is commit `e6fdb07c` on
`pr/scope-zoom-passes` (equivalent work on `perf/zoom`). GitHub PR #392 was
reported OPEN by the API at investigation time, even though a local
remote-tracking history contains a merge of it. The patch itself was
cherry-picked with attribution as `74b59d4e`, not a broad upstream merge.

## What the scope fix does

`source/rasterizer/xbox/rasterizer_xbox_screen_effect.c` previously multiplied
convolution pass count by output scale: at 2160 lines, two stock passes
became ten. The fix keeps the stock radius and adds sparse refinement
passes, four total at 4K. It applies to convolution effects, including
weapon scopes, not every kind of fog or transparency.

11 imported harness checks pass locally using a compiler wrapper that
removes `-m32` (macOS cannot link the harness's normal i386 executable).
This is native host algorithm coverage, not Linux/i386 validation or a
physical-TV scope benchmark.

## What actually ran during the opening

Opt-in `HALO_METAL_PROFILE=1` adds:

- Screen-effect parameters/pass-count markers, including interval counts
  of every invocation so a brief effect is not discarded by sampling.
- Existing rasterizer phase tags: draw attempts and feedback snapshots for
  fog, fog-screen, queued transparents and screen effects.
- CPU time in runtime Metal shader and render-pipeline compilation.
- Sampled command-buffer GPU time and completion cadence. Completion
  cadence is NOT GPU execution time. Explicit screenshot/readback paths
  may split a frame into command buffers, so final-buffer timing is not a
  universal whole-frame GPU measurement.

An isolated tvOS simulator run at 3840x2160, a10 from fresh launch through
its opening, recorded 58 intervals with **zero screen convolution calls**.
Fog-plane geometry and queued transparent draws were active (roughly
32 fog draws and 46 transparent draws in one opening section). Those draw
counts do NOT measure phase GPU time. This establishes that the tested
opening is not paying the scope's ten-pass convolution cost.

First-use CPU shader compilation was substantial in that simulator:
several frames spent roughly 0.4–1.1 seconds compiling libraries, with
additional pipeline work. These are actual host measurements, but are
NOT the A12 TV's timings or proof of the full slowdown's cause. They can
explain discrete hitches; sustained 4K fill/overdraw cost remains to be
measured on the physical TV.

## Safe copy reduction

The Metal host previously snapshotted the same current color target for
each matching texture stage, even within one draw. With four matching
stages, the same pre-draw image was copied four times into the same scratch
texture. A draw-local snapshot now serves all matching stages. A new draw
always takes a new snapshot, so writes between draws remain visible.

A sampled 4K opening draw went from **four snapshots / 126.56 MiB copied**
to **one / 31.64 MiB** in repeated simulator runs. These count payload bytes,
not total memory-bus traffic. Scene timing can drift between launches;
this is evidence of eliminated duplicate copies, not a claimed TV fps gain
or pixel-perfect image comparison. The fake-resource regression harness
checks 1/2/4 matching stages, updated target contents on the next draw, and
no copy for nonmatching stages. Actual GPU image comparison/validation is
still pending.

## Next measurements and possible work

(Superseded by "Baseline (A12 device)" below, which measured item 1 on the
TV and found the slow fog to be the cryo bay after the cinematic.)

1. Install the instrumented build during an agreed restart and capture the
   same opening on the A12 TV. Keep input/camera, 4K and high-res HUD settings
   consistent; account for cold versus warm shader compilation.
2. Capture GPU phase costs for fog-plane and transparent work separately.
   Temporarily bypassing one phase is a diagnostic negative control only,
   never the shipping visual fix.
3. If CPU compilation dominates hitches, investigate loading-time shader
   preparation and a compatible on-disk library/pipeline cache. Moving
   compilation must preserve variant keys, fallback behavior and thread/
   Metal ownership; caching only pipeline state does not eliminate source
   library compilation.
4. If GPU fill dominates, investigate redundant target transitions, active
   shader texture usage and overdraw before considering lower-resolution
   effects. Any quality trade-off requires an explicit setting and visual
   comparison, not silently removing fog.

## Validation limits

Simulator opening smoke tests pass before and after the copy change;
Mac build/startup and AppleTVOS device compilation pass. 34 focused TV,
parser and Metal diagnostic/copy checks plus the 11 scope checks pass.
The new instrumented/copy/zoom build has **not been deployed over the
owner's running TV session**, and has not established A12 frame-time or
visual improvements. No fog appearance or gameplay behavior was changed.

# Baseline (A12 device)

Measured 2026-10-21 on the owner's Apple TV 4K (2nd generation, A12, tvOS
27.0, CoreDevice `443953B8-9072-5EAD-A947-95563A1C3D52`), app built from
**`d5961e94`** on branch `tv/bench` (base `tvos-metal` `a8fd7a20` plus the
opt-in frame log; nothing visual changed). The TV display runs at 60 Hz, so
16.7 ms is the cap; the window's frames/s is the real rate. Raw logs and
frame CSVs: `<scratch>/tvbench-out/device-baseline2` (a10) and
`device-gameplay` (b30, cryo bay); simulator: `sim-baseline`.
`<scratch>` = `/var/folders/15/6xlrw1ln4w90d9z8q732vyy00000gn/T/bridge/halo-e6af79dd`
(expires after about a week).

## Headline

- **4K gameplay does not hold 60 fps on the A12: it is GPU-bound.** In the
  two gameplay scenes at 4K the GPU is busy 94-100% of the time and frames
  alternate between 16.7 and 33.3 ms: a10 bridge 49-54 fps, b30 beach 42-48
  fps (p95 33.5 ms; 180-840 frames over 33 ms per 35-60 s). At 1080p the
  same scenes hold 58-60 fps with the GPU 80-89% busy.
- **No frame over 50 ms in steady warm gameplay**, but cold launches stall
  on first-use shader compiles: during load one frame takes 3.5-8.4 s (the
  load screen; 3.4-7.9 s of it compiling), then in play individual frames take
  0.13-1.75 s, nearly all compile time (7.7-13.6 s of compile per run). Warm
  launches still have a ~220 ms first frame after a saved state is read.
- **The owner's "opening fog" is the cryo bay, after the cinematic, not the
  cinematic.** See "The a10 fog question" below.
- CPU per frame (guest + host) is 7-9.5 ms, under the budget at both
  resolutions; it does not change with resolution. The ~9-14 ms "wait
  drawable" is the frame pacing to 60 Hz plus the GPU queue, not CPU work.

## Commands

    cd /Users/halo/code/halo/OpenCE-tv-bench
    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer PATH="$HOME/.local/bin:$PATH"
    python3 tools/tvos_deploy.py build --platform device      # under tv-perf.lock
    python3 tools/tvos_deploy.py sign                          # needs the profile; tells you if missing
    python3 tools/tvos_bench.py make-core --maps <scratch>/halo_data/maps                # a10 bridge state
    python3 tools/tvos_bench.py make-core --scene b30 --maps <scratch>/halo_data/maps    # b30 beach state
    # one device session (ask the TV's owner first; the TV must be awake):
    python3 tools/tvos_bench.py run --target device --install --maps <scratch>/halo_data/maps \
        --plan bridge/1080/2,bridge/2160/2,b30/1080/2,b30/2160/2,a10play/2160/1 --out OUT
    python3 tools/tvos_bench.py run --target simulator --maps <scratch>/halo_data/maps --scenes bridge,b30 --out OUT
    python3 tools/tvos_bench.py report OUT

The device run backs up the TV's `config.toml` and `init.txt`, restores both
in a `finally` and checks them byte-for-byte (all three sessions: verified
identical). It stops the batch on any invalid launch (TV asleep, GPU work
refused in the background, app not started).

## Definitions

- **cold**: the first launch with a new `HALO_METAL_SHADER_SALT` (a comment
  appended to every Metal shader, diagnostic only, so the system's
  compiled-shader cache from earlier launches is missed). This is what a first
  launch after install, or a new shader, costs. **warm**: the next launch with
  the same salt.
- **window**: the scene's measured stretch (frame-log events and fog counts
  find it, not wall time). **steady**: the window less its first 5 s. "run"
  columns are the whole launch, loading included.
- Scenes (no controller input; see `tools/tvos_bench.py`):
  - *a10 cutscene (partial)*: fresh a10 launch, 30 s from the first sustained
    fog-plane drawing. This is the first ~half of the opening **cinematic**,
    not gameplay. Kept for comparison, no longer run by default.
  - *a10 bridge*: saved state on a10's bridge (BSP 1), 35 s, gameplay.
  - *b30 beach*: saved state on The Silent Cartographer's beach right after
    the insertion (marine/covenant firefight running), 60 s, gameplay. A
    `HALO_TEST_INPUT` bot was tried and dropped: it is timed from app launch,
    so load-time differences changed the path (draw counts diverged); with no
    input, draws per 10 s agree within ~5% between runs.
  - *a10 cryo bay*: fresh a10 launch run ~285 s through the whole opening
    cinematic into the first playable section; 50 s from the first 5 s with
    >= 40 fog-plane draws per frame.
- GPU = sum of `GPUEndTime - GPUStartTime` of the frame's command buffers;
  with frames in flight this can exceed the frame interval (GPU-bound). **GPU
  busy %** = union of those intervals over the window: the reliable
  bound-ness figure.
- guest = wall - host renderer calls - main-thread sleeps; host = Metal
  encoding excluding compiles and waits.

## Frame times (all runs)

| target | scene | res | mode | rep | fps | steady p50 | p95 | p99 | max | window max | >25 ms | >33.3 ms | >50 ms | run >50 ms | run compile s | worst compile frame ms | GPU busy % |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| A12 device | a10 cutscene (partial) | 1080 | cold | 1 | 54.0 | 16.7 | 16.9 | 17.1 | 1330.3 | 1330 | 4 | 4 | 4 | 11 | 9.1 | 3549 | 32 |
| A12 device | a10 cutscene (partial) | 1080 | warm | 1 | 59.9 | 16.7 | 17.0 | 17.1 | 27.3 | 51 | 3 | 1 | 1 | 6 | 0.0 | 6 | 38 |
| A12 device | a10 cutscene (partial) | 2160 | cold | 1 | 53.9 | 16.7 | 16.9 | 17.1 | 1324.0 | 1324 | 6 | 5 | 4 | 12 | 8.9 | 3578 | 78 |
| A12 device | a10 cutscene (partial) | 2160 | warm | 1 | 59.9 | 16.7 | 16.9 | 17.1 | 28.9 | 52 | 3 | 1 | 1 | 6 | 0.0 | 6 | 84 |
| A12 device | a10 bridge (gameplay) | 1080 | cold | 1 | 58.2 | 16.7 | 16.9 | 17.1 | 674.3 | 674 | 4 | 3 | 3 | 6 | 7.7 | 6054 | 80 |
| A12 device | a10 bridge (gameplay) | 1080 | warm | 1 | 60.0 | 16.7 | 16.9 | 17.1 | 19.0 | 19 | 0 | 0 | 0 | 5 | 1.2 | 544 | 83 |
| A12 device | a10 bridge (gameplay) | 2160 | cold | 1 | 52.6 | 16.7 | 33.5 | 33.8 | 671.7 | 672 | 197 | 181 | 4 | 9 | 7.7 | 6043 | 94 |
| A12 device | a10 bridge (gameplay) | 2160 | warm | 1 | 54.0 | 16.7 | 33.5 | 33.7 | 33.9 | 49 | 210 | 196 | 0 | 15 | 1.2 | 546 | 97 |
| A12 device | a10 cutscene (partial) | 1080 | cold | 2 | 53.9 | 16.7 | 16.9 | 17.1 | 1322.3 | 1343 | 4 | 4 | 4 | 12 | 8.9 | 3578 | 32 |
| A12 device | a10 cutscene (partial) | 1080 | warm | 2 | 60.0 | 16.7 | 17.0 | 17.1 | 28.7 | 32 | 2 | 0 | 0 | 5 | 0.1 | 6 | 38 |
| A12 device | a10 cutscene (partial) | 2160 | cold | 2 | 53.9 | 16.7 | 17.0 | 17.1 | 1323.0 | 1334 | 6 | 5 | 4 | 12 | 8.9 | 3421 | 77 |
| A12 device | a10 cutscene (partial) | 2160 | warm | 2 | 59.9 | 16.7 | 16.9 | 17.1 | 25.7 | 44 | 2 | 1 | 0 | 5 | 0.0 | 6 | 84 |
| A12 device | a10 bridge (gameplay) | 1080 | cold | 2 | 58.2 | 16.7 | 16.9 | 17.2 | 673.0 | 673 | 4 | 3 | 3 | 6 | 7.7 | 6053 | 80 |
| A12 device | a10 bridge (gameplay) | 1080 | warm | 2 | 60.0 | 16.7 | 16.9 | 17.1 | 19.7 | 20 | 0 | 0 | 0 | 5 | 1.2 | 544 | 83 |
| A12 device | a10 bridge (gameplay) | 2160 | cold | 2 | 51.4 | 16.7 | 33.6 | 33.8 | 671.1 | 671 | 239 | 221 | 4 | 8 | 7.7 | 6052 | 94 |
| A12 device | a10 bridge (gameplay) | 2160 | warm | 2 | 48.8 | 16.7 | 33.6 | 33.8 | 34.0 | 34 | 392 | 350 | 0 | 17 | 1.2 | 544 | 99 |
| A12 device | b30 beach (gameplay) | 1080 | cold | 1 | 59.1 | 16.7 | 16.8 | 16.9 | 354.0 | 906 | 5 | 5 | 5 | 7 | 9.4 | 7850 | 89 |
| A12 device | b30 beach (gameplay) | 1080 | warm | 1 | 59.9 | 16.7 | 16.9 | 17.0 | 17.3 | 224 | 3 | 2 | 1 | 3 | 0.3 | 276 | 89 |
| A12 device | b30 beach (gameplay) | 2160 | cold | 1 | 47.7 | 16.7 | 33.5 | 33.6 | 372.0 | 633 | 673 | 584 | 9 | 10 | 9.3 | 7798 | 99 |
| A12 device | b30 beach (gameplay) | 2160 | warm | 1 | 45.3 | 16.7 | 33.5 | 33.6 | 33.8 | 223 | 875 | 836 | 2 | 3 | 0.0 | 12 | 100 |
| A12 device | a10 cryo bay (first gameplay) | 2160 | cold | 1 | 55.9 | 16.7 | 17.0 | 17.3 | 33.8 | 67 | 137 | 113 | 45 | 176 | 13.6 | 3451 | 96 |
| A12 device | a10 cryo bay (first gameplay) | 2160 | warm | 1 | 55.8 | 16.7 | 21.8 | 23.8 | 35.1 | 59 | 126 | 108 | 28 | 55 | 0.1 | 6 | 100 |
| A12 device | b30 beach (gameplay) | 1080 | cold | 2 | 59.1 | 16.7 | 16.9 | 17.0 | 367.8 | 947 | 5 | 5 | 5 | 6 | 9.3 | 7727 | 88 |
| A12 device | b30 beach (gameplay) | 1080 | warm | 2 | 59.9 | 16.7 | 16.9 | 17.0 | 17.1 | 219 | 2 | 1 | 1 | 2 | 0.0 | 12 | 89 |
| A12 device | b30 beach (gameplay) | 2160 | cold | 2 | 44.3 | 16.7 | 33.5 | 33.6 | 375.8 | 633 | 890 | 756 | 6 | 7 | 9.1 | 7811 | 99 |
| A12 device | b30 beach (gameplay) | 2160 | warm | 2 | 41.7 | 16.8 | 33.6 | 33.6 | 33.8 | 222 | 1095 | 840 | 2 | 4 | 0.3 | 263 | 100 |
| tvOS simulator | a10 cutscene (partial) | 1080 | cold | 1 | 105.7 | 4.3 | 21.2 | 22.2 | 493.7 | 494 | 4 | 4 | 4 | 11 | 3.3 | 1284 | 2 |
| tvOS simulator | a10 cutscene (partial) | 1080 | warm | 1 | 114.0 | 4.3 | 21.2 | 22.1 | 31.7 | 39 | 2 | 1 | 0 | 3 | 0.1 | 8 | 2 |
| tvOS simulator | a10 cutscene (partial) | 2160 | cold | 1 | 128.3 | 4.4 | 21.0 | 22.0 | 517.6 | 532 | 4 | 4 | 4 | 10 | 3.2 | 1157 | 2 |
| tvOS simulator | a10 cutscene (partial) | 2160 | warm | 1 | 136.6 | 4.5 | 21.0 | 22.1 | 30.2 | 36 | 2 | 1 | 0 | 3 | 0.1 | 9 | 3 |
| tvOS simulator | a10 bridge (gameplay) | 1080 | cold | 1 | 157.1 | 4.4 | 20.9 | 21.6 | 244.7 | 245 | 3 | 3 | 2 | 9 | 3.2 | 2052 | 5 |
| tvOS simulator | a10 bridge (gameplay) | 1080 | warm | 1 | 159.8 | 4.3 | 20.9 | 22.0 | 23.1 | 23 | 0 | 0 | 0 | 2 | 0.1 | 11 | 5 |
| tvOS simulator | a10 bridge (gameplay) | 2160 | cold | 1 | 133.9 | 5.0 | 20.8 | 21.4 | 247.2 | 247 | 3 | 3 | 2 | 9 | 3.1 | 2011 | 4 |
| tvOS simulator | a10 bridge (gameplay) | 2160 | warm | 1 | 139.0 | 5.0 | 20.7 | 21.4 | 22.6 | 23 | 0 | 0 | 0 | 1 | 0.1 | 12 | 4 |
| tvOS simulator | a10 cutscene (partial) | 1080 | cold | 2 | 108.4 | 4.3 | 21.2 | 22.2 | 482.1 | 484 | 4 | 4 | 4 | 10 | 3.2 | 1193 | 2 |
| tvOS simulator | a10 cutscene (partial) | 1080 | warm | 2 | 114.4 | 4.3 | 21.4 | 22.3 | 26.3 | 26 | 1 | 0 | 0 | 3 | 0.1 | 9 | 3 |
| tvOS simulator | a10 cutscene (partial) | 2160 | cold | 2 | 125.6 | 4.5 | 21.0 | 21.7 | 546.9 | 549 | 4 | 4 | 4 | 10 | 3.3 | 1169 | 2 |
| tvOS simulator | a10 cutscene (partial) | 2160 | warm | 2 | 133.9 | 4.4 | 21.2 | 22.2 | 30.4 | 36 | 2 | 1 | 0 | 3 | 0.1 | 10 | 3 |
| tvOS simulator | a10 bridge (gameplay) | 1080 | cold | 2 | 149.8 | 4.4 | 20.9 | 21.8 | 246.4 | 246 | 3 | 3 | 2 | 9 | 3.1 | 2060 | 4 |
| tvOS simulator | a10 bridge (gameplay) | 1080 | warm | 2 | 158.6 | 4.4 | 20.8 | 21.8 | 23.3 | 24 | 0 | 0 | 0 | 2 | 0.1 | 10 | 5 |
| tvOS simulator | a10 bridge (gameplay) | 2160 | cold | 2 | 132.2 | 5.0 | 20.8 | 21.4 | 249.5 | 249 | 3 | 3 | 2 | 9 | 3.2 | 2039 | 4 |
| tvOS simulator | a10 bridge (gameplay) | 2160 | warm | 2 | 135.3 | 5.0 | 20.8 | 21.4 | 22.6 | 23 | 0 | 0 | 0 | 2 | 0.1 | 11 | 4 |

(Simulator rows run uncapped at 100-160 fps on the Mac's GPU; its p95 ~21 ms
is the simulator's own presentation cadence. **Simulator numbers are not
Apple TV numbers.** The Mac build was used only to validate tooling.)

## Where the frame goes (window means, ms, repeat 1)

| target | scene | res | mode | rep | guest | host | compile | wait drawable | wait in-flight | GPU (cmd-buffer sum) | draws | fog draws | transparent draws | passes | feedback copies |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| A12 device | a10 cutscene (partial) | 1080 | cold | 1 | 5.16 | 2.35 | 1.26 | 9.12 | 0.00 | 5.8 | 373 | 32 | 72 | 2.7 | 0.0 |
| A12 device | a10 cutscene (partial) | 1080 | warm | 1 | 5.21 | 2.41 | 0.01 | 9.05 | 0.00 | 6.3 | 381 | 32 | 72 | 3.8 | 0.0 |
| A12 device | a10 cutscene (partial) | 2160 | cold | 1 | 5.14 | 2.36 | 1.26 | 9.14 | 0.00 | 14.0 | 373 | 32 | 72 | 2.7 | 0.0 |
| A12 device | a10 cutscene (partial) | 2160 | warm | 1 | 5.15 | 2.33 | 0.01 | 9.19 | 0.00 | 14.0 | 381 | 32 | 72 | 3.8 | 0.0 |
| A12 device | a10 bridge (gameplay) | 1080 | cold | 1 | 4.69 | 1.98 | 0.53 | 9.78 | 0.00 | 13.8 | 281 | 16 | 21 | 9.8 | 0.0 |
| A12 device | a10 bridge (gameplay) | 1080 | warm | 1 | 4.72 | 2.03 | 0.00 | 9.71 | 0.00 | 13.8 | 279 | 16 | 21 | 9.9 | 0.0 |
| A12 device | a10 bridge (gameplay) | 2160 | cold | 1 | 5.01 | 2.11 | 0.59 | 11.05 | 0.00 | 22.7 | 274 | 16 | 21 | 9.8 | 0.0 |
| A12 device | a10 bridge (gameplay) | 2160 | warm | 1 | 5.02 | 2.12 | 0.00 | 11.10 | 0.00 | 23.1 | 272 | 16 | 21 | 9.8 | 0.0 |
| A12 device | b30 beach (gameplay) | 1080 | cold | 1 | 5.59 | 1.82 | 0.11 | 9.25 | 0.00 | 14.9 | 735 | 15 | 36 | 18.0 | 0.0 |
| A12 device | b30 beach (gameplay) | 1080 | warm | 1 | 5.54 | 1.81 | 0.00 | 9.31 | 0.00 | 14.9 | 735 | 15 | 36 | 18.0 | 0.0 |
| A12 device | b30 beach (gameplay) | 2160 | cold | 1 | 6.55 | 2.03 | 0.23 | 11.96 | 0.00 | 30.5 | 737 | 15 | 38 | 18.0 | 0.0 |
| A12 device | b30 beach (gameplay) | 2160 | warm | 1 | 7.08 | 2.19 | 0.00 | 12.85 | 0.00 | 33.6 | 731 | 15 | 34 | 18.0 | 0.0 |
| A12 device | a10 cryo bay (first gameplay) | 2160 | cold | 1 | 5.07 | 2.52 | 0.00 | 9.22 | 0.00 | 16.3 | 543 | 43 | 65 | 13.0 | 0.0 |
| A12 device | a10 cryo bay (first gameplay) | 2160 | warm | 1 | 5.03 | 2.67 | 0.00 | 0.05 | 9.06 | 43.6 | 541 | 43 | 63 | 13.0 | 0.0 |
| tvOS simulator | a10 cutscene (partial) | 1080 | cold | 1 | 1.76 | 0.97 | 0.25 | 6.53 | 0.00 | 0.2 | 381 | 32 | 72 | 4.0 | 0.0 |
| tvOS simulator | a10 cutscene (partial) | 1080 | warm | 1 | 1.74 | 0.97 | 0.01 | 6.08 | 0.00 | 0.2 | 382 | 31 | 71 | 4.4 | 0.0 |
| tvOS simulator | a10 cutscene (partial) | 2160 | cold | 1 | 1.75 | 0.93 | 0.23 | 5.08 | 0.00 | 0.2 | 378 | 32 | 72 | 3.5 | 0.0 |
| tvOS simulator | a10 cutscene (partial) | 2160 | warm | 1 | 1.76 | 0.94 | 0.01 | 4.93 | 0.00 | 0.2 | 379 | 32 | 72 | 3.7 | 0.0 |
| tvOS simulator | a10 bridge (gameplay) | 1080 | cold | 1 | 1.49 | 0.77 | 0.08 | 4.02 | 0.00 | 0.3 | 275 | 16 | 21 | 9.9 | 0.0 |
| tvOS simulator | a10 bridge (gameplay) | 1080 | warm | 1 | 1.50 | 0.78 | 0.00 | 4.01 | 0.00 | 0.3 | 270 | 16 | 20 | 9.9 | 0.0 |
| tvOS simulator | a10 bridge (gameplay) | 2160 | cold | 1 | 1.69 | 0.81 | 0.09 | 4.84 | 0.00 | 0.3 | 285 | 17 | 20 | 9.9 | 0.0 |
| tvOS simulator | a10 bridge (gameplay) | 2160 | warm | 1 | 1.67 | 0.80 | 0.00 | 4.70 | 0.00 | 0.3 | 281 | 16 | 20 | 10.0 | 0.0 |


## The a10 fog question

The a10 opening (fresh launch, ~285 s, 4K, A12) in order:

| time from launch | what | fog-plane draws/frame | A12 4K |
|---|---|---|---|
| 0-3 s | load, flash of the space scene | 26 | first frames 0.5 s (load) |
| 3-12 s | black/titles | 0 | 60 fps |
| 12-45 s | cinematic: Pillar of Autumn in space (fog planes + up to 84 transparents) | 32 | 60 fps warm, GPU 10-15 ms, busy 84%; cold: 4 stalls of 0.5-1.3 s |
| 45-110 s | cinematic: bridge with Keyes | 12-21 | 60 fps |
| ~111 s | brief 10-pass screen convolution (cinematic blur), ~1 s | | |
| 110-220 s | cinematic continues: hangar, cryo | 13-20 | |
| **~223 s on** | **cryo bay: first gameplay, "tutorial" with the 2nd blur (6 passes, ~10 s) and then the fogged room** | **43** | **GPU busy 96-100%; ~56 fps; first 5 s: 45 frames over 50 ms (cold) / 28 (warm), worst 67 ms; warm GPU sum 44 ms/frame** |

So the fog that slows 4K is the **cryo bay's** (43 fog-plane draws per frame,
the most anywhere measured, plus 63-84 transparents and 13 passes), in the
first playable section, not in the cinematic. The cinematic's fog runs at 60
fps warm. The cryo bay's worst stretch is the first 5 s, when the tutorial's
screen convolution (6 passes at 4K) is drawn over the fog: 41-45 ms frames,
the GPU queue full ("wait in-flight" 29 ms warm). After it the room runs at
16.7 ms with the GPU ~95-100% busy and no margin. (The phase timeline and
convolution timing are from a Mac profile run, `<scratch>/tvbench-cryo-prof.log`;
the A12 frame times are the device runs.) The two cryo-bay runs differ in how
the GPU time is attributed (16 vs 44 ms GPU sum) because with three frames in
flight, queued work overlaps: use GPU busy %, which is ~100% for both.

## Traces (xctrace): not obtained

Metal System Trace and Time Profiler were attempted in both device sessions
and failed. The TV is network-paired: devicectl reaches it, but `xctrace list devices`
shows it under "Devices Offline". `--attach OpenCETV` → "Cannot find process
matching name"; `--attach <pid>` (devicectl's pid, while the app ran) →
"Cannot find process for provided pid"; `--all-processes` → "Timed out
waiting for device to boot". So there is no per-encoder GPU breakdown and no CPU
symbol profile from the A12 yet. Getting one probably needs Instruments.app
with the TV paired for profiling (or a USB-capable path); this needs the owner.
The frame log's counters (passes, fog/transparent draws, GPU busy) are the
substitute.

## Run-to-run noise and overhead

- Repeats agree closely: wall p95 within 0.1 ms, cold compile totals within
  2%, 4K over-33 counts vary more (181 vs 221, 584 vs 756) because the GPU is
  at its limit and thermals/timing decide which frames miss.
- The 4K warm runs are *not* faster than cold in steady state (b30: 42-45 vs
  44-48 fps): cold/warm only changes the compile stalls.
- Frame-log overhead (Mac, 40 s warm a10, 4740 frames, vsync): user CPU
  7.20-7.66 s across off / summary / full, within run noise. Not measured on
  the TV separately; all device runs had it on.

## Ranked recommendation

1. **GPU fill/bandwidth at 4K (tile bandwidth, pass breaks, overdraw)**: the
   steady-state blocker. Every 4K gameplay scene is GPU-bound at 94-100% busy;
   b30 does 18 passes/frame, the cryo bay 13 passes plus 43 fog planes and 60+
   transparents, all 4K-wide and alpha-blended. Feedback copies are 0 in every
   scene measured, so the copy fix is not the lever; pass count/load-store,
   fog and transparent overdraw and full-screen effects (the tutorial blur)
   are. Next step: get a GPU trace (above) or add per-phase GPU timestamps.
2. **Shader compile stalls**: on cold launches in-play frames stall
   0.13-1.75 s (compile is 85-99% of each), plus a 3.5-8.4 s load frame;
   7.7-13.6 s of compile per run. Compiles cause nearly every frame over
   100 ms (the others are load frames).
   Precompiling at load or a binary archive would remove them. Large for a
   first play, invisible warm.
3. **DXT->BGRA8 texture bandwidth**: plausible contributor to (1) (4x the
   bytes of BC1) but unmeasured; worth a diagnostic A/B once GPU timing per
   pass exists.
4. **CPU guest / bounds checks**: 5-7 ms guest + 2 ms host per frame; fine at
   60 fps now, not the 4K limiter. The 200 ms first frame after a state load
   is guest work (load), a one-off.
5. **Lifecycle**: the TV asleep refuses launches; background GPU work is
   refused. Tooling detects both. Since tv/lifecycle the game stops while
   inactive and submits nothing ("Lifecycle (A12)" below).

# Shader compile stalls (A12)

Branch `tv/shaders`. Measured 2026-10-21 on the owner's Apple TV 4K (2nd gen,
A12, tvOS 27.0, CoreDevice `443953B8-…`), app built from **`aa7fd613`**.
"Before" is the same build with the changes switched off by environment
(`HALO_METAL_LANGUAGE_VERSION=default HALO_METAL_SHADER_CACHE=0`), so both
use the corrected salt (below). 4K, 2 repeats, 20 runs, all valid. The TV's
`config.toml` and `init.txt` were backed up and restored, and verified
identical, after each batch. Raw logs: `<scratch>/shaders/device-before`,
`device-after`. Mac/simulator data: `<scratch>/shaders/mac-*`, `sim-cache1`.

## What was changed (3 commits + bench tooling)

1. **MSL 2.2 instead of the SDK's default (4.0)** (`compile_options`,
   `host_metal.m`). Most of a runtime compile was spent in the newest
   `metal_stdlib`'s texture templates, not in the shader. A trivial pixel
   shader with one `texture2d` argument costs 95 ms at 4.0, 4 ms at 2.0 and
   27 ms at 2.2 on an M4. The translated shaders need only 2.1 (invariant
   positions).
   - Mac, a10 and b30's 228 dumped pixel shaders: 47 ms → 8.8 ms each.
     Vertex shaders: 8 ms either way.
   - Rendering is **bit-identical**: all 228 pixel shaders × 4 random trials
     into RGBA32F, and all 56 vertex shaders' outputs captured in a buffer,
     4.0 vs 2.2 (also vs 2.4 and 3.0).
   - Negative control: safe math in place of fast math changes 191 of 228
     pixel shaders and 56 of 56 vertex shaders, so the comparison does detect
     a change.
   - `HALO_METAL_LANGUAGE_VERSION` selects another version for A/B.
2. **The salt was only half cold.** `HALO_METAL_SHADER_SALT` was a comment,
   which leaves the AIR unchanged, so the system's *pipeline* cache still
   hit.
   - It now also renames the entry point (`fragment_main_salt_<salt>`).
   - Mac b30 cold pipeline CPU: 0.30 → 0.54 s.
   - **The "Baseline (A12 device)" cold numbers above undercount pipeline
     compile by about that much.**
3. **A shader cache with background preparation.**
   - What it stores (`host_shader_manifest.c`, `Library/Caches/<bundle id>/metal_shaders.manifest`):
     - the source of every shader the game asked for, and what each draw
       pipeline was made of;
     - never compiled code.
   - At the next launch, two threads compile those shaders, then those
     pipelines, from the moment the device exists.
   - A draw whose shader text is exactly a prepared one takes that function,
     waiting if a thread is mid-compile. Any other draw compiles on the main
     thread as before.
   - The prepared pipelines fill the system's pipeline cache, so the draws'
     own `newRenderPipelineState` calls hit it.
   - The file key:
     - GPU, MSL version and record layout must match, else the file is not
       read;
     - executable UUID and OS version must match, else the file is read and
       prepared, then started again. So an app or OS update is covered.
   - Records are checksummed; a bad or partial record ends the read and is
     cut off. The file is flock'd and capped at 32 MiB.
   - `HALO_METAL_SHADER_CACHE=0` turns it off;
     `HALO_METAL_SHADER_CACHE_RESET=1` empties it.
   - `tools/tvos_bench.py` uses its own file (`tvbench_shaders.manifest`) and
     empties it on every cold run. New mode **primed**: a new salt, so the
     system caches miss, with the record the cold run left. This is a launch
     after an OS update, by someone who has played before.

## A12 results (4K, per run; "in play" = after the load frame)

| build | scene | mode | rep | load frame s | run >50 ms | run >100 ms | in-play >50 | in-play >100 | worst in-play ms | in-play compile frames (>5 ms) | worst compile frame ms | run compile s |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| before | a10 bridge | cold | 1 | 7.06 | 9 | 7 | 7 | 5 | 696 | 5 | 689 | 8.28 |
| before | a10 bridge | cold | 2 | 7.16 | 9 | 6 | 8 | 5 | 692 | 5 | 685 | 8.27 |
| before | a10 bridge | warm | 1 | 0.57 | 16 | 5 | 15 | 4 | 686 | 4 | 618 | 1.33 |
| before | a10 bridge | warm | 2 | 0.57 | 18 | 5 | 17 | 4 | 655 | 4 | 584 | 1.26 |
| before | b30 beach | cold | 1 | 9.07 | 6 | 6 | 4 | 4 | 376 | 4 | 367 | 9.79 |
| before | b30 beach | cold | 2 | 9.06 | 7 | 7 | 5 | 5 | 608 | 5 | 596 | 10.25 |
| before | b30 beach | warm | 1 | 0.63 | 4 | 2 | 2 | 0 | 67 | 0 | 1 | 0.03 |
| before | b30 beach | warm | 2 | 0.68 | 3 | 2 | 1 | 0 | 72 | 0 | 1 | 0.03 |
| after | a10 bridge | cold | 1 | 2.56 | 25 | 6 | 23 | 4 | 255 | 9 | 194 | 2.95 |
| after | a10 bridge | cold | 2 | 2.58 | 26 | 5 | 25 | 4 | 267 | 9 | 193 | 2.96 |
| after | a10 bridge | warm | 1 | 0.56 | 17 | 1 | 16 | 0 | 85 | 0 | 3 | 0.02 |
| after | a10 bridge | warm | 2 | 0.56 | 15 | 1 | 14 | 0 | 79 | 0 | 3 | 0.03 |
| after | a10 bridge | primed | 1 | 1.75 | 14 | 1 | 13 | 0 | 72 | 0 | 3 | 1.15 |
| after | a10 bridge | primed | 2 | 1.75 | 16 | 1 | 15 | 0 | 81 | 0 | 3 | 1.15 |
| after | b30 beach | cold | 1 | 3.55 | 7 | 4 | 5 | 2 | 104 | 6 | 91 | 3.40 |
| after | b30 beach | cold | 2 | 3.18 | 4 | 3 | 2 | 1 | 103 | 4 | 91 | 3.00 |
| after | b30 beach | warm | 1 | 0.58 | 3 | 2 | 1 | 0 | 70 | 0 | 1 | 0.02 |
| after | b30 beach | warm | 2 | 0.59 | 3 | 2 | 1 | 0 | 55 | 0 | 1 | 0.02 |
| after | b30 beach | primed | 1 | 1.99 | 3 | 2 | 1 | 0 | 58 | 0 | 1 | 1.31 |
| after | b30 beach | primed | 2 | 1.94 | 4 | 2 | 2 | 0 | 71 | 1 | 47 | 1.30 |

("Run" columns include the load frames and the frame after a saved state
is read, which is guest work, about 220 ms.)

**Cold (first launch ever, or a new GPU or compiler).**
- Compile per run: 8.3 → 3.0 s (bridge), 9.8–10.2 → 3.0–3.4 s (b30).
- Load frame: 7.1 → 2.6 s, and 9.1 → 3.2–3.6 s.
- Worst in-play frame: 692–696 → 255–267 ms, and 376–608 → 103–104 ms.
- In-play stalls are **reduced, not eliminated**: there are still 1–4 frames
  over 100 ms, all compile.

**Warm (same build and OS).**
- Before, a10 bridge still compiled 1.3 s in 4 frames late in the run, up to
  686 ms. The same happened in the tv/bench baseline (1.2 s): the system cache
  does not always keep what was compiled shortly before the previous run
  exited. With the cache they are prepared at launch: 0 compile frames.

**Primed (OS or app update, the game played before).**
- 0 in-play frames over 100 ms.
- 0 compile frames over 5 ms, except one of 47 ms.
- Worst in-play frame 58–81 ms. All the remaining compile, 1.15–1.31 s, is in
  the load frame. On the TV the threads prepared the 76–82 shaders in about
  1.6 s and the 56–62 pipelines in about 0.45 s.

**Load time.**
- Cold: −4.5 to −5.9 s.
- Warm: unchanged.
- Primed vs warm: +1.2–1.4 s, because the main thread waits for prepared
  compiles. That is the cost of having them out of play.

The remaining in-play frames over 50 ms in warm and primed runs have **no
compile**. They wait about 35–55 ms for a drawable with the GPU sum at 90–110
ms (bridge about 50 s in; b30's frame after the state read). That is GPU-bound
work for the bandwidth item. After-cold bridge shows more frames over 50 ms
(25 vs 9) because that same GPU-bound stretch is no longer hidden behind the
compile stalls; warm runs before and after have the same count (15–18).

**The cryo bay (from the baseline logs, not rerun).** None of its 45 cold / 28
warm frames over 50 ms in the first 5 s compile (compile 0.0–0.8 ms) or copy
(0 feedback copies). Host time (encoding plus uploads) is 4.7–6.3 ms. In the
cold run they wait for a drawable (37 ms mean); in the warm run, for a frame
in flight (36 ms mean). The command-buffer GPU sum is 77–122 ms. **They are
GPU queueing, not compile, pipeline creation or texture upload.**

## Variant structure (a10 bridge + b30 beach, Mac dump)

- **Library variant:** one `newLibraryWithSource` per guest shader key.
  - A pixel shader per `nv2a_pixel_shader_key`: combiner state, texture
    modes, sampler types, alpha kill/test, fog, per-pixel lighting, …
  - A vertex shader per (program, immediate/stream, lit/unlit), with the
    packed-attribute mask in its text.
  - Bridge: 27 VS + 111 PS handles (49 distinct PS sources). b30: 29 VS + 117
    PS handles (51 distinct). 112 distinct sources in total, 1.5 MB.
- **Pipeline variant:** `pipeline_key` = (VS handle, PS handle, vertex-layout
  handle, colour format, depth format, blend enable/src/dst/op/write mask).
  56 (bridge) and 61 (b30) pipelines.
  - Vertex layout = attribute formats, offsets and buffers, plus strides and
    the constant step.
- Duplicate PS sources (same text, another handle) compile again. The
  system's cache makes that cheap within a run.

## Options evaluated

- **Async compile with a fallback.** Not built. A draw whose shader has
  never been seen needs it in that frame. Without a fallback, the wait is
  only moved; with one (skip the draw, or draw a generic shader) the picture
  changes, which needs the owner's approval. The record plus background
  preparation gives the asynchronous benefit with no visual change, for every
  launch except the first.
- **Library batching** (Mac): N shaders in one library, each in its own
  namespace.
  - At MSL 4.0: 47 ms/shader alone, 10.9 batched by 8, 6.4 by 32.
  - At 2.2: 8.8, 4.2 and 3.5.
  - 4 parallel compiles: 2.9 ms/shader wall.
  - Batching needs the variants known in advance, i.e. the record. It could
    shorten the primed load frame further (follow-up).
  - Function constants would mean rewriting the translator
    (`nv2a_psh.c`/`nv2a_vsh.c`); not attempted.
- **Precompiled `.metallib`.** The Metal Toolchain is not installed on this
  Mac (`xcrun metal` says to run `xcodebuild -downloadComponent
  MetalToolchain`), and I did not install it. The variant set is
  data-dependent: 77 distinct PS sources in two scenes, and more key
  combinations over the campaign and Custom Edition maps. So `default.metallib`
  cannot enumerate it. Not pursued.
- **MTLBinaryArchive.** Not measured. For the same build and OS the system
  cache already makes warm compile about 0, and an OS update can invalidate
  an archive (the SDK header says so). That is the case primed covers. It
  also skips only the back end; the source compile is the larger part (Mac
  b30 cold: shader 0.77 s, pipeline 0.54 s). Not ruled out; untested.
- **Other compile options.** `preserveInvariance` off: no change (8.8 ms).
  `optimizationLevel = Size`: slower (9.6 ms). Narrower `#include`s in place
  of `metal_stdlib`: no change at either version. The cost is the language
  version's library, not the includes.

## Correctness

- Variant keys are unchanged. A prepared function is used only for the
  identical compiled text: the same `shader_text()` applied to the same
  source, the same kind and the same options. Otherwise the main thread
  compiles from the game's source as before.
- Prepared pipelines are dropped. Draws still make their own, keyed as
  before.
- Threads:
  - only the main thread writes the record and its tables;
  - prepared shaders are shared under one `NSCondition`;
  - shader diagnostics' environment is read once on the main thread.
- The file (C harness `tools/test_metal_shader_manifest.py`):
  - absent or another device's: nothing prepared;
  - another build's: prepared, then restarted;
  - damaged or partial records: cut off;
  - locked by another copy: not kept;
  - unwritable folder: the renderer goes on without a record.
- ARC throughout, as before. Device and simulator builds compile clean.
- Mac and simulator runs: 0 prepare failures. On the TV: 0 shaders and 0
  pipelines failed to prepare.

# GPU bandwidth (A12)

Branch `tv/bandwidth` (on `tv/bench` `be9c4a19`). **Everything in this
section so far is measured on the Mac (M4) or the tvOS simulator, not on the
A12.** The device session (below, "Device: pending") will replace the
expectations with TV numbers. Scratch results:
`<scratch>/bw/` (Mac/simulator), `<scratch>/tvbench-out/device-bandwidth`
(device, when run).

## Tools added

- `HALO_PASS_LOG=1` (with `HALO_FRAME_LOG=1`): one record per render pass
  (targets, size, load/store actions, why it ended, draws, the rasterizer
  phases in it) and its GPU time from `MTLCounterSampleBuffer` timestamps at
  the pass's vertex and fragment stage boundaries; written as
  `<frame log>-passes.csv`. `tools/metal_pass_log.py PASSES.csv --frames
  FRAMES.csv --scene b30` sums it per phase and per pass index for the
  steady window. Stage sampling: M4 yes; **tvOS simulator no** (draw and blit
  boundaries only, so no per-pass time there); A12 unknown until the device
  session (the fallback, `HALO_METAL_PASS_TIMING=buffers`, gives a command
  buffer per pass).
- Diagnostics, inert unless set: `HALO_METAL_PASS_SPLIT=phase` (end a pass
  at every phase change, to time phases apart: adds loads/stores, so it
  overstates), `HALO_METAL_SKIP_PHASES=17,22,...` (drop a phase's draws: an
  upper bound on what it costs), `HALO_METAL_HOIST=0|toggle:N`,
  `HALO_METAL_DEPTH_DISCARD=toggle:N`.
- Scene `cryo` (`tvos_bench.py make-core --scene cryo`): the cryo bay from a
  saved state, ~70 s a run instead of ~285 s. The tutorial's 6-pass screen
  convolution runs ~5-13 s into it, then the fogged room (43 fog planes).
- `tvos_bench.py run --signed APP` (install a private signed copy) and
  `--no-push`; the pass log is copied off the TV with the frame log.

## What the passes were (Mac, 3840x2160, steady window, before)

The Mac draws the same frames as the TV at 3840x2160 when its window is
16:9 (`HALO_WINDOW_SIZE=1920x1080`; the default 4:3 window gives 2880x2160).
Pass counts agree with the A12 baseline (bridge 9.8, b30 18, cryo 13).

| scene | passes/frame | screen target (c1+d2, 3840x2160 BGRA8 + D32S8) passes | attachment loads + stores a frame |
|---|---|---|---|
| a10 bridge | 9.8 | 4 (1 clear + 3 reloads) | 208 + 283 MB |
| b30 beach | 18 | 7 (1 clear + 6 reloads) | 448 + 523 MB |
| a10 cryo bay | 13.5 | 5 (1 clear + 4 reloads); 20 during the convolution | 317 + 392 MB |

Why the screen pass was broken: every small offscreen pass Halo draws
between its phases ended it, and the next screen draw loaded colour, depth
and stencil again (~75 MB in, ~75 MB out at 4K): the shadow maps (128x128,
2-3 lights, each a pair of passes), the water's bump map and its 4 mips
(b30), a 64x64 HUD meter, a mip-composite blit. No same-target consecutive
passes, no empty passes, no feedback copies (all ~0). The depth/stencil
store was always Store. Full per-pass listings:
`<scratch>/bw/mac-pass-169/*`, `<scratch>/bw/mac-pass-cryo/*`.

## Per-phase GPU time (Mac M4, 3840x2160, pass per phase: diagnostic)

Fragment-stage ms a frame with `HALO_METAL_PASS_SPLIT=phase` (hoisting off).
The split adds a 4K load+store per phase (22-31 passes a frame), so
absolute numbers are inflated and the first pass after a wait carries it;
use them for the ranking, not as A12 milliseconds. The A12 table comes from
the device session.

| scene | top phases (ms/frame) | sum | frame GPU span |
|---|---|---|---|
| bridge | shadows 1.29, diffuse lights 0.96, reflections 0.77, queued transparents 0.67, textures 0.61, lightmaps 0.57, fog planes 0.56, reflection masks 0.55, specular 0.47, sky 0.41 | 7.4 | 6.6 |
| b30 | queued transparents 2.15, shadows 1.90, lightmaps 0.62, textures 0.60, fog planes 0.56, water 0.48, sky 0.44, models 0.43, HUD 0.40 | 7.9 | 6.4 |
| cryo | shadows 2.06, reflections 1.22, textures 0.90, lightmaps 0.88, fog planes 0.81, reflection masks 0.77, models 0.43, transparents 0.30 | 7.6 | 6.6 |
| cryo, convolution frames | the 6 screen-effect passes 3.26 of 9.2 | 9.2 | |

Ceilings by dropping phases (`HALO_METAL_SKIP_PHASES`, diagnostic, Mac at
7680x4320 uncapped, where the M4 is GPU-bound; fps, hoisting on):

| dropped | cryo | b30 |
|---|---|---|
| nothing | 96.7 | 66.1 |
| fog planes (17) | 100.7 (+4%) | 70.4 (+7%) |
| queued transparents (22) | 99.2 (+3%) | 118.8 (CPU-capped: +80% or more) |
| screen effect (26) | 92.6 (noise) | 66.0 |
| 17 + 22 | 109.0 (+13%) | 119.6 |
| 17 + 22 + 26 | 115.3 (+19%) | 119.6 |

So in b30 the transparents (marines' and covenant effects, particles) are
the largest single cost; in the cryo bay the fog planes and transparents
together are ~13% and the rest is the ordinary opaque scene. Removing fog
or transparents is not an option (quality); these bound what any fog or
transparent optimisation can win.

## Change 1: hoisting small passes out of the screen's pass (default on)

`d9ebe84f`. The screen's pass is parked (encoder left open) while the small
passes and blits are encoded into their own command buffer, committed
ahead of the frame's; the screen pass then continues. Dependency rules
(per-handle "read/written by command buffer N"; a shadow map the screen
pass already sampled is renamed to a pooled texture; readbacks, present,
feedback copies, redefinitions unpark first) are in host_metal.m's comment.
`HALO_METAL_HOIST=0` restores the old behaviour.

| scene (Mac, 3840x2160) | passes/frame | loads + stores a frame |
|---|---|---|
| bridge | 9.8 -> 7.2 | 208 + 283 -> 13 + 88 MB |
| b30 | 18 -> 12 | 448 + 523 -> 0.3 + 75 MB |
| cryo | 13.5 -> 9.5 | 317 + 392 -> 20 + 95 MB |

Throughput, Mac at 7680x4320 (GPU-bound there; at 3840x2160 the Mac runs
~119 fps either way, capped by CPU), steady window, two runs each:

| scene | before fps | after fps | wall p95 before -> after |
|---|---|---|---|
| bridge | 67.7 / 63.6 | 68.2 / 68.0 | 19.3 / 20.6 -> 19.4 / 19.5 ms |
| b30 | 57.4 / 55.7 | 67.1 / 67.2 (+18%) | 19.2 / 19.9 -> 16.7 / 16.7 ms |
| cryo | 79.1 / 78.8 | 91.4 / 91.7 (+16%) | 12.5 / 12.5 -> 11.4 / 11.3 ms |

The M4 has far more bandwidth per pixel than the A12, so the TV's gain may
be larger; it is not measured yet.

Visual check (numeric; images not viewed): Mac, the game's time frozen
(`game_speed 0` over telnet) on the bridge, b30 and cryo states, back buffer
read every 30 frames with `HALO_METAL_HOIST=toggle:30` (hoisting on and off
in alternate 30-frame blocks of the same run): **every compared pair of
3840x2160 frames, hoisted vs not, is byte-identical (max diff 0, mean 0)**
in all three scenes. (Two separate launches of the same build differ by up
to max 177 / mean 1.1 on the bridge from animation not stopped by
game_speed, so cross-launch comparisons are not a usable check; the
in-run toggle is.)

## Change 2: depth/stencil not stored at the present (opt-in)

`38b96a59`, `HALO_METAL_DEPTH_DISCARD=present`. The last pass of the frame
stores depth and stencil as DontCare (store action chosen at pass end).
Nothing reads the screen's depth after the frame: the guest never samples a
depth target, readbacks/screenshots are colour only, and every 3D window
clears depth+stencil first (`rasterizer_window_begin`). That last part is
the game's behaviour, not a guarantee, so it is off by default and watched:
a load of a discarded depth target is logged and turns it off. Mac: 0 such
loads in bridge, b30, cryo; stores a frame 88 -> 46 / 75 -> 34 / 95 -> 54 MB;
frozen-frame toggle check byte-identical in all three. Menus, cinematics
and split screen not checked. Memoryless depth is not possible as long as
any pass after the first reloads the screen depth (cryo's convolution and
the unhoistable cases still do).

## Not done, and why

- **Memoryless attachments**: the screen's depth is reloaded by passes that
  cannot be hoisted (the convolution passes ping-pong through the screen
  colour target; the screen-effect target is 4K too); small targets are
  sampled later. Nothing qualifies safely yet.
- **Framebuffer fetch**: the only same-pixel reads (feedback copies) are 0
  in every scene measured; the convolution samples at offsets, so it does
  not qualify.
- **Fog/transparent cost**: measured (above) but not reduced; any reduction
  is a quality trade-off for the owner.

## Simulator sanity (tvOS simulator, not a TV)

tv/bandwidth `c3ad3460` simulator build, warm, 3840x2160, pass log on:
bridge, b30 and cryo all ran their windows with no GPU errors; passes
7.4 / 12.0 / 9.6 a frame (as the Mac); the simulator cannot sample at stage
boundaries, so no per-pass times there. Its frame times are the Mac GPU's.

## Device results (A12, 2026-10-22)

One session on the owner's Apple TV 4K (A12, tvOS 27.0), integrated build
**tv/integrate `3ed14e70`** (tv/shaders + tv/bandwidth + tv/textures,
`HALO_TEXTURE_FORMAT` at its default `exact`), installed once from
`<scratch>/tv-deploy-integrate`; before/after are environment switches on
that one binary (`HALO_METAL_HOIST=0` = before). 3840x2160, high-res HUD,
steady window (scene minus its first 5 s), all warm except the first
before-run of each scene. Order: r1-before (cold, warm), r1-hoist,
r1-hoist+discard, r2-hoist+discard, r2-hoist, r2-before, pass-split,
pass-hoist (~32 min of runs). config.toml/init.txt restored and verified
identical after every batch (16/16). Raw: `<scratch>/tvbench-out/device-bandwidth`.
The A12 **does** sample timestamps at stage boundaries (`stage 1`), so the
pass log has real per-pass GPU times on the TV.

| run | scene | fps | p50 | p95 | p99 | >33.3 | >50 | GPU busy % | passes |
|---|---|---|---|---|---|---|---|---|---|
| r1 before (cold) | bridge | 54.3 | 16.7 | 33.5 | 33.7 | 140 | 2 | 95.7 | 9.8 |
| r1 before | bridge | 54.8 | 16.7 | 33.5 | 33.7 | 150 | 0 | 96.4 | 9.8 |
| r1 hoist | bridge | 56.0 | 16.7 | 33.5 | 33.6 | 114 | 0 | 99.9 | 7.3 |
| r1 hoist+discard | bridge | 55.0 | 16.7 | 33.5 | 33.7 | 145 | 0 | 99.9 | 7.2 |
| r2 hoist+discard | bridge | 54.3 | 16.7 | 33.5 | 33.7 | 158 | 0 | 99.9 | 7.3 |
| r2 hoist | bridge | 52.0 | 16.7 | 33.6 | 33.7 | 221 | 0 | 99.9 | 7.3 |
| r2 before | bridge | 50.7 | 16.7 | 33.6 | 33.8 | 259 | 0 | 97.5 | 9.8 |
| r1 before (cold) | b30 | 49.6 | 16.7 | 33.5 | 33.6 | 437 | 0 | 100 | 18.0 |
| r1 before | b30 | 49.4 | 16.7 | 33.5 | 33.6 | 449 | 0 | 100 | 18.0 |
| r1 hoist | b30 | 53.8 | 16.7 | 33.5 | 33.6 | 287 | 0 | 100 | 12.0 |
| r1 hoist+discard | b30 | 53.7 | 16.7 | 33.5 | 33.6 | 290 | 0 | 100 | 12.0 |
| r2 hoist+discard | b30 | 51.1 | 16.7 | 33.5 | 33.6 | 394 | 0 | 100 | 12.0 |
| r2 hoist | b30 | 47.7 | 16.7 | 33.6 | 33.6 | 581 | 0 | 100 | 12.0 |
| r2 before | b30 | 41.5 | 16.8 | 33.6 | 33.6 | 802 | 0 | 100 | 18.0 |
| r1 before (cold) | cryo | 57.8 | 16.7 | 16.9 | 33.6 | 69 | 21 | 91.9 | 13.2 |
| r1 before | cryo | 57.7 | 16.7 | 16.9 | 50.0 | 60 | 32 | 92.0 | 13.1 |
| r1 hoist | cryo | 58.3 | 16.7 | 17.0 | 33.4 | 52 | 11 | 100 | 9.2 |
| r1 hoist+discard | cryo | 57.7 | 16.7 | 17.0 | 50.0 | 56 | 31 | 100 | 9.1 |
| r2 hoist+discard | cryo | 57.9 | 16.7 | 16.9 | 50.0 | 51 | 31 | 100 | 9.1 |
| r2 hoist | cryo | 57.9 | 16.7 | 16.9 | 50.1 | 49 | 30 | 100 | 9.1 |
| r2 before | cryo | 57.0 | 16.7 | 17.0 | 50.0 | 86 | 34 | 97.9 | 13.1 |

(p95 is pinned at 33.5 ms whenever more than 5% of frames miss a vsync, so
it cannot show these gains; the >33.3 count and fps can. GPU busy rises to
100% with hoisting because more frames are drawn in the same time.)

Reading it, with care for a drift through the session (below): compare
runs that were adjacent in time. r1-before -> r1-hoist (consecutive):
b30 49.4 -> 53.8 fps (+9%), frames over 33 ms 449 -> 287; bridge
54.8 -> 56.0 (+2%), 150 -> 114. r2-hoist -> r2-before (consecutive):
b30 47.7 vs 41.5 (+15%), 581 vs 802; bridge 52.0 vs 50.7 (+3%), 221 vs
259. Cryo: fps the same (57-58: it holds 60 except the first ~10 s, the
tutorial's convolution), frames over 33 ms 60/86 -> 52/49; frames over
50 ms not reliably better (32/34 -> 11/30). **Hoisting helps b30 by ~10-15%
and bridge by ~2-3% on the A12, but no scene reaches 60 fps at 4K.**

**Depth discard** (`HALO_METAL_DEPTH_DISCARD=present`): no gain beyond the
noise (r1 b30 53.7 vs 53.8; r2 runs favour whichever ran earlier). 0 loads
after a discard. Leave it off.

### Why: the A12 is shading-bound, not attachment-bound

The hoisted pass log on the TV shows the frame is now one 4K pass plus tiny
ones, with attachment traffic down to ~75-90 MB a frame (b30 before: the
split run moved 1.4 + 1.5 GB). That one pass's fragment stage takes:

| scene | the screen pass's fragment time (A12, hoisted) | budget |
|---|---|---|
| a10 bridge | 17.9 ms | 16.7 |
| b30 beach | 19.9 ms | 16.7 |
| cryo bay (after the blur) | 13.5 ms | 16.7 |

Each extra 4K load+store pass cost 0.4-0.6 ms on the A12 (from the split
run: 15-19 more 4K passes added 6.7-9.6 ms), so the 3-6 reloads hoisting
removed were worth ~1.5-3.5 ms: real, but the remainder is shading and
blending of the scene itself.

### Per-phase GPU time (A12, pass per phase, diagnostic)

Fragment ms a frame with `HALO_METAL_PASS_SPLIT=phase`, hoisting off.
Each phase pass carries a 4K load+store (~0.4-0.6 ms) the real frame does
not, so subtract roughly that from each line; the ranking holds.

| scene | phases, fragment ms/frame (A12) | sum |
|---|---|---|
| bridge | diffuse lights 4.03, reflections 3.66, textures 3.04, queued transparents 2.90, specular 2.19, lightmaps 2.06, sky 2.01, fog planes 1.99, reflection masks 1.98, shadows (screen) 1.95, models 1.09 | 27.8 |
| b30 | **queued transparents 10.06**, shadows 3.55, textures 3.06, fog planes 2.36, lightmaps 2.00, sky 1.98, models 1.85, HUD 1.06 | 27.1 |
| cryo | reflections 4.10, textures 3.28, shadows 2.93, fog planes 2.38, reflection masks 2.17, lightmaps 2.06, models 1.51, transparents 1.36, screen effect 0.40 | 20.7 |

The b30 transparents (the firefight's particles, plasma and effects) are
the single biggest cost anywhere; the a10 fog planes are ~2-2.4 ms in both
a10 scenes (less 0.5 ms of split overhead: ~1.5-1.9 ms), not the limiter.

### Drift and the owner's "lag that builds up"

- Within a run (10 s buckets, `<scratch>/bw/device-drift.txt`): no steady
  drift; b30 is flat (e.g. r2-before 43.0, 39.8, 41.9, 41.6, 42.4, 41.9
  fps). The bridge's third 10 s is slow in every run (scene content).
- **Across the session, yes:** the same configuration got slower the
  longer the TV had been at 100% GPU: b30 before 49.4 fps (~0-5 min in)
  -> 41.5 (~25 min in); b30 hoisted 53.8 -> 47.7 -> 47.7 (end of
  session). That is a ~12-15% loss over ~25-30 min of sustained full GPU
  load, which looks like thermal throttling (not measured: thermalState is
  not in the frame log, and no code was added for it). It fits the
  owner's report of late-a10 slowdowns, and it means the A12 needs margin
  below 16.7 ms, not just to reach it. Logging
  `NSProcessInfo.thermalState` per interval would confirm it.

## What remains for 4K60 on the A12

Bandwidth work is largely done (one 4K pass a frame). The remaining gap is
fragment work: bridge needs ~1-2 ms less, b30 ~3-4 ms less, *plus*
headroom for thermal throttling (~15%). That requires a quality trade-off
or a resolution change; none is implemented (owner's decision). Options,
with the evidence for their size:

1. **Dynamic resolution / a lower 4K render scale** (e.g. 1800p or 1872p
   upscaled to 2160p): fragment time scales roughly with pixels; 1800p is
   69% of 4K's pixels, ~-5 to -6 ms on b30. Simplest, covers every scene,
   costs sharpness. 1080p already holds 58-60 fps.
2. **Transparents at half resolution** (render queued transparents into
   a half-res target and composite): b30 ~10 ms -> ~3-4 ms (A12 split
   numbers); softer particles/effects. A real feature, not a switch.
3. **Screen convolution at half resolution** (cryo's tutorial blur and
   scopes): removes the cryo bay's first-10 s dip (convolution frames run
   ~41-45 fps); a blur loses little at half res.
4. **ASTC/compressed textures** (#103's `HALO_TEXTURE_FORMAT=astc`):
   texture sampling bandwidth, measured separately by #103.

# Texture formats (A12)

Branch `tv/textures`. Device results are in "A12 device" at the end; the
sections before them were measured on the Mac mini (M4) or in the tvOS simulator (correctness,
memory and fidelity; not A12 GPU time).

## What changed

The A12 has no BC formats (`supportsBCTextureCompression` is NO, and so is
the tvOS simulator's). Before this branch, `xbox_textures.c` decoded every
DXT1/3/5 level to BGRA8 **inside the wasm2c guest** (bounds-checked
loads), and uploaded it into shared-storage textures.

- **Default `display.texture_format = "exact"`.** The guest passes the
  map's blocks to a new host import, `host_metal_texture_bc`. The host
  decodes them natively (`port/macos/host/host_textures.c`; large levels
  are split across cores with `dispatch_apply`) and blits them into a
  **private-storage** texture. A12-and-later GPUs compress private
  textures losslessly; shared ones are not compressed. The texels are
  bit-identical to the guest's decode (`tools/test_texture_formats.py`
  compiles the guest's decoder from `xbox_textures.c` and compares).
  Same-frame Mac screenshots with `HALO_METAL_NO_BC=1` are byte-identical
  between `guest` and `exact`.
- **Opt-in `"astc"`.** ASTC 4x4 LDR: 16 bytes per 4x4 block, a quarter of
  BGRA8 and twice BC1. It is lossy on some textures (below), so it is not
  the default.
- **`"guest"`.** The old path, for A/B on one build.
- **A Mac with BC is unchanged**: BC1/2/3 are uploaded as they are.
  `HALO_METAL_NO_BC=1` forces the TV's path on a Mac.
  `HALO_METAL_TEXTURE_SHARED=1` keeps the converted textures in shared
  storage, to separate the storage effect from the format effect.
- **Logging.** With `HALO_FRAME_LOG` on, the host logs `host: textures: ...`
  at exit: sampled-texture MiB (the converted DXT part separately), the
  peak, the conversion count and time.

No on-disk transcode cache was added. Conversion costs tens of ms per
scene for `exact` (below), so a cache in Library/Caches would not pay for
its space.

## Formats a10 and b30 actually use (map survey)

| | DXT1 | DXT3 | DXT5 |
|---|---|---|---|
| textures (2D + cube; no DXT 3D) | 607 | 254 | 15 |
| MiB in the maps (all levels) | 24.7 | 19.3 | 3.9 |
| as BGRA8 | 197.2 | 77.1 | 15.8 |
| as ASTC 4x4 | 49.3 | 19.3 | 3.9 |

DXT is 44% of a10's 48.7 MiB of bitmap data and 61% of b30's 35.3 MiB.
Of the DXT1 textures, 552 of 607 contain transparent (3-colour, index 3)
texels. That rules out a 16-bit B5G6R5 fallback for most of them, and it is
why DXT1 is not an exact ASTC mapping everywhere.

## ASTC fidelity vs the exact decode (Mac, all DXT blocks of a10 + b30)

Measured with `python3 tools/texture_formats.py a10.map b30.map`. The
reference is the exact BGRA8 decode; errors are in 255ths, per channel, and
alpha is included.

| format | ASTC PSNR dB | blocks max err <=2 | 3-4 | >4 | per-texture PSNR p1/p5/p25/p50 | textures with a block err >4 | >32 |
|---|---|---|---|---|---|---|---|
| DXT1 | 35.9 | 83.2% | 10.2% | 6.5% | 28.0/31.1/35.0/39.4 | 83% | 81% |
| DXT3 | 31.3 | 19.5% | 6.2% | 74.3% | 28.3/28.9/30.1/32.2 | 98% | 89% |
| DXT5 | 31.2 | 29.3% | 13.8% | 56.9% | 28.5/28.5/30.0/31.9 | 100% | 100% |

What the numbers mean:

- **Exact up to rounding (<=2/255).** These blocks map onto one ASTC
  colour line:
  - DXT1 blocks of 4 colours, 3 colours without transparency, or
    transparency plus one colour;
  - DXT3/5 blocks of one alpha.
- **Over 4/255, DXT1 (6.5% of blocks).** All of the larger errors are in
  the **colour** of opaque texels in blocks that mix transparency with
  several colours, where no ASTC 2-partition pattern separates them.
  - Alpha is never off by more than 4.
  - Two-partition endpoints lose 1-2 bits.
- **DXT3/DXT5 alpha.** Varying alpha gets 3-4 levels on a dual plane,
  against DXT3's 16 and DXT5's 8. That is the error in 74% / 57% of their
  blocks, typically 16-32/255 on alpha gradients.
- **Verdict: not near-lossless.** Most textures have at least one block
  that is visibly off, so `astc` stays **opt-in**.
  - A near-lossless DXT3/5 mode would need ASTC 4x4 at higher weight
    precision, which does not fit 128 bits with 4-bit alpha; or an
    alpha-only second texture, which needs shader changes.
  - A per-texture policy (ASTC only for opaque DXT1, BGRA8 for the rest)
    is possible but was not built or measured.
- **Validation of the ASTC encoder.** Every ASTC block it produced for the
  4.95M real blocks, plus 200k random ones, was decoded by ARM's `astcenc`
  (Apache-2.0; compiled in scratch, not vendored). 0 structural
  mismatches; only ±1 rounding where astcenc's float path rounds
  differently. Same-frame Mac screenshots of `astc` diverge from `guest`
  from the first frame that draws the level (PSNR 13.9 dB on that bridge
  frame, which is largely a frame-timing difference, so it is not a
  fidelity figure). Use the per-block table above.

## Texture memory and conversion time (Mac M4, `HALO_METAL_NO_BC=1`, 1080p, 25 s from the saved state)

Sampled textures are all sampled textures, not only DXT. The size is
Metal's `allocatedSize`, or the computed size where that is 0.

| scene | path | sampled MiB | of it converted DXT | DXT textures converted | conversion time | Metal device allocations |
|---|---|---|---|---|---|---|
| a10 bridge | Mac BC (reference) | 16.9 | 0 | 0 | 0 | 234.5 MiB |
| a10 bridge | guest | 40.3 | (BGRA8 in guest) | 69 | not timed (in guest) | 257.8 MiB |
| a10 bridge | exact | 40.3 | 29.2 | 69 (5.4 MiB blocks) | 14.3 ms | 257.8 MiB |
| a10 bridge | astc | 18.6 | 7.5 | 69 | 76.0 ms | 236.0 MiB |
| b30 beach | Mac BC (reference) | 93.5 | 0 | 0 | 0 | 311.2 MiB |
| b30 beach | guest | 137.9 | (BGRA8 in guest) | 175 | not timed | 355.6 MiB |
| b30 beach | exact | 137.9 | 53.1 | 175 (7.7 MiB blocks) | 20.3 ms | 355.5 MiB |
| b30 beach | astc | 98.9 | 14.0 | 175 | 89.6 ms | 316.6 MiB |

So at these scenes the BGRA8 fallback costs +23 MiB (bridge) and +44 MiB
(b30) of texture memory over BC. ASTC gives back 22 MiB and 39 MiB of
that.

Conversion speed on one M4 core is ~900 Mtexels/s to BGRA8 and ~37
Mtexels/s to ASTC (68 Mtexels/s for DXT1 alone). In the tvOS simulator
(conversion on the host CPU, timed the same way), exact takes 34.6 ms
(bridge) and 50.5 ms (b30) for the whole load, and ASTC 346 ms (bridge).
A12 CPU times are not measured.

## Simulator (tvOS simulator on the M4, 2160, warm, 1 run each; not A12 numbers)

| scene | path | frames | wall p50 | p95 | max | GPU p50 | GPU p95 |
|---|---|---|---|---|---|---|---|
| a10 bridge | guest | 4517 | 5.1 | 20.4 | 249.1 | 0.3 | 0.5 |
| a10 bridge | exact | 4674 | 5.1 | 20.3 | 229.7 | 0.3 | 0.5 |
| a10 bridge | astc | 4822 | 5.0 | 20.3 | 247.8 | 0.3 | 0.5 |
| b30 beach | guest | 9308 | 5.1 | 19.7 | 386.8 | 0.3 | 0.5 |
| b30 beach | exact | 9381 | 5.1 | 19.7 | 350.8 | 0.3 | 0.5 |

The simulator confirms the path:
- it reports no BC;
- the log says `DXT textures decoded on the host` or `converted to ASTC`;
- all runs are valid.

Its GPU is the Mac's, so these timings cannot show A12 texture bandwidth.
Each path's largest frame is the state-load frame: `exact` is 20-36 ms
shorter than `guest` (one run each, indicative only).

## A12 device: guest vs exact vs astc (4K)

Measured 2026-10-23 on the owner's A12 Apple TV 4K (tvOS 27.0, CoreDevice
`443953B8-9072-5EAD-A947-95563A1C3D52`) with the integrated build
**tv/integrate 3ed14e70** (tv/shaders + tv/bandwidth + tv/textures). It was
already installed by #101 and the batch checked build-info before running.
All three formats ran on the same build, selected by `HALO_TEXTURE_FORMAT`.

- **Cold is truly cold here:** the integrated salt renames the shader entry
  points and the bench resets the shader manifest.
- **Plan:** per format and scene, one salt with 1 cold launch then 2 warm
  launches.
- **Raw data:** `<scratch>/tex/device/{guest,exact,astc}`, plus the
  ordering check in `<scratch>/tex/device-check`.
- **Validity and restore:** all 27 runs are valid. config.toml and init.txt
  were restored and verified byte-identical after every batch.

Batch 1 ran in the order guest, then exact, then astc (bridge and b30 each;
~21 min):

| scene | format | mode | fps (whole window) | steady frames >33.3 ms | steady GPU mean ms | GPU busy % | load frame ms | sampled textures MiB | DXT conversion (whole load) |
|---|---|---|---|---|---|---|---|---|---|
| a10 bridge | guest | cold | 53.2 | 175 | 22.7 | 97 | 2584 | 56.7 | in guest (not timed) |
| a10 bridge | guest | warm | 52.3 / 50.9 | 194 / 242 | 24.2 / 25.3 | 99 / 99 | 572 / 574 | 56.7 | in guest |
| a10 bridge | exact | cold | 49.1 | 295 | 28.8 | 97 | 2583 | 56.7 (43.7 DXT) | 113 tex, 7.4 MiB blocks, 47 ms |
| a10 bridge | exact | warm | 50.0 / 50.1 | 290 / 273 | 28.0 / 26.5 | 99 / 99 | 571 / 558 | 56.7 | 46 ms |
| a10 bridge | astc | cold | 49.6 | 296 | 28.3 | 97 | 2716 | 24.3 (11.3 DXT) | 284 ms |
| a10 bridge | astc | warm | 49.9 / 49.4 | 269 / 314 | 26.7 / 28.3 | 99 / 99 | 716 / 694 | 24.3 | 257 ms |
| b30 beach | guest | cold | 47.6 | 585 | 31.4 | 98 | 3161 | 137.4 | in guest |
| b30 beach | guest | warm | 47.5 / 46.9 | 593 / 603 | 31.6 / 32.3 | 99 / 99 | 596 / 572 | 137.3 | in guest |
| b30 beach | exact | cold | 48.1 | 565 | 31.0 | 98 | 3217 | 137.0 (52.6 DXT) | 182 tex, 7.8 MiB, 50 ms |
| b30 beach | exact | warm | 47.9 / 47.2 | 576 / 601 | 31.2 / 32.1 | 99 / 99 | 576 / 584 | 136.8 | 47 ms |
| b30 beach | astc | cold | 48.3 | 575 | 31.0 | 98 | 3267 | 98.3 (14.0 DXT) | 252 ms |
| b30 beach | astc | warm | 47.4 / 46.9 | 599 / 629 | 31.7 / 32.0 | 99 / 99 | 738 / 733 | 98.6 | 236-249 ms |

Batch 2 is the ordering and storage check: a10 bridge only, in the reverse
order exact, then exact with `HALO_METAL_TEXTURE_SHARED=1`, then guest
(~9 min):

| format | cold fps | warm fps | warm steady >33.3 ms | warm steady GPU mean ms |
|---|---|---|---|---|
| exact (private storage) | 54.1 | 52.6 / 52.3 | 207 / 216 | 24.0 / 25.6 |
| exact, shared storage | 51.8 | 51.4 / 51.5 | 232 / 236 | 24.5 / 24.9 |
| guest | 51.5 | 51.6 / 51.2 | 239 / 246 | 25.8 / 26.4 |

## What the device shows

- **No measurable fps or GPU-time effect from the texture format at 4K.**
  - In both batches the format run *first* was the fastest. In batch 1 exact
    trailed guest by 1.6 fps on the bridge; in batch 2 exact led guest by
    1.0 fps.
  - Run order and session drift (thermal) move bridge fps by ~2, more than
    any format does. Every run is still GPU-bound at 97-99% busy.
  - On b30 all three formats agree within 0.5 fps (47.1-47.5 warm).
  - ASTC, at a quarter of the texture bytes, is no faster than BGRA8 on
    either scene. So texture bandwidth is not what limits the A12 at 4K;
    fill and blending (#101's area) are.
  - Private storage, which lets the A12 compress losslessly, against shared
    storage: +1 fps in batch 2, within the same noise.
- **Memory:** the clear, real effect of ASTC.
  - Sampled textures, bridge: 56.7 MiB to 24.3 MiB (-32 MiB, -57%).
  - Sampled textures, b30: 137.3 MiB to 98.5 MiB (-39 MiB, -28%).
  - The DXT part shrinks 4x (43.7 to 11.3 MiB and 52.6 to 14.0 MiB).
    `exact` uses the same memory as `guest`.
- **Load time:**
  - `exact` converts a scene's DXT in 46-50 ms on the A12. Its load frame
    is the same as `guest`'s within noise (558-584 ms against 572-596 ms
    warm), so moving the decode out of the wasm2c guest did not measurably
    shorten the load.
  - `astc` costs 236-284 ms of encoding per scene. That adds ~140 ms to the
    warm load frame (694-738 ms) and ~130 ms to the cold one.
  - Cold load frames (2.6 s bridge, 3.2 s b30) are ~3 s of shader compiles
    whatever the format: texture conversion is not part of the cold-load
    problem.
- **Conclusion:**
  - The default stays `exact`: identical texels, the same speed, no decode
    in the guest.
  - `astc` stays opt-in. It saves 32-39 MiB of texture memory at these
    scenes, gains no fps, costs ~0.15-0.3 s per load, and has the fidelity
    loss measured above. It is worth enabling only if memory pressure ever
    matters on the TV (an app-memory limit or jetsam).
  - No on-disk transcode cache: `exact` is fast enough, and caching `astc`
    would only win back ~0.25 s per load.

# Play telemetry (tv/telemetry)

The fixed scenes above are 35-80 s; the owner's late-a10 lag ("builds up,
smooth a while, lags again; quiet downtime, not fights; never early in the
level; like a buffer filling and being freed") came after ~20+ minutes, in
**two-player split-screen co-op**. The TV build now records itself while it
is played (`port/macos/host/host_telemetry.h`), and `tools/tv_telemetry.py`
pulls and reports it.

## What is recorded

On by default in the TV build (`debug.telemetry = "auto"`; `"off"` in
config.toml or `HALO_TELEMETRY=off` turns it off; a Mac records only with
`"on"`). JSON lines in the app's container, written in parts:
`Library/Caches/OpenCE/telemetry/<UTC start>-<pid>.p001.jsonl`, `.p002`, ...,
a new part at 8 MB or 30 minutes (`HALO_TELEMETRY_PART_MB`/`_KB`/`_MIN`),
each beginning with the session's line plus `"part"`. A closed part is
static, so `tv_telemetry.py pull` can copy it while the game runs
(devicectl's copy of a growing file breaks off at 10 MB). The newest 40
sessions and at most 200 MB of parts are kept (rotated at each part), 256
MB a session at most; ~1.7 KB a second, so a 2-hour session is ~12 MB.

- **A second** (`"k":"s"`): wall and game time; map, BSP, local players,
  player views drawn (1 in a cinematic), each view's camera position;
  frames, fps, wall p50/p95/max, frames over 16.7/33.3/50 ms, **display
  slots missed** (a frame of n refresh intervals missed n-1); guest ms and
  host ms (mean, p95), **GPU command-buffer ms p50/p95/max** (read a second
  late from the frame log's ring: no GPU wait), the GPU span p95, hoisted
  passes' ms; compile ms, drawable / in-flight / readback waits, sleeps;
  draws, passes, copies, shadow / fog / transparent draws, and the same **per
  view** (player 1-4, other) with each view's main-thread ms; textures
  uploaded (count, MB), DXT-converted on the host (count, MB, ms), dropped;
  bytes streamed through the frame's ring and ring overflows; the main
  thread's file reads and writes (count, MB, ms) and shader manifest writes
  (count, KB, ms); `phys_footprint`, `os_proc_available_memory`, Metal's
  `currentAllocatedSize`; `NSProcessInfo.thermalState`; whether the app is
  active; **the render size, the drawable's and their scale** (for dynamic
  resolution); the recorder's own main-thread and writer ms.
- **A hitch** (`"k":"hitch"`, a frame over 50 ms): its whole breakdown, the
  above counts for that one frame, and the last event in it (a map or saved
  state opened, the views changing, the app's activity).
- **Events**: map and saved-state opens, views 1 <-> 2, active/inactive,
  thermal state changes. **End**: totals at a clean exit (tvOS rarely gives
  one: the file is complete without it).

It runs on the frame log's breakdown (level 2, a 1024-frame ring, no CSV).
The writer is a serial utility-QoS dispatch queue: the main thread formats
a second's lines into a buffer and hands it over once a second; `fwrite` +
`fflush`, never `fsync`.

## Cost (measured)

| where | telemetry off | on |
|---|---|---|
| A12, bridge state, 4K, 75 s x2 (frames / p50 / p95 / >33 ms) | 4189, 4285 / 16.69, 16.68 / 17.05, 17.03 / 152, 147 | 4287, 4268 / 16.68, 16.68 / 17.02, 17.07 / 147, 162 |
| A12, its own clock | - | 0.17 ms a second on the main thread (max 0.44), 0.27 ms a second on the writer |
| Mac M4, bridge, 720p vsync off, 40 s x2 | 120.0 fps | 120.0 fps; 0.10 ms/s main, 0.06 ms/s writer |
| Mac M4, process CPU, 40 s x3 | 9.31 s | 9.19 s (noise) |

0.17 ms a second is 0.017% of the main thread: within the runs' noise in
every measure. It stays on by default.

## Getting the data

    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
    python3 tools/tv_telemetry.py pull                   # ~/code/halo/tv-telemetry/443953B8/
    python3 tools/tv_telemetry.py report --last 1 --out ~/code/halo/tv-telemetry/report.md

`pull` lists the TV's telemetry folder and copies only new or grown files
(`devicectl device copy from`, the app's container; nothing goes to the TV;
the app need not run). Live mode exists but is off: `debug.telemetry_live =
"<Mac IPv4>:47777"` in the TV's config.toml and `tools/tv_telemetry.py
listen` on the Mac. It is plain UDP to a LAN address: tvOS asks for
local-network permission the first time (Info.plist already has
`NSLocalNetworkUsageDescription`, for network play); no entitlement is
needed for that; not tried on the TV (the owner would see the prompt).

## The report

`report` gives a verdict (the metrics that are higher in seconds that
missed >= 3 display slots than in seconds that missed none, with their
correlation with the missed slots; the thermal state and its correlation
with GPU ms and with the misses; the lag's period by autocorrelation; what
the hitches were), time by map and players, split screen per view, by BSP,
the worst 10-unit position squares, the hitches by cause and the worst ten,
and per session: the lag's stretches (how long, how far apart, when the
first began), trends (footprint, Metal allocations, GPU and guest ms, fps,
per minute; footprint drops of over 20 MB; texture sweeps and their period),
thermal state against GPU ms, and a minute-by-minute table.

## First device session (A12, 4K, 2026-10-23)

The split-screen bench scene (`tools/tvos_bench.py`'s `split`:
`player_spawn_count 2` then `map_name levels\a10\a10`; no second controller,
player 2 stands still) works: the opening cinematic is one view, then two
views from ~234 s. Its 50 s window: wall p50 16.68, p95 16.85, max 17.1 ms,
**GPU 15.1 ms mean (p95 15.6)**, guest 4.6 ms, host 3.0 ms; 0 frames over
33 ms. Two views at the cryo bay start: 1020 draws a frame (510 each),
7 passes (one 4K screen pass per frame: views share it, the hoisting
holds), 25 shadow draws, 85 transparent draws, ~800 MB/s streamed through
the ring. The single-view cinematic before it was slower (GPU 17.5 ms p50,
179 missed slots a minute): the opening's fog. So two views at the bay are
not over budget; the owner's lag was later in the level, with both players
moving, and the next play session's telemetry is what answers it.

The render target was 3840x2160 and the **drawable 1920x1080** on this TV
in every run. The session line (which records the screen) says why:
`screen 1920x1080 points, native 1920x1080 at 1.0x, mode 1920x1080`: **the
Apple TV's output was 1080p** during these runs (a 4K output mode would be
native 3840x2160 at 2.0x). A "4K" run here draws 3840x2160 and scales it
down to the 1080p screen at the present: 4x the pixels of what is shown.
Whether the owner plays in this mode, or the TV switches mode (Match
Content, or the TV's input), is for the owner to say; every session's first
line now records it, and each second records render and drawable sizes.

## Code findings on the periodic-lag suspects

1. **Shader manifest (host_shader_manifest.c): not it.** It appends one
   record per new shader or pipeline with a single `write()` on the main
   thread, no `fsync`, and never rewrites the file (only `ftruncate` at open
   for a damaged tail or another build's file). Cost does not grow with the
   file. The telemetry times every append (`manifest_ms`); on the device
   they were all in the first seconds (with the compiles).
2. **Texture sweep + DXT conversion: the strongest code suspect for a
   sawtooth.** `xbox_textures.c`'s `xgpu_texture_cache_begin_frame` runs
   every 600 frames (10 s at 60 fps) and deletes every host texture unused
   for 1800 frames (30 s), and bumps `texture_drop_serial`, which empties
   the `recent_textures` fast path for every texture. A texture that comes
   back into view (walking back through an area, the second player looking
   elsewhere) is made again: on the A12 (no BC) that is a synchronous
   `host_metal_texture_bc` DXT conversion on the main thread
   (dispatch_apply across cores, but the main thread waits), ~37 Mtexels/s
   for ASTC. In quiet downtime fewer textures are drawn, so more age out;
   with two players in two places, more come and go. Recorded now:
   `tex_deletes` (the sweep's), `tex_converts`/`tex_convert_ms` per second
   and per hitch. The report gives the sweep's period. If the next session
   confirms it, the fix is a longer idle time or a byte budget instead of a
   fixed 30 s (not changed here: no data yet, and it changes memory use).
3. **The page-hash write watch** (`host_watch_hash.c`, the guest's
   `web_memory_watch.c`): pages hashed unchanged 30 times are re-hashed
   every 60 frames, all at once if they loaded together; freshly read pages
   every frame for 30 frames. A periodic guest-time cost proportional to
   the texture bytes drawn: shows as `guest_ms` p95 in the telemetry.
4. **Ring overflow**: past 64 MB a frame the host makes a new 16 MB buffer
   per overflow per frame. The device split scene streamed ~13 MB a frame
   (800 MB/s): no overflows (`ring_overflows` 0). Recorded.
5. **Memory**: footprint 878 -> 982 MB over the 5-minute split run (Metal
   406 MB; ~1.2 GB available). Recorded per second; the report gives the
   slope and any bulk drops.
6. **Thermal**: thermalState stayed nominal in this ~12-minute session. The
   earlier session's 12-15% drop over 25-30 min of full GPU load is the
   thing to confirm: the report correlates thermal state with GPU ms.
7. **Game caches** (source/cache): the texture cache is 256 MB of 16 KB
   pages, LRU, flushed only at map change; the sound cache 16 MB, LRU,
   decoded per packet. `lruv_block_new` walks the cache's block list on
   every miss and `lruv_cache_verify` walks it again on every new block and
   delete (asserts are on), so a miss costs more as the cache fills over a
   level, a cost that grows with play time; it is guest time on a miss
   (cluster changes: `predicted_resources_precache`), recorded as guest ms
   and hitches. Object garbage collection (5-10% free, 50 -> 30 garbage
   objects) is a sawtooth too, unmeasured.
8. **Split-screen specifics**: per view, everything but mirrors and detail
   objects is drawn again (shadows into the shadow target, water bump,
   transparents, fog, lens flares, HUD); the second view's start does not
   end the 4K screen pass (same targets: `host_metal_set_targets` returns
   at once; the viewport clear is a quad), and small offscreen passes are
   still parked and hoisted. Recorded per view.

# Half resolution (tv/halfres)

Two opt-in settings, both default off and with an env override for A/B
on one binary: `display.half_res_convolution` /
`HALO_HALF_RES_CONVOLUTION` (this section's first part) and, later,
`display.half_res_transparents`. Diagnostic: `HALO_HALF_RES_CYCLE=N`
alternates off and on every N frames in one run (frozen-frame fidelity),
and `tools/halfres_compare.py` gives PSNR, max, mean and p99.9 per region
(all / touched / untouched against a frame without the effect / edges /
interior / centre / border). Everything "half" is half the scene's own
resolution (`halfres_scene_scale` in d3d8_metal.c, the one adapter for
dynamic resolution), not 1920x1080.

## Screen convolution at half resolution

The blur and the scopes' warp (`_rasterizer_screen_effect`) draw their
passes between the first and the last into a pair of half-scale targets
in the screen's game units (so the game's quads and texture coordinates
are unchanged); pass 0 reads the screen at full resolution, the last pass
draws into it, sampling bilinearly. Pass count and radii are unchanged.
Split screen never convolves (the game asserts one window).

Fidelity (Mac M4, 3840x2160, cryo state frozen with `game_speed 0`,
scripted `cinematic_screen_effect_set_convolution`, last off vs last on
frame of a 30-frame cycle; "touched" = pixels the effect changes against a
frozen frame without it):

| effect (extra passes, type, radius) | passes | all PSNR | max | p99.9 | edges PSNR | centre PSNR | untouched max |
|---|---|---|---|---|---|---|---|
| warp 1 x r1 | 6 | 45.3 | 42 | 11 | 37.2 | 47.6 | 18 |
| warp 1 x r3 (as the tutorial: 6 passes) | 6 | 49.2 | 31 | 8 | 45.4 | 50.9 | 21 |
| warp 1 x r9.7 | 6 | 47.1 | 33 | 19 | 46.0 | 53.6 | 23 |
| warp 0 x r2 (a scope: 4 passes) | 4 | 48.8 | 30 | 8 | 41.9 | 49.8 | 12 |
| blur 0 x r0.5 | 4 | 50.6 | 32 | 5 | 46.1 | 52.9 | 11 |
| blur 0 x r1 | 4 | 49.7 | 40 | 7 | 47.5 | 53.1 | 27 |
| blur 1 x r3 | 6 | 44.0 | 57 | 25 | 42.4 | 55.4 | 45 |

The effect is full-screen, so "untouched" pixels (unchanged by the full
resolution blur, flat areas) can still differ by a few levels: the
half-resolution chain rounds to 8 bits at a quarter of the pixels. No
radius threshold: the smallest radii are as close as the largest.

Byte-identical with the feature off: the base build (8bc487e3) and this
build, setting off, the same frozen frame: **identical** (one launch each;
two launches of one build differ by up to 1-17 levels from animation not
stopped by `game_speed`, so only one same-numbered pair was comparable).

GPU (Mac, pass log in the same run, the frozen 6-pass warp): the screen
effect's passes 3.67 -> 1.47 ms of fragment time a frame (5 of the 6
passes at 1920x1080), frame fragment total 8.89 -> 6.77 ms; 15 passes a
frame either way (the screen pass still ends for the first pass, as it
did).

A12 (bedroom TV, cryo, 3840x2160 picture; ABBA, consecutive, warm, one
binary `5fde174e`; window = the scene's 50 s; the tutorial blur is its
first ~12 s):

| run | window fps | >33.3 | >50 | first 12 s fps |
|---|---|---|---|---|
| off (1st) | 56.2 | 115 | 28 | 44.5 |
| on (2nd) | 57.7 | 88 | 3 | 50.5 |
| on (3rd) | 57.2 | 103 | 6 | 48.5 |
| off (4th) | 56.5 | 110 | 39 | 45.7 |

Frames over 50 ms 28/39 -> 3/6; the blur's seconds 44.5/45.7 -> 50.5/48.5
fps. The rest of the frame (fog room) is unchanged.

## The late-level lag: measured and fixed (2026-10-23)

The owner's report: late in a10, two-player co-op, quiet downtime,
stretches of lag that built up, went smooth and came back. No new play
session was on the TV, so the reproduction is a bench scene:
`tools/tvos_bench.py`'s **latelag** (a10 split screen for 10 minutes: the
opening at 8x, both players out of the cryo tubes, then both moved between
the cryo bay (BSP 0) and the bridge (BSP 1) every 40 s, player 1 turning
and looking about). It is scripted by `debug.test_commands`
(`HALO_TEST_COMMANDS`, console commands at game seconds), which works on
the TV, which has no telnet console:

    python3 tools/tvos_bench.py run --target device --install --signed APP \
        --plan latelag/2160/1 --modes warm --out OUT
    python3 tools/tv_telemetry.py pull     # the run's per-second telemetry

Each area's textures go unseen for 40 s, longer than the old 30 s idle
time, and then come back. The A12 numbers below come from four runs
interleaved base, fix, base, fix (base = the bench scene's commit without
the fixes, fix = with them), TV output 1080p, rendering 3840x2160, warm.
The columns are for the 554 s from the first return to an area (45 s after
two views), which leaves out the shader compiles of the first visit.

| A12, latelag, 554 s | base 1 | fix 1 | base 2 | fix 2 |
|---|---|---|---|---|
| textures dropped | 776 | **0** | 749 | **0** |
| textures converted (DXT -> BGRA, main thread) | 417 | **0** | 419 | **0** |
| conversion ms | 152.5 | **0** | 156.6 | **0** |
| worst second of conversion | 14.9 ms | 0 | 16.4 ms | 0 |
| seconds with a sweep | 24 | 0 | 20 | 0 |
| frames over 33 ms | 21 | 17 | 20 | 17 |
| frames over 50 ms | 8 | 8 | 9 | 7 |
| worst frame | 67.5 ms | 60.5 | 67.1 | 61.6 |
| missed display slots | 47 | 40 | 46 | 40 |
| guest ms mean / p95 (per-second median) | 4.57 / 5.67 | 4.58 / 5.66 | 4.57 / 5.67 | 4.59 / 5.78 |
| Metal allocations, peak | 476 MB | 497 MB | 476 MB | 497 MB |
| footprint, peak | 974 MB | 1016 MB | 985 MB | 1010 MB |

1. **Texture idle sweep: confirmed, fixed.** On base, every 10 s sweep
   dropped 35 to 65 textures (the area left 30 s before). Each return to
   an area then converted 15 to 40 of them back in its first second
   (5 to 15 ms of main-thread conversion, frames of 60 to 67 ms) and kept
   converting for a few seconds. That is the "buffer filling and freed".
   With the fix, nothing is dropped and nothing converted after the first
   visit. `xbox_textures.c` now keeps host textures up to a byte budget
   (`debug.texture_budget_mb`: 512 MiB on the TV, 1024 MiB on a Mac with
   BC, 512 MiB on Linux, 256 MiB on GLES). Past the budget it drops the
   least recently drawn, at most 4 a frame and none drawn in the last 300
   frames, and clears only that entry's `recent_textures` slot.
   - **The budget.** A whole a10 traversal (both areas, two players) held
     175 MiB of sampled textures on the Mac's TV path (96 MiB of it
     converted DXT). Metal's allocations rose 21 MB on the TV (476 to
     497 MB) and the footprint about 30 MB, with ~1.2 GB still available.
     So 512 MiB is about three times a level's textures: in practice the
     budget is never reached within a level, and it bounds a very long
     session. Map changes still let the game's texture cache go
     (`lruv_flush`), but host textures of a level left behind wait for
     the budget. Visuals are unchanged: the same texels, kept longer.
2. **lruv_cache_verify (asserts on in the guest): measured, not a
   cause; left as it is.**
   - *Cost.* Counted with temporary counters on the Mac (latelag, TV
     texture path): ~820 verify calls a frame (mostly the cheap
     `verify_blocks` FALSE ones, from `lruv_block_touch`/`get_address`),
     0.019 ms a frame on average, the worst frame 0.062 ms.
   - *Block walks.* Full walks happen only on new blocks and deletes: in
     126 of 597 seconds, at most 6942 blocks visited in a frame.
   - *Over time.* The cost does not grow over the run (minute by minute
     flat after the first two).
   - *On the A12.* Scaled ~4x it is ~0.08 ms a frame, the worst under
     0.3 ms. Not a source of lag.
   - *Why no change.* The asserts stay: they are the decompilation's own
     checks, and `HALO_RELEASE` already exists for builds without them.
3. **Page-hash watch: a real periodic spike, small; fixed.**
   - *The spike.* Stable pages were re-hashed together every 60 frames:
     33 pages a frame on average, but up to 1183 in one frame (0.32 ms on
     an M4).
   - *The fix.* `host_watch_hash.c` now re-hashes each stable page in the
     frame of every 60 its index picks.
   - *Effect on the A12.* The extra guest time of the worst of the 60
     frame phases, against frames of the same tick parity, fell from
     0.21/0.23 ms (base) to 0.07/0.06 ms (fix).
   - *Correctness.* A write nobody announced into a stable page is still
     seen within 60 frames (120 for a page not drawn in its frame).
   - *Not this.* The strong guest autocorrelation at lag 60 that remains
     is the game's 30 Hz tick at 60 fps (lags 59 and 61 are negative): a
     two-frame alternation, not the hash.
4. **Other guest sawtooth effects (object garbage collection and so on):
   not found.**
   - *Drift.* Guest ms per minute stays flat over the 10 minutes (4.53 to
     4.63 ms, every run).
   - *What remains.* A ~50 ms guest frame at each teleport, present in
     base and fix alike. It is the scripted `switch_bsp`: a structure BSP
     change. It is the bench's own move, not something the owner meets
     in quiet play.
   - *The first 45 s.* These hold the shader compiles of each area's first
     visit (a ~1 s, 300 ms guest frame at the first two-view frame, and
     compile stalls of 100 to 450 ms). Those are the first visits'
     compiles, cached afterwards, and are another task's (the bundled
     shader manifest).

# Dynamic resolution (A12)

Branch `tv/dynres` (on tv/integrate `85a49369`). Setting
`display.dynamic_resolution` = `"auto"` (on on the Apple TV, off on a
Mac), `"on"`, `"off"`; `display.dynamic_resolution_floor` (1440 lines, one
player), `_split_floor` (1080 lines, split screen), `_ceiling` (`"display"`:
the drawable's lines; `"render"`: resolution_scaling's), `_filter`
(`"bilinear"`, `"sharp"`). Environment: `HALO_DYNRES=on|off`,
`HALO_DYNRES_FIXED=<fraction>[,toggle:<n>,<other>]` (diagnostic).

## How it works

- **Where.** Each player view's 3D scene (render.c: a new
  `halo_screen_scene_begin` hook before `rasterizer_window_begin`, up to the
  existing `halo_screen_anti_alias` hook, before the HUD) is drawn into a
  *scene target* the size of the back buffer, in its top left part, at the
  frame's fraction: the targets' scale (viewports, scissors, clears,
  visibility counts) is multiplied by it, exactly as resolution_scaling
  scales them. Nothing is allocated when the fraction changes (one texture
  of the back buffer's size, made once). The depth buffer is the screen's
  own (top left part used; cleared by every view's `rasterizer_window_begin`;
  nothing after the scene tests depth or stencil but the debug draws).
- **Reads of the screen inside the scene** (active camouflage's copy, the
  sun glow's) find the scene target (`xgpu_render_target_find`) with their
  linear texture coordinates scaled to its part (`bind_textures`). The
  screen effect (convolution, scope masks) runs after the scene, on the
  upscaled back buffer, at full resolution, so it needs nothing.
- **Upscale.** At the scene's end the view's rectangle of the scene target
  is drawn over the view's rectangle of the back buffer in the HUD's pass
  (`host_metal_scene_upscale`: one full-screen triangle, the back buffer's
  pass with its depth attached, depth and stencil neither tested nor
  written), sampling clamped to the source's pixel centres so no filter
  reads outside the scene's part; alpha kept (the game keeps scratch in
  destination alpha). `"bilinear"` is one sample; `"sharp"` adds a
  contrast-adaptive sharpening from 4 neighbours (AMD CAS, simplified).
  HUD, menus and text are then drawn at the screen's resolution.
  MetalFX: `MTLFXSpatialScaler` is in the tvOS 27 SDK (`supportsDevice:`
  decides at run time). It is not used: support on the A12 (Apple5) is
  not verified here (not called on the TV), and public reports have even
  an A15 answering no; MetalFX's spatial scaler is also an FSR1-class
  filter, which the "sharp" filter approximates at a fraction of the cost.
  If wanted, a one-line `supportsDevice:` log on the TV settles it.
- **Split screen.** Each view is scaled up at its own scene's end, before
  the next view's scene begins, so the scene target is reused per view;
  the views keep their rectangles (the clamp keeps one view's filter out
  of the other's pixels).
- **At fraction 1 nothing of this runs** (the frame is drawn as before).

## The controller (port/macos/host/host_dynres.c)

- **Inputs, never waiting on the GPU:** each frame's GPU time from the
  frame's completed handler (its hoisted passes' first start to its end,
  from when the frame before ended: not its wait in the queue), and when
  the display showed it, from the drawable's presented handler (available
  on tvOS; the old comment saying otherwise was wrong).
- **Why not "aim at 14 ms":** the A12 lowers its GPU clock to fill the
  interval whenever there is room: every scene that holds 60 fps shows
  14-15 ms of GPU time at 1080 and at 2160 alike (Baseline tables above:
  bridge 1080 13.8 ms, b30 1080 14.9 ms). GPU time therefore cannot say how
  much room is left, only when there is none: a frame past 97% of the
  interval was drawn at the fastest clock and is the real cost.
- **Down:** 3 hot frames of the last 8, or a frame shown late whose GPU
  time was hot, or any frame past 1.5 intervals (split screen's 4-5x combat
  spikes): at once, as far as a model says (25% fixed, the rest in
  proportion to the pixels, aimed at 85% of the interval), at least a step.
  Late frames whose GPU time was short (loads, compiles) do not count.
- **Up:** one step after 180 frames in a row clear (p90 ≤ 94% of the
  interval) and the wait since the last step down; a step up is a *try*:
  its first hot frame within 120 frames sends it back and doubles that
  step's wait (8, 16, 32 ... 256 s); a step that held 60 s resets it.
- **Steps** (lines of the target): 2160, 2016, 1872, 1800, 1728, 1620, 1512,
  1440, 1344, 1260, 1152, 1080, 960, 864, 720. **Ceiling:** the drawable's
  lines (a 1080p TV mode draws 1080: more is scaled away), two steps lower
  at thermalState serious, four at critical. **Floors:** 1440 one view,
  1080 split screen; one view and split screen keep their own steps, split
  screen starting at 1620 (b30 two views at 2160 measured ~26 ms of GPU in
  play on Capital OFFICE).
- **Logged:** frame log column `render_scale`; telemetry `render_scale`,
  `render_scale_min` per second; a log line every 600 frames (fraction,
  GPU ms, frames late and the GPU's share, steps, failed tries, thermal).

Model test (`tools/test_dynres.py`, a C harness against an A12 model with
the clock behaviour above; not a device number): a 13.5 ms scene stays at
2160 with 0 late; a 19.9 ms b30-like scene settles at 1620-1872 (mean 1863)
with 9 late frames in 5 min; with 15% heating over 25 min, 22 late frames of
90000 (0.024%); a 2-view b30 (26 ms at 2160) mean 1507, 9 late in 5 min;
combat bursts (2x for 3 s every 20 s) at most 4 late frames in a row.

## Telemetry fix: fps and missed slots from the display (commit d76751e4)

Capital OFFICE reported 114.7 fps and 142 missed slots a minute in a 30 s
menu/load session; an A12 frame log here has 44 s of 120 host presents a
second (a10play warm, the cinematic's start). CAMetalLayer lets a present
replace a drawable that was never shown, so host present-to-present
intervals neither count frames shown nor see the display's stutter. The
telemetry's `fps` and `missed` now come from the presented handler's times
(`missed_from: "display"`), the host's kept as `presents`/`missed_host`.

## The telemetry's "other" view bucket (1 draw, 10-42 ms)

It is not a costly draw. The per-view time is main-thread wall time from
a view's start to the next view's start, the last one running to the
present (`host_telemetry_view`, `host_frame_log_present`), so "other" (the
overlay window drawn after the player views: one draw) includes the
present's `wait_drawable` and `wait_inflight`, which is where the main
thread waits for a GPU that is behind. In the bedroom TV's telemetry (2558
seconds) "other" averages 9.80 ms against 9.65 ms of those waits; it grows
with gpu_p50 because the waits do. It is back-pressure, not work, and it
is not the upscale (which did not exist in those builds; it sits before
the HUD, in the view's own time).

## Correctness (Mac M4, numerical; images not viewed)

Method: `tools/dynres_frames.py`: a saved state, the game's time frozen
(`game_speed 0` over telnet), the back buffer written every 30 frames,
`HALO_DYNRES_FIXED` alternating two fractions every 30 frames in one run
(cross-launch frames differ by animation game_speed does not stop).

- **Scale 1.0 is byte-identical.** The scene-target path forced at 1:1
  (`HALO_DYNRES_FIXED=1.0001`: redirect, upscale of a 1:1 rectangle)
  against the direct path, alternating in one run, 3840x2160: bridge 32/32,
  b30 31/31, cryo 30/30 frame pairs byte-identical. (And with dynamic
  resolution off or at fraction 1, the code path is the old one.)
- **Reduced (0.75 = 1620 lines) vs whole, same frozen moment:** bridge
  PSNR 55.6 dB (mean abs 0.07; its 3D view is letterboxed rows 270-1889 by
  the cinematic bars, which stay black), b30 32.0 dB (mean 2.8: foliage
  and the firefight's particles), cryo 38.9 dB (mean 1.0). **Edges:** the
  error in the outermost 8 rows/columns of the view equals the error 40
  pixels in (b30 left 6.0 vs 5.5, right 1.8 vs 1.9, top 0.3 vs 0.4, bottom
  1.0-1.5 vs 1.0-1.8; cryo 1.1 vs 1.1; bridge 0.12 vs 0.16), and the
  near-black share of the edge rows matches the whole frame's (b30 0.002
  vs 0.002; cryo 0.307 vs 0.347 at the top: the room's dark ceiling): no
  black or garbage edge from sampling outside the scene's part.
- **Split screen** (b30split, two views, 0.75 vs 1.0 alternating every 30
  frames, Mac): the game could not be frozen here (no two same-fraction
  frames were identical: the second player's view keeps animating), so
  only structure is checked, not pixels. The divider between the views
  (rows 1075-1083 at 2160) is pure black in both fractions; the error of
  the rows next to it (top view's last rows 9-11, bottom view's first
  3.5-5.5) is the same as the error across a mixed pair's interiors and
  as same-fraction pairs 60 frames apart (8-11): no view's filter reads
  into the divider or into the other view (that would bleed the divider's
  black or the other view's pixels across it). Whole frame mean abs 5.4,
  PSNR 27-28 dB, the motion included.

## Mac sanity (M4 at 7680x4320, GPU-bound there; not a TV number)

b30, `HALO_DYNAMIC_RESOLUTION_CEILING=render`: off 57.5 fps, 869 frames
over 17.5 ms in the window, GPU p50 41.6 ms; on 92.5 fps (the Mac's
120 Hz), 58 over 17.5 ms, scale 0.622 (2688 of 4320 lines). The controller
works end to end on Metal.

## Device: split screen A/B (A12, 2026-10-23, bedroom TV)

**Output mode during this session: 1080p** (the session's line:
`screen 1920x1080 points, native 1920x1080 at 1.0x, mode 1920x1080`;
drawable 1920x1080). The TV's own config renders 2160 lines and scales
them down. With the default ceiling (`"display"`) "on" therefore draws the
3D view at 1080 lines (the display's), the HUD at 2160. The 4K output's
case is emulated by `HALO_DYNAMIC_RESOLUTION_CEILING=render` (ceiling 2160
lines). Build `2425727f` from `<scratch>/tv-deploy-dynres`, installed once;
off/on are `HALO_DYNRES` switches on that binary; warm (one shader salt
for the session, after one warm-up run of each scene); telemetry off;
config.toml/init.txt restored and verified identical after every run
(10/10). Raw: `<scratch>/tvbench-out/device-dynres`. Window: from the
first frame of sustained two views; a10 `split` 50 s (the cryo bay,
player 2 standing), `b30split` 60 s (the beach firefight, both players
standing at the insertion). "missed" = the display's own count (presented
handler: refresh slots with no new frame), "lines" = the 3D view's lines.

| run | scene | frames | fps | wall p95 | p99 | >16.7 | >33.3 | >50 | missed | GPU p50 | GPU p95 | lines mean | lines min |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| off | a10 split | 2998 | 60.0 | 16.8 | 16.9 | 1155 | 0 | 0 | 0 | 15.2 | 15.7 | 2160 | 2160 |
| on | a10 split | 2998 | 60.0 | 16.8 | 16.9 | 1124 | 0 | 0 | 0 | 15.1 | 17.1 | 1080 | 1080 |
| on, ceiling render | a10 split | 2998 | 60.0 | 16.8 | 16.9 | 1118 | 0 | 0 | 0 | 15.1 | 16.7 | 1080 | 1080 |
| off | b30 split | 3318 | 55.3 | 33.4 | 33.6 | 1258 | 256 | 0 | **279** | 26.0 | 33.2 | 2160 | 2160 |
| on | b30 split | 3597 | 60.0 | 16.8 | 16.9 | 1301 | 0 | 0 | **0** | 14.7 | 15.6 | 1080 | 1080 |
| on (repeat) | b30 split | 3597 | 60.0 | 16.9 | 16.9 | 1251 | 0 | 0 | **0** | 14.7 | 15.6 | 1080 | 1080 |
| off (repeat) | b30 split | 3110 | 51.9 | 33.6 | 33.6 | 1240 | 459 | 0 | **486** | 27.9 | 36.4 | 2160 | 2160 |
| on, ceiling render | b30 split | 3597 | 60.0 | 16.9 | 17.0 | 1278 | 0 | 0 | **0** | 14.7 | 15.6 | 1080 | 1080 |

(">16.7" counts frames of 16.70-16.9 ms, the display's interval measured
by the host's clock: not misses. "missed" is the measure.)

- **b30 split screen: 279 and 486 missed slots in 60 s at 2160 → 0 and 0
  with dynamic resolution**, 60.0 fps, p95 16.8 ms, worst frame 21-22 ms.
  This is the owner's 30 Hz case (Capital OFFICE: gpu_p50 26-29 ms at
  2160, the same as here: 26.0 and 27.9).
- **a10 split screen holds 60 at 2160 already** (GPU p50 15.2 ms), no miss
  either way; "on" still dropped to 1080, which costs sharpness for
  nothing there. That is the controller's measuring error (below), fixed
  in the next commit and to be measured again.
- **Thermal:** nominal throughout (the session's ~35 min of runs).

**What the runs showed about the controller.** With the ceiling at 2160
lines the b30 split run spent 8628 frames at 1080 lines, 2123 at 1440, 3 at
1620 and 10 at 2160: it reached the split floor and stayed there,
the same as on the a10 scene that does not need it. The log's "GPU 16.7 ms"
at 1080 lines shows why: the controller's per-frame GPU time was the span
from the frame's first hoisted buffer to its own buffer's end, and the
hoisted buffers are committed early in the frame while the frame's own
buffer is committed at the present, paced by the display. The span holds
the GPU's idle wait between them, so it is about one interval whatever
the load, and it reads "hot". Fixed: each buffer's own time from when the
GPU was done with what came before it, summed (commit after this
section). Everything else (the very-hot rule, the probe and its back-off,
the ceiling and floors) worked as designed: 2 steps down, 1 step up, 0
failed tries in each run.

## Device: session 2, the GPU-time fix (A12, 2026-10-23, bedroom TV, 1080p output)

Build `4ee34304` (rebased on tv/integrate `78125c36`: 24 MiB rings, texture
budget, half-res convolution opt-in), ABBA order (A off at 2160, B on),
B runs with `HALO_DYNAMIC_RESOLUTION_CEILING=render` (2160-line ceiling:
the 4K output's load) for b30split-B2, split-B1 and every single-view B (b30split-B1: the display's ceiling, 1080); 20 runs, config.toml/init.txt
restored and verified identical after each (40/40 files). Raw:
`<scratch>/tvbench-out/device-dynres2`.

| run | scene | frames | fps | wall p95 | p99 | >16.7 | >33.3 | >50 | missed | GPU p50 | GPU p95 | lines mean | lines min | valid |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| b30split-A1 | b30split | 3472 | 57.9 | 16.913 | 33.529 | 1248 | 121 | 0 | 124 | 22.697 | 31.138 | 2160 | 2160 | yes |
| b30split-B1 | b30split | 3597 | 60.0 | 16.822 | 16.914 | 1299 | 0 | 0 | 0 | 14.725 | 15.579 | 1080 | 1080 | yes |
| b30split-B2 | b30split | 3597 | 60.0 | 16.849 | 16.951 | 1277 | 0 | 0 | 0 | 14.717 | 15.631 | 1080 | 1080 | yes |
| b30split-A2 | b30split | 3301 | 55.0 | 33.446 | 33.569 | 1244 | 281 | 0 | 295 | 25.636 | 33.473 | 2160 | 2160 | yes |
| split-B1 | split | 2998 | 60.0 | 16.849 | 16.933 | 1165 | 0 | 0 | 0 | 15.094 | 15.662 | 1620 | 1620 | yes |
| bridge-A1 | bridge | 1921 | 54.9 | 33.507 | 33.663 | 793 | 171 | 0 | 177 | 19.455 | 36.368 | 2160 | 2160 | yes |
| bridge-B1 | bridge | 2096 | 59.9 | 16.938 | 17.125 | 860 | 1 | 0 | 0 | 15.059 | 16.09 | 1441 | 1440 | yes |
| bridge-B2 | bridge | 2096 | 59.9 | 16.919 | 17.104 | 888 | 1 | 0 | 1 | 15.021 | 16.23 | 1441 | 1440 | yes |
| bridge-A2 | bridge | 1920 | 54.9 | 33.479 | 33.719 | 876 | 164 | 0 | 176 | 20.067 | 36.848 | 2160 | 2160 | yes |
| b30-A1 | b30 | 3144 | 52.4 | 33.52 | 33.609 | 1390 | 375 | 1 | 452 | 27.429 | 36.002 | 2160 | 2160 | yes |
| b30-B1 | b30 | 3595 | 59.9 | 16.86 | 16.968 | 1419 | 3 | 1 | 1 | 14.853 | 15.638 | 1440 | 1440 | yes |
| b30-B2 | b30 | 3596 | 59.9 | 16.848 | 16.95 | 1411 | 3 | 1 | 0 | 14.833 | 15.742 | 1440 | 1440 | yes |
| b30-A2 | b30 | 3169 | 52.8 | 33.504 | 33.604 | 1400 | 373 | 1 | 427 | 27.141 | 35.797 | 2160 | 2160 | yes |
| cryo-A1 | cryo | 2828 | 56.6 | 17.193 | 50.036 | 1227 | 113 | 32 | 174 | 15.1 | 38.487 | 2160 | 2160 | yes |
| cryo-B1 | cryo | 2877 | 57.6 | 17.056 | 33.598 | 1193 | 104 | 1 | 123 | 14.718 | 37.138 | 1478 | 1440 | yes |
| cryo-B2 | cryo | 2884 | 57.7 | 17.031 | 33.603 | 1216 | 98 | 1 | 117 | 14.707 | 37.39 | 1478 | 1440 | yes |
| cryo-A2 | cryo | 2832 | 56.6 | 17.186 | 50.002 | 1263 | 109 | 29 | 168 | 15.088 | 35.734 | 2160 | 2160 | yes |

- **Every scene's missed slots fall to (nearly) none, b30 split to 0:**
  b30 split 124/295 → 0/0; bridge 177/176 → 0/1; b30 452/427 → 1/0. The
  cryo bay 174/168 → 123/117: all of them in its first 10 s (the
  tutorial's 6-pass screen convolution, which runs after the upscale at
  the screen's full resolution, so dynamic resolution cannot cut it; the
  half-resolution convolution, opt-in on tv/integrate, is the tool for
  it); its other 60 s have 0 late frames either way.
- **But it settled at its floors** (b30 split 1080, single view 1440,
  where 1440-1620 split / 1620-1872 single would hold) and a10 split
  bounced 1440-2160. Cause: the A12's clock regulation leaves 3-7% of
  frames past 97% of the interval at steps it holds with no late frame
  (b30 split 1080: 3.3% hot, 0 late; a10 split 1440: 6.5%, 0.4% late;
  bridge 1080: 7.3%), and "3 hot of the last 8" fired on that noise. At
  steps it cannot hold frames are 17-100% hot and 2-13% late. Fixed in
  `43a9726d` (down on a GPU-late frame, a frame past 1.1 intervals, 16 hot
  of 64, or any past 1.5 intervals; up after a run with none late or over
  and at most 10% hot); session 3 measures it.

## Device: sessions 3 and 4, the controller's measuring (A12, bedroom TV, 1080p output)

Same harness as session 2; B at the 2160-line ceiling (the 4K output's
load) unless marked. Raw: `<scratch>/tvbench-out/device-dynres3`, `-4`.

Session 3 (`1508b94d`: "16 hot of 64" instead of "3 hot of 8"):

| run | scene | frames | fps | wall p95 | p99 | >16.7 | >33.3 | >50 | missed | GPU p50 | GPU p95 | lines mean | lines min | valid |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| b30split-A1 | b30split | 3346 | 55.8 | 33.416 | 33.559 | 1261 | 236 | 0 | 250 | 25.301 | 32.59 | 2160 | 2160 | yes |
| b30split-B1 | b30split | 3597 | 60.0 | 16.836 | 16.928 | 1260 | 0 | 0 | 0 | 14.719 | 15.695 | 1143 | 1080 | yes |
| b30split-B2 | b30split | 3597 | 60.0 | 16.818 | 16.91 | 1242 | 0 | 0 | 0 | 14.756 | 15.617 | 1143 | 1080 | yes |
| b30split-A2 | b30split | 3218 | 53.7 | 33.488 | 33.591 | 1239 | 348 | 0 | 379 | 26.824 | 34.405 | 2160 | 2160 | yes |
| split-B1 | split | 2998 | 60.0 | 16.813 | 16.898 | 1142 | 0 | 0 | 0 | 15.106 | 15.668 | 1711 | 1620 | yes |
| bridge-B1 | bridge | 2096 | 59.9 | 16.929 | 17.1 | 902 | 1 | 0 | 0 | 15.024 | 16.223 | 1441 | 1440 | yes |
| b30-B1 | b30 | 3596 | 59.9 | 16.847 | 16.966 | 1376 | 3 | 1 | 0 | 14.847 | 15.647 | 1440 | 1440 | yes |
| cryo-B1 | cryo | 2889 | 57.8 | 17.027 | 33.624 | 1205 | 104 | 1 | 113 | 14.799 | 36.533 | 1479 | 1440 | yes |
| b30split-D (display ceiling) | b30split | 3597 | 60.0 | 16.82 | 16.911 | 1309 | 0 | 0 | 0 | 14.725 | 15.615 | 1080 | 1080 | yes |

It stopped after its first moves: b30 split held 1152 lines for the whole
run, a10 split 1728, single view 1440. The log showed why: "485 frames
counted" from frame 3000 to the end. The guest hands each frame's fraction
back as a float and the controller matched it against its double steps
within 1e-9, so frames at any step but 1.0 and 0.5 were never counted
again. Fixed in `c8228cf6` (1e-5; the model test now hands floats back
and requires the controller to keep moving).

Session 4 (`40380da8`, the fix):

| run | scene | frames | fps | wall p95 | p99 | >16.7 | >33.3 | >50 | missed | GPU p50 | GPU p95 | lines mean | lines min | valid |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| b30split-B1 | b30split | 3596 | 60.0 | 16.849 | 16.946 | 1333 | 0 | 0 | 0 | 14.869 | 15.854 | 1528 | 1080 | yes |
| b30split-B2 | b30split | 3597 | 60.0 | 16.833 | 16.922 | 1291 | 0 | 0 | 0 | 14.74 | 15.777 | 1338 | 1080 | yes |
| b30split-A | b30split | 3223 | 53.7 | 33.517 | 33.605 | 1265 | 344 | 0 | 374 | 26.752 | 34.191 | 2160 | 2160 | yes |
| split-B1 | split | 2998 | 60.0 | 16.801 | 16.879 | 1108 | 0 | 0 | 0 | 15.072 | 17.029 | 1101 | 1080 | yes |
| bridge-B1 | bridge | 2096 | 59.9 | 16.93 | 17.115 | 865 | 0 | 0 | 0 | 15.036 | 16.163 | 1524 | 1440 | yes |
| b30-B1 | b30 | 3595 | 59.9 | 16.871 | 16.983 | 1430 | 2 | 1 | 0 | 14.877 | 15.89 | 1579 | 1440 | yes |
| cryo-B1 | cryo | 2885 | 57.7 | 17.015 | 33.624 | 1217 | 99 | 1 | 115 | 14.85 | 36.684 | 1586 | 1440 | yes |

- Now it moves and the misses stay at 0: b30 split mean 1528 / 1338
  lines (1080-1800) with 0 missed against 374 off; bridge 1524, b30 1579
  (0 missed each, against 176-452 off in session 2).
- a10 split mean only 1101: one frame past the interval as the views
  changed (the cinematic's cut to two views, 233 s) sent it from 1620 to
  1080 with the model's jump, and single over-frames failed its tries up.
  Fixed in `83ead8a9`: a lone frame over steps down one step; the model's
  jump and a failed try need two over of eight, a GPU-late frame or one
  past 1.5 intervals.
- cryo's 115 missed are its first 10 s (the screen convolution, above).

## Device: session 5 (`22d7ea0c`), upscale cost, 30-minute thermal (A12, bedroom TV, 1080p output)

B at the 2160-line ceiling. Raw: `<scratch>/tvbench-out/device-dynres5`.

| run | scene | frames | fps | wall p95 | p99 | >16.7 | >33.3 | >50 | missed | GPU p50 | GPU p95 | lines mean | lines min | valid |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| b30split-B1 | b30split | 3596 | 59.9 | 16.843 | 16.946 | 1296 | 0 | 0 | 0 | 14.88 | 15.824 | 1506 | 1080 | yes |
| split-B1 | split | 2998 | 60.0 | 16.824 | 16.911 | 1153 | 0 | 0 | 0 | 15.004 | 17.084 | 1113 | 1080 | yes |
| bridge-B1 | bridge | 2097 | 59.9 | 16.923 | 17.062 | 869 | 1 | 0 | 0 | 14.98 | 16.473 | 1488 | 1440 | yes |
| b30-B1 | b30 | 3596 | 59.9 | 16.847 | 16.967 | 1398 | 2 | 1 | 0 | 14.863 | 15.883 | 1564 | 1440 | yes |
| cryo-B1 | cryo | 2885 | 57.7 | 17.029 | 33.634 | 1193 | 100 | 1 | 116 | 14.742 | 35.756 | 1492 | 1440 | yes |
| up-with | b30 | 3596 | 59.9 | 16.822 | 16.941 | 1401 | 2 | 1 | 1 | 14.867 | 15.671 | 1620 | 1620 | yes |
| up-without | b30 | 3596 | 59.9 | 16.861 | 16.958 | 1420 | 2 | 1 | 0 | 14.875 | 15.689 | 1620 | 1620 | yes |
| up-sharp | b30 | 3596 | 59.9 | 16.858 | 16.975 | 1388 | 1 | 1 | 0 | 14.858 | 15.91 | 1620 | 1620 | yes |

**The upscale's cost** (b30, fixed 1620 lines, 3840x2160 back buffer):
`up-with` (bilinear) vs `up-without` (`HALO_DYNRES_NO_UPSCALE`, its draw
left out) vs `up-sharp`: GPU p50 14.87 / 14.88 / 14.86 ms, p95 15.67 /
15.69 / 15.91 ms, missed 1 / 0 / 0. The A12 holds 60 at 1620 either way,
so its clock lowers to fill the interval and hides the difference in the
means: the cost is below what this can resolve (under ~0.2 ms at p95 for
bilinear; sharp's p95 is 0.24 ms higher). One full-screen triangle with
one (bilinear) or five (sharp) samples a pixel at 3840x2160 is that order.

**a10 split** still drifted to 1080-1152 (mean 1113): pairs of 18-28 ms
frames at the cinematic's cuts and in the bay stepped it down, though none
of the run's 29 such pairs was shown late. Fixed in `3516fc2d` (down on
display-late, four over of eight, 25% hot, or one past 2.5 intervals).

**Thermal: 30 minutes of b30 two-player split screen**, on (2160-line
ceiling) then off, back to back (the frame log's ring kept each run's
last 10 and 14.6 minutes; thermalState stayed nominal in both):

| run | minutes | frames | missed | missed/min | lines mean (min) | GPU p50 | wall p99 |
|---|---|---|---|---|---|---|---|
| on | 21-23 | 7193 | 0 | 0 | 1513 (1260) | 14.9 | 17.0 |
| on | 23-25 | 7193 | 0 | 0 | 1487 (1080) | 14.9 | 17.0 |
| on | 25-27 | 7193 | 0 | 0 | 1524 (1344) | 14.9 | 17.0 |
| on | 27-29 | 7194 | 0 | 0 | 1431 (1080) | 14.9 | 17.0 |
| on | 29-31 | 5397 | 0 | 0 | 1524 (1260) | 14.9 | 16.9 |
| off | 16-18 | 5979 | 1213 | 606 | 2160 (2160) | 27.9 | 35.2 |
| off | 18-20 | 5883 | 1310 | 655 | 2160 (2160) | 30.0 | 33.6 |
| off | 20-22 | 4431 | 2761 | 1380 | 2160 (2160) | 27.1 | 38.2 |
| off | 22-24 | 3429 | 3763 | 1882 | 2160 (2160) | 25.8 | 38.6 |
| off | 24-26 | 3429 | 3761 | 1880 | 2160 (2160) | 25.8 | 38.7 |
| off | 26-28 | 5293 | 1900 | 950 | 2160 (2160) | 26.6 | 37.7 |
| off | 28-30 | 5919 | 1274 | 637 | 2160 (2160) | 29.8 | 33.6 |

- **On: 0 missed slots in every 2-minute block of minutes 21-31**, wall
  p99 17.0 ms, the 3D view at 1430-1524 lines on average (1080-1344 at
  the worst moments). The controller log over all 30 min: 108,603 frames
  counted, 3 late. (It moved a step about every 12 s: the a10-split drift
  above, fixed in `3516fc2d`, which session 6 measures.)
- **Off: 600-1900 missed slots a minute** in minutes 16-30, worst at
  20-26 min (1380-1880 a minute: wall p99 38.7 ms, fewer than 3500
  frames in 2 minutes, i.e. ~28 fps), GPU p50 26-30 ms. This is the
  owner's late-session b30 split-screen lag.

## Device: session 6, the controller as it ships (`781db209`, A12, bedroom TV, 1080p output)

The build installed on the TV at the end. B at the 2160-line ceiling (the
4K output's load); A off at 2160. Raw: `<scratch>/tvbench-out/device-dynres6`;
config.toml/init.txt restored and verified identical after each run (16/16).

| run | scene | frames | fps | wall p95 | p99 | >16.7 | >33.3 | >50 | missed | GPU p50 | GPU p95 | lines mean | lines min | valid |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| b30split-A1 | b30split | 3278 | 54.6 | 33.454 | 33.577 | 1282 | 305 | 0 | 318 | 26.089 | 33.408 | 2160 | 2160 | yes |
| b30split-B1 | b30split | 3597 | 60.0 | 16.851 | 16.953 | 1283 | 0 | 0 | 0 | 14.884 | 15.911 | 1627 | 1512 | yes |
| split-B1 | split | 2998 | 60.0 | 16.823 | 16.913 | 1159 | 0 | 0 | 0 | 15.129 | 15.691 | 1972 | 1620 | yes |
| b30split-B2 | b30split | 3597 | 60.0 | 16.848 | 16.966 | 1324 | 0 | 0 | 0 | 14.931 | 15.913 | 1631 | 1512 | yes |
| split-A1 | split | 2998 | 60.0 | 16.829 | 16.914 | 1182 | 0 | 0 | 0 | 15.105 | 15.704 | 2160 | 2160 | yes |
| bridge-B1 | bridge | 2095 | 59.9 | 16.911 | 17.066 | 878 | 2 | 0 | 1 | 15.186 | 18.966 | 1838 | 1728 | yes |
| b30-B1 | b30 | 3595 | 59.9 | 16.872 | 16.968 | 1422 | 3 | 1 | 1 | 14.93 | 16.724 | 1760 | 1620 | yes |
| cryo-B1 | cryo | 2876 | 57.5 | 17.08 | 33.605 | 1248 | 95 | 1 | 124 | 15.063 | 37.511 | 1917 | 1440 | yes |

Off in single view, for comparison (session 2, same TV, same scenes): bridge
177/176 missed, b30 452/427, cryo 174/168.

- **b30 two-player split: 318 missed in 60 s off → 0 and 0 on**, the 3D view
  at 1627/1631 lines on average, never below 1512 (sessions 1-5 on this
  scene: 124-486 off, 0 on in every run).
- **a10 split: 0 missed either way, and now at 1972 lines on average** (it
  holds 60 at 2160 most of the time; the controller steps down only for
  the heavy moments, never below 1620).
- **Single view: bridge 1 missed (1838 lines mean), b30 1 (1760), against
  176-452 off.** cryo's 124 are all in its first 12 s (the tutorial's
  screen convolution, after the upscale, at full resolution; 0 after), as
  off (168-174): dynamic resolution does not touch it. That needs the
  half-resolution convolution (tv/halfres, opt-in), not this.
- The script ended before its 30-minute thermal run on this build (a
  script error after cryo); the 30-minute A/B is session 5's, on the
  controller before `3516fc2d` (0 missed for 10 minutes of the last 10 on,
  600-1900 a minute off).

## Conclusions (dynamic resolution)

- **Default: on for the Apple TV** (`display.dynamic_resolution = "auto"`;
  off on a Mac). It removes the missed display slots in every gameplay
  scene measured, b30 split screen's included (the owner's 30 Hz case),
  keeps single view at 1440 or more (1620-1840 on average), and costs
  nothing measurable where the A12 already holds 60 (the upscale's draw is
  below what the device's own timing resolves). Scale 1.0 is byte-identical
  to the old path.
- **Ceiling: the display's lines.** This TV's output was 1080p in every
  session here: the default draws the 3D view at 1080 lines on it, the
  same as a 2160-line render scaled down by the display would show, at a
  quarter of the work. A 4K output (Capital OFFICE) gets up to 2160.
  `display.dynamic_resolution_ceiling = "render"` keeps the old behaviour
  (render resolution_scaling's lines, scaled down).
- **Floors:** 1440 lines one view, 1080 split screen. The split floor was
  reached only in the controller's earlier, wrong versions; the shipping
  one stays at 1512 or above on b30 split.
- **Upscale:** bilinear by default; `"sharp"` (CAS-lite) is available and
  costs ~0.2 ms at p95. MetalFX is not used (A12 support unverified).
- **What it does not fix:** CPU-side hitches (loads, compiles) and the
  full-resolution screen convolution after the scene (cryo's first 10 s).
- **Still to measure:** the 30-minute thermal run on the shipping build;
  the real 4K output (Capital OFFICE), where the ceiling is 2160 and the
  numbers above with `ceiling render` are the closest stand-in.

# Lifecycle (A12)

Branch `tv/lifecycle`. Measured 2026-10-23 on the bedroom TV (Apple TV 4K
2nd gen, A12, tvOS 27.0, CoreDevice `443953B8-…`) and the tvOS 27.0
simulator on the M4. Code: `port/macos/host/host_lifecycle.{h,m}`; test
tool: `tools/tvos_lifecycle.py`; source checks: `tools/test_tv_lifecycle.py`.

## The problem

The game kept rendering when the app stopped being active (the Home button,
Control Center, the screensaver, the TV's sleep, a launch behind another
app). tvOS refuses that work. On the base build (tv/integrate `a2e2bd44`,
the bench's bridge state, three trips to TVSettings and back, 20 s away
each), the run logged **1182** `Insufficient Permission (to submit GPU work
from background)` failures (178 frames' command buffers). Each return was
one **18.6 s frame**: the game had gone on drawing into a layer with no
drawables, and the return's first `nextDrawable` waited for the stretch
away (18.7 s drawable wait in one of the three).

## What happens now

- **Inactive** (SDL's `WILL_ENTER_BACKGROUND`, UIKit's willResignActive; an
  event watch, not the queue). Every command-buffer commit in `host_metal.m`
  (the frame's, hoisted passes, texture uploads, readbacks, pass-timing
  buffers) and the present, before it takes a drawable, call
  `host_lifecycle_gate()`. While active that is one relaxed atomic load. While
  not, on the main thread, the game stops there:
  1. the audio devices are paused (`SDL_PauseAudioStreamDevice`; the ones
     this paused are resumed later);
  2. the frames in flight and the last upload finish (`host_metal_drain`:
     every frame slot taken and given back, hoisted buffers waited for):
     0-25 ms on the A12;
  3. what is cheap to remake is released: the free frame rings, the park's
     pooled renamed targets, and the feedback copy (**130 MB** on the A12,
     128 MB in the simulator);
  4. UIKit's run loop runs (the game's thread is UIKit's main thread) until
     `DID_ENTER_FOREGROUND` (didBecomeActive), or until the app's state is
     seen to be active.
- **Launched while not active** (behind another app, or with the TV
  asleep). `host_metal_attach` reads `applicationState`. Every launch by
  devicectl starts "inactive (not yet on the screen)", and the first frame
  waits for activation (0.8-0.9 s on the A12). That wait is not counted as
  a pause.
- **Foreground.** The game goes on in the same frame. The drawable size is
  read again every present, as before. The guest polls
  `host_lifecycle_stops()` once a frame. After a stop:
  - a game in progress opens its **pause menu**, as if player 1 had pressed
    Start (`ui_widget.c`, `ui_check_for_pause_game`; not in cinematics or
    menus, or when a menu is already up);
  - that frame's seconds are clamped to one frame's (1/60 s, `main.c`, both
    timing paths), so there is no game-time spike;
  - the frame log's and the telemetry's wall times leave the time away out.
- **Terminating** flushes the telemetry. **The Mac** gets none of these events
  from SDL, so it never stops; its defaults are unchanged.

## Device results (A12, 4K picture, TV in 1080p output)

`tools/tvos_lifecycle.py device --cycles 3 --play 25-30 --away 20`
(TVSettings in front, as Home does, then OpenCE again), bridge state, no
input.

| run | build | background GPU errors | GPU-failed frames | longest frame | pause menu on return | first frames after the return (ms) |
|---|---|---|---|---|---|---|
| before | `a2e2bd44` (tv/integrate) | **1182** | 178 | **18650 ms** (x3) | no | (the 18.6 s frame) |
| after, 1 | `186123f8` | **0** | 0 | 47.8 ms | yes (3/3) | 7.9, 12.7, 41.1, 16.7, 16.8, 16.5 |
| after, 2 (+ memory warning) | `c5a5febd` | **0** | 0 | 115 ms | yes (3/3) | 5.6, 11.1, 47.7, 16.7, 16.8, 16.5 |

Every stop took 21.5 s for the 20 s away (the devicectl launches included).
The third frame after a return, about 45 ms, is the pause menu's first
frame: it loads the widget. Later frames are at 16.7 ms. The `init.txt` was
restored and verified identical every time.

## GPU waits in play (the frame log's `wait_*`, mean ms a frame)

Every blocking GPU wait in the host, and when it fires:

| wait | where | when | in normal play |
|---|---|---|---|
| `dispatch_semaphore_wait(frames_free)` | `frame_begin` | the frame `FRAMES_IN_FLIGHT` (3) before is still on the GPU | pacing only: 0.000 mean, max 0.04 ms |
| `nextDrawable` | `host_metal_present` | the display's pacing (vsync) | 9-15 ms: this is the vsync wait, not GPU stalls |
| `[submitted waitUntilCompleted]` | `host_metal_texture_read` | only `debug.screenshot_every` (the guest's one caller, `write_screenshot`) | never: 0.000 |
| `[early waitUntilCompleted]` | the frame's completed handler | on Metal's completion thread, not the game's | never on the game's thread |
| semaphore and `waitUntilCompleted` | `host_metal_drain` | only when the app resigns active | never in play |

No `waitUntilScheduled` exists. Nothing was moved off the frame path: the
audit found no blocking wait on it besides the 3-frame throttle.
`tools/test_tv_lifecycle.py` keeps it so (no new wait site, every commit
gated).

| run (A12, all frames of play, not the 18 s ones) | frames | drawable | in flight (max) | readback |
|---|---|---|---|---|
| telemetry, 8 owner sessions (`tv-telemetry/443953B8`, 2026-10-23) | 147k | 9.5-10.6 | 0.000-0.041 | 0.000 |
| before (`a2e2bd44`) | 10589 | 11.07 | 0.061 (18.6 at a return) | 0.000 |
| after, 1 | 9328 | 12.28 | 0.000 (0.04) | 0.000 |
| after, 2 | 10518 | 12.26 | 0.000 (0.04) | 0.000 |

The "after" runs were on the `20d1333f` base, which has no dynamic
resolution. In the bench's heavy stretches the 4K picture costs 20-23 ms
of GPU a frame, which raises the drawable wait and makes >33 ms frames.
Those stretches appear before the first stop as well as after the returns:
they come from the scene and the resolution, not from the lifecycle.
Within the runs, before vs after each stop, the in-flight wait stays at 0
and the readback at 0.

## Memory warnings

- Watched: UIKit's warning (`SDL_EVENT_LOW_MEMORY`) and the kernel's
  pressure (a `DISPATCH_SOURCE_TYPE_MEMORYPRESSURE` source, warn and
  critical). Each is logged with the footprint and
  `os_proc_available_memory`. At the next frame it is also a frame-log and
  telemetry **event** of kind `memory`, with detail `warning footprint N MB
  avail M MB`, and `trimmed X MB footprint N MB` when the trim ends.
  `tv_telemetry.py report` lists these and the lifecycle events, with each
  second's footprint, under "Lifecycle and memory events".
- The trim is an emergency trim, one step a frame, never a burst on the
  main thread. It drops what has been unused for ~60 frames: the park's
  pooled targets, the feedback copy, and the shader cache's prepared
  shaders that no draw has taken. Steady-state budgets (the texture cache's
  byte budget, the frame rings) are #110's and are not changed here.
- On the A12: devicectl's `sendMemoryWarning` fails on this TV (`ENOENT`),
  so `HALO_MEMORY_WARNING_AFTER=<s>` (diagnostic) sends one in its place.
  Run 2 logged `memory warning footprint 896 MB avail 1202 MB` at 75.9 s.
  The trim took 3 frames and dropped 17 prepared shaders (0.3 MB). There
  was no frame over 33 ms in those frames.
- Not yet known: whether tvOS warns before the reclaim freezes seen on
  Capital OFFICE (footprint 1267 -> 1178 MB with no frees by the app). The
  event is now recorded, so the owner's next sessions will show it.

## Simulator (tvOS 27.0 on the M4, 720 lines; not TV numbers)

`tools/tvos_lifecycle.py simulator` (simctl launch of TVSettings, then of
the game). Two cycles: 0 background GPU errors, 0 GPU failures, both stops
16.2 s, the pause menu opened twice, the first frames after 2.9-9 ms, the
in-flight and readback waits 0.

## Not tested

- The real Home button, Control Center and the screensaver. None can be
  pressed from here. The screensaver and the TV's sleep go through the same
  `willResignActive` and `didEnterBackground` as the TVSettings launch, so
  the same gate covers them, but neither was seen.
- A memory warning from the system itself.

# Shading (A12)

Branch `tv/shading` (on tv/integrate `78125c36`). Question: can the
generated pixel shaders (port/linux/src/nv2a_psh.c, the NV2A combiners as
MSL) shade the environment's passes (lightmap, diffuse lights, specular,
reflection masks, reflections, fog) for less, with the same pixels?
**Short answer: no change that keeps byte-identical pixels was found. The
one that helps a little (half precision) is an opt-in,
`display.half_precision_shading` / `HALO_HALF_PRECISION_SHADING`, off by
default; it is within the A12's run-to-run noise in fps.** Default
behaviour and the shaders' text are unchanged (checked: the 80 pixel
shaders of the bridge dumped by the old and new builds are identical
byte for byte, so the shader manifests and #108's bundled packs still
match).

## What the generated shaders look like

Dumped with `HALO_GPU_DUMP_SHADERS` from the bridge, b30 and cryo states
(80 / 136 / 67 pixel shaders). The environment's, by phase:

| phase | texture stages | combiners | alpha test | blend |
|---|---|---|---|---|
| lightmaps | project2d bump, project2d lightmap, cube normalization | 2 (6 with self-illumination) | per shader | none, depth write |
| diffuse lights | project2d bump, cube gel, 3D attenuation, cube normalization | 4 | alpha > 0, discard | one + one |
| textures | 4 x project2d (base + 3 details) | 3 | none | dest color x src |
| specular lights | project2d bump, project3d attenuation, 2 cubes | 6 (8 extra shiny) | alpha > 0, discard | dest alpha x src + dest |
| reflection masks | project2d lightmap | 2 | none | alpha only |
| reflections | bump, dot product, dot-reflect-diffuse, dot-reflect-specular (dependent cube read) | 5 | none | dest alpha x src + dest |
| fog | 2 x project2d | 2 | alpha > 0, discard | one + inv src alpha |

Every combiner is written for all four registers and both portions whether
or not its inputs are zero (`max(vec4(0.0).b, 0.0)` and the like), each
input mapping clamps, and every output is clamped to [-1, 1]. All of that
is constant-folded by Metal's compiler: unused portions, zero inputs and
dead writes do not survive into the GPU code, so rewriting the text to
skip them would not change the work done (not pursued). There is no
branching besides the alpha test, and the only dependent reads are the
NV2A's own (the reflections' dot-reflect cube lookup, bump env maps). The
alpha test of the three additive phases is `if (!(floor(clamp(a) * 255 +
0.5) > alpha_reference)) discard_fragment()`. No shader discards with an
ALWAYS test (`comparison_operator` returns "" and nothing is emitted), and
none has a discard the state does not ask for.

## What was tried (all behind `HALO_METAL_SHADING=<mask>`, a diagnostic)

`HALO_METAL_SHADING_CYCLE=N` alternates the mask with 0 (the old text) in
blocks of N frames in one run, the frozen-frame method of "GPU bandwidth"
(Mac M4, 3840x2160, `game_speed 0` over telnet, the back buffer every 30
frames, N = 60; each pair of consecutive shots compared: "other" pairs
straddle a switch, "same" pairs do not, and every "same" pair was
identical or within 1 level, so the run was frozen).

| mask | change | bridge (other pairs) | b30 | cryo |
|---|---|---|---|---|
| 1 | combiners in half (texture stages, fog, alpha test float) | max 2, 4.24% px changed, none > 2 | max 3, 11.4%, none > 2 | max 3, 12.9%, none > 2 |
| 2 | samples as `texture<half>` | max 2, 2.23% | max 2, 8.1% | max 2, 7.0% |
| 3 | 1 + 2 (**the opt-in**) | max 2, 4.40%, mean 0.015 | max 2, 11.4%, mean 0.042 | max 3, 13.9%, mean 0.051 |
| 4 | failed alpha test returns 0 instead of discarding, where the blend leaves the target unchanged by a 0 | identical | max 7, 0.037%, 0.003% > 2 | max 1, 1 px |
| 8 | no `bias()` argument where the stage's LOD bias is 0 | (not run) | identical | identical |
| 15 | all | max 2, 4.36% | max 6, 11.4%, 0.002% > 2 | max 3, 13.9% |

- **Half precision is not bit-exact.** The NV2A combiners are ~9-bit, but
  the port's float combiners carry more than that, and the 8-bit output's
  rounding moves by a level where a value sits near a half step: about 4%
  of the bridge's pixels and 11-14% of the b30/cryo pixels move by 1-3
  levels (none by more than 3, except mask 4's few pixels below). Opt-in.
- **Alpha test as a color of 0 (mask 4)** is exact on the bridge but not
  on b30 or cryo. Bisected on b30 with `HALO_METAL_SKIP_PHASES`: the
  difference is in phases 3-15 (environment), not in transparents, water,
  sky or the screen effects. By the blend arithmetic a source of 0 should
  leave each of those draws' target exactly as it was; the cause of the
  b30 difference was not found (not bisected further, since the change is
  slower on the A12 anyway, below). Not offered.
- **No `bias()` of 0 (mask 8)**: exact, but no measurable gain (below).
  Kept as a diagnostic only, so that the default text stays the shader
  packs' text.

## A12 results (bedroom TV, 3840x2160, one binary `18e5592e`)

Installed once from `<scratch>/tv-deploy-shading`; one shader salt for
every run, a cold launch of each variant first (not counted), then warm
runs in ABBA order: off, on, on, off (on = `HALO_HALF_PRECISION_SHADING=true`,
which in the binary measured meant mask 15, all four changes; see
"Correction" below). Steady window as tvos_bench. Raw:
`<scratch>/tvbench-out/device-shading/`.

| run | scene | fps | >33.3 | >50 | GPU ms mean | GPU p50 |
|---|---|---|---|---|---|---|
| A1 off | bridge | 54.2 | 156 | 0 | 22.3 | 17.2 |
| B1 on | bridge | 51.7 | 228 | 0 | 24.8 | 22.1 |
| B2 on | bridge | 51.6 | 231 | 0 | 24.7 | 21.3 |
| A2 off | bridge | 52.2 | 226 | 0 | 24.1 | 21.7 |
| A1 off | b30 | 51.2 | 429 | 0 | 28.3 | 28.3 |
| B1 on | b30 | 52.3 | 344 | 0 | 27.8 | 27.7 |
| B2 on | b30 | 52.8 | 358 | 0 | 27.3 | 27.1 |
| A2 off | b30 | 49.0 | 538 | 0 | 30.2 | 30.0 |
| A1 off | cryo | 58.2 | 48 | 17 | 16.2 | 15.1 |
| B1 on | cryo | 57.8 | 52 | 29 | 16.4 | 15.1 |
| B2 on | cryo | 58.1 | 55 | 15 | 16.3 | 15.1 |
| A2 off | cryo | 57.8 | 65 | 22 | 16.5 | 15.1 |
| A1 off | split | 59.9 | 0 | 0 | 15.06 | 15.10 |
| B1 on | split | 59.9 | 0 | 0 | 15.09 | 15.10 |
| B2 on | split | 59.9 | 0 | 0 | 15.08 | 15.11 |
| A2 off | split | 59.9 | 0 | 0 | 15.08 | 15.13 |

Per-phase fragment ms a frame (`HALO_METAL_PASS_SPLIT=phase`, hoisting
off, one run each, consecutive; a phase pass carries ~0.4-0.6 ms of
load/store the real frame does not):

| mask | bridge sum | diffuse lights | reflections | textures | specular | fog planes | refl. masks | lightmaps | shadows | cryo sum | cryo reflections | cryo textures | cryo fog |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 (default) | 26.20 | 3.82 | 3.42 | 2.87 | 2.05 | 1.94 | 1.93 | 1.91 | 1.84 | 20.19 | 4.00 | 3.21 | 2.35 |
| 3 (half) | 25.97 | 4.06 | 3.46 | 2.71 | 1.94 | 1.95 | 1.94 | 1.83 | 1.69 | 19.40 | 3.93 | 3.02 | 2.32 |
| 4 (alpha test as 0) | 29.85 | 4.29 | 3.67 | 3.08 | 2.68 | 2.82 | 1.98 | 2.06 | 2.13 | 21.73 | 4.12 | 3.31 | 3.06 |
| 8 (no bias of 0) | 27.00 | 3.98 | 3.51 | 2.93 | 2.25 | 1.96 | 1.98 | 1.94 | 1.89 | 20.23 | 3.98 | 3.19 | 2.35 |
| 15 (all) | 27.55 | 4.34 | 3.45 | 2.71 | 2.45 | 2.48 | 1.93 | 1.83 | 1.84 | 19.92 | 3.92 | 2.96 | 2.70 |

Reading it:

- **Half precision (mask 3)**: textures -0.16 / -0.19 ms, lightmaps -0.08
  / -0.12, shadows -0.15 / -0.31, specular -0.11; diffuse lights +0.24 on
  the bridge (one run each: within noise). Bridge -0.2 ms (-1%), cryo
  -0.8 ms (-4%) of fragment time. The A12's fragment cost on these passes
  is texture fetch and blending, not combiner ALU: half math saves
  little. Not worth the levels it moves for a default.
- **Alpha test as 0 (mask 4) is slower**: fog planes +0.9 / +0.7 ms,
  specular +0.6, diffuse lights +0.5; bridge +3.7 ms, cryo +1.5 ms. On
  the A12 the discard of the three additive passes is cheaper than
  blending a 0: discarding skips the blend (a read-modify-write of the
  tile) for the many pixels these passes do not light (alpha 0 = outside
  the light's radius / fog's extent). Early-Z/HSR are not lost by the
  discard here anyway: these passes test depth EQUAL and write nothing, so
  there is nothing for HSR to remove behind them. Rejected.
- **No bias of 0 (mask 8)**: +0.8 / +0.04 ms, noise. Rejected.
- **ABBA, the shipped opt-in**: the device runs used the setting as first
  committed (mask 15, all four: see "Correction"). Bridge got worse when
  on (54.2 / 52.2 off vs 51.7 / 51.6 on; >33 ms frames 156 / 226 vs 228 /
  231), b30 a little better (51.2 / 49.0 vs 52.3 / 52.8; 429 / 538 vs 344
  / 358), cryo and split unchanged (split holds 59.9 fps at 15.1 ms either
  way). That matches the phase table: mask 4's slower fog and lights cost
  the bridge more than half precision saves; b30's transparent-heavy frame
  gains a little. The A12 drift across a session (GPU busy for an hour,
  "GPU bandwidth" above) is as large as these differences: A1 -> A2 for
  the same setting moved the bridge 54.2 -> 52.2 and b30 51.2 -> 49.0.

**Correction / what is committed:** after the session, the setting was
narrowed to the half precision flags only (mask 3); masks 4 and 8 stay as
`HALO_METAL_SHADING` diagnostics. Mask 3's own A12 effect is the
phase table's (-1% bridge, -4% cryo fragment time), not measured as an
ABBA fps run (the TV's queue; it would be inside the noise above). Leave
the setting off.

## Early depth / HSR (idea 2)

Checked in the generated text and the draw state: the environment's
EQUAL passes write no depth, no shader writes depth, and no shader
discards without the game's alpha test (none for ALWAYS). The only
discards are the additive passes' alpha > 0 test, which is cheaper than
the alternative (mask 4, above). Nothing to fix.

## Texture sampling (idea 4)

`configure_sampler` (d3d8_metal.c) maps D3D's filters one to one: min /
mag / mip POINT or LINEAR, anisotropy only for `D3DTEXF_ANISOTROPIC` with
`MAXANISOTROPY` > 1 (the game sets neither: no `MAXANISOTROPY` anywhere in
source/rasterizer), `MAXMIPLEVEL` as `lodMinClamp`. The environment's
lightmaps, attenuation and normalization maps are already POINT where the
game asks. The samplers sample no more than the Xbox did; nothing to cut
without changing pixels.

## Pass merging (idea 3): design, not built

The environment draws the same triangles (one dynamic index buffer built by
`structure_render_preprocess`, the same `first_triangle_index` / count per
material chunk) in each phase, all but the lightmap with depth EQUAL and
no depth write, into the same 4K target. Folding consecutive passes into
one fragment shader with framebuffer fetch (`[[color(0)]]` is valid on the
A12) would save each extra pass's rasterization and its tile read-modify-
write. Why it was not built, and what it would take:

1. **Order and state between the passes.** The phases are not adjacent per
   surface: the frame goes lightmaps (all surfaces), object shadows,
   diffuse lights (per light, each light's own cluster subset and
   surface list), decals, textures, decals, specular (per light), specular
   lightmaps, reflection masks, mirrors, reflections, transparents, fog.
   Decals and shadows between them read and write the same pixels, so a
   merge is only legal for runs with nothing between them: in practice
   **reflection masks -> reflections** (consecutive unless a mirror is
   drawn) and **lightmap -> (nothing)**. The per-light passes cover other
   surface sets than the textures pass.
2. **The prototype pair.** reflection masks write alpha only (`dest
   alpha x lightmap term`), reflections then add `dest alpha x
   reflection` to RGB. Merged: one draw per chunk, fragment reads
   `[[color(0)]]`, computes the mask's new alpha, then the reflection with
   that alpha, writes both. Needs: the guest to recognise the pair (same
   vertex buffers, index range, chunk order, no draw between), a combined
   pixel shader key (two combiner programs and their constants: two
   uniform sets), blending off and the blend done in the shader with the
   same 8-bit rounding (the blend unit rounds the stored alpha to 8 bits
   before the reflection reads it: the shader must round it the same way,
   `floor(a * 255 + 0.5) / 255`), and the draw's raster state the
   reflection's. A12 cost of the pair: 1.93-2.15 ms (masks) + 3.4-4.0 ms
   (reflections) a frame; the masks' share of that is mostly a 4K tile
   pass that one draw instead of two would save, upper bound ~1.5-2 ms.
3. **Risk.** The game's draw stream is not built in pairs: the guest would
   have to buffer a phase's draws until the next phase starts (it already
   knows the phase: `host_metal_profile_phase`) and match them chunk by
   chunk, falling back on any mismatch. Correctness checked as above
   (frozen frames, merged vs unmerged in one run). Estimated a few days;
   left for later.

## What remains

The shaders are not where the A12's environment time goes: the passes
cost what their texture fetches and tile blends cost. The levers left are
the ones that remove pixels or passes: dynamic resolution (#105), half
resolution effects (#106), the reflection-mask merge above, and the b30
transparents.

# Half resolution (tv/halfres), continued

## Scope zoom (masked convolution): left at full resolution

The scopes' zoom is a masked warp: inside the mask (the view through the
scope) every pass carries the screen's own picture forward, so at half
resolution that view loses half its sharpness. b30's pistol 2x (radius
10.8, 4 passes, 3840x2160, frozen, `HALO_TEST_INPUT=zoom:1:30`): 42.8 dB
over the frame, **35.1 dB in the centre** (max 73), 27.7 dB at edges.
`display.half_res_convolution` therefore leaves masked convolutions at
full resolution; it covers the unmasked blurs (a10's tutorial) only.

## Queued transparents at half resolution (display.half_res_transparents)

Design: host_metal.m, "the queued transparents at half resolution";
d3d8_metal.c, half_run_*. Runs of affine-blended phase-22 draws accumulate
S and M (RGBA16F, half scale each way); a composite sets dst = S + M*dst.

Survey (`HALO_METAL_TRANSPARENT_SURVEY=1`, Mac 4K): every phase-22 draw on
b30 (39 a frame), cryo (59) and bridge (21) blends affinely with the
read-only stencil test EQUAL 0 mask 1, except 0.9 a frame on bridge (MAX
blends, one depth writer). b30's blends: ONE/ONE 25, SRCALPHA/INVSRCALPHA
6.4, SRCALPHA/ONE 6.0, ZERO/SRCCOLOR 2.0. Excluded (full resolution, in
order): non-add operations, constant or destination-alpha factors, depth or
alpha writes, stencil writes, draws sampling a render target (camouflage),
draws into other targets.

Fidelity (Mac, 3840x2160, frozen; off vs on; "touched" = pixels the
transparents change):

| scene | all PSNR | touched PSNR | touched p99.9 | max | full-res accumulation alone |
|---|---|---|---|---|---|
| b30 | 52.9 | 49.5 | 7 | 123 (isolated edge pixels) | 55.2 dB, max 26 |
| cryo | 59.7 | 50.9 | 4 | 12 | 60.4 dB, max 2 |

A12 (bedroom TV, picture 3840x2160, **drawable 1920x1080**; the TV was in
its 1080p output mode), convolution on in both, ABBA, warm, one binary
(68186890 on tv/integrate 566bb6d4); window = the scene's:

| scene | run | fps | >33.3 | >50 | GPU p50 | GPU p95 |
|---|---|---|---|---|---|---|
| b30 | off | 59.74 | 5 | 3 | 14.71 | 16.79 |
| b30 | on | 59.93 | 2 | 1 | 14.86 | 15.81 |
| b30 | on | 59.93 | 2 | 1 | 14.83 | 15.86 |
| b30 | off | 59.96 | 1 | 1 | 14.72 | 16.65 |
| b30split | off | 59.96 | 0 | 0 | 14.70 | 15.60 |
| b30split | on | 59.91 | 2 | 0 | 14.90 | 15.43 |
| b30split | on | 59.92 | 1 | 0 | 14.90 | 15.45 |
| b30split | off | 59.96 | 0 | 0 | 14.72 | 15.61 |
| split | off | 59.96 | 0 | 0 | 15.11 | 16.89 |
| split | on | 59.94 | 1 | 0 | 15.11 | 15.88 |
| split | on | 59.96 | 0 | 0 | 15.12 | 15.92 |
| split | off | 59.96 | 0 | 0 | 15.09 | 16.86 |

No clear win: these scenes already hold 60 fps. The heavy-effect tail
falls (GPU p95 -0.9 ms on b30 and split) but the median rises 0.15-0.2 ms
(the run's fixed passes). Why, from the b30split pass log with it on: each
view's run is a 1920x1080 pass (1.8-2.0 ms of fragment time for 21 draws)
plus the screen's pass broken and reloaded after it (a 3840x2160 load and
store, ~1.1 ms each view: the "lens_flares" and empty passes). The draws
themselves got cheaper; the pass break ate most of it.

**Default: off.** What would make it pay: compositing into the pass the
game opens next instead of a pass of its own (no extra screen reload), and
skipping a run whose draws cover little of the screen.

## Memory: the footprint, who owns it, and the caps (2026-10-23)

Capital OFFICE's 2.5-hour session (a10 then b30 in split screen, build
8bc487e3) grew the footprint 849 -> 1379 MB and Metal's allocations
400 -> 707 MB. tvOS then reclaimed memory under the game four times, each
with a 50 to 90 MB drop in footprint and ~3 s of 200-320 ms frames (guest
and host both 10-15x slower, the GPU barely). The play telemetry's "mem"
(every second) now says who owns Metal's allocations. `footprint -p` on
the Mac gives the rest.

**Who owned the footprint.** Mac, b30 split, TV texture path, 4K, before
these changes, 1143 MB in all:
- **the guest's memory, 437 MB.** Mostly the game's 256 MiB texture cache,
  plus the tag cache, the sound cache and game state. The window allocator
  cleared every block on wasm, so all of it was backed at map load.
- **graphics, ~594 MB:**
  - sampled textures (grew from map to map);
  - render targets, 74 MB;
  - frame rings, 3 x 64 MiB;
  - the 4K feedback copy, 33 MB, once an effect needed it;
  - pipelines and the like.
- **the rest, ~110 MB:** malloc, drawables, page tables.

In a 67-minute ten-map Mac run (`multimap.py`), the sampled textures were
the only thing that kept growing: 111 -> 428 MB by Metal's count.

**The caps** (tv/latelag):
- **24 MiB frame rings on tvOS** (was 64). The most any frame streamed in
  ~9900 s of telemetry is 19.5 MB; overflow buffers are kept for the rest.
- **No clearing of never-used window pages on wasm.** On the Mac the guest
  went 437 -> ~190 MB.
- **The game's texture cache: 96 MiB on the TV** (`game.texture_cache_mb`,
  set by tv_main.m; it was 256).
- **The host texture budget: 256 MiB on the TV, counted as Metal allocated
  it** (`host_metal_sampled_bytes`). Past it the least recently drawn go,
  4 a frame at most.
- **The feedback copy is let go after 600 frames unused.** The lifecycle's
  memory-warning trim shares its serial.

**A12, bedroom TV (output 1080p, rendering 3840x2160), interleaved:**
- base = tv/integrate 566bb6d4 (24 MiB rings already);
- fix = tv/latelag 912fa2f2.

| run | peak footprint | min available | peak Metal | textures (end) | missed slots (excl. ~30 fps stretches) | frames > 50 ms | file reads | conversions |
|---|---|---|---|---|---|---|---|---|
| b30long base | 946 | 1152 | 394 | 190 | 57 | 5 | 12 | 249 |
| b30long fix | **682** | 1416 | 399 | 195 | 53 | 5 | 12 | 250 |
| b30long fix | **676** | 1422 | 393 | 189 | 71 | 5 | 12 | 246 |
| b30long base | 934 | 1164 | 399 | 195 | 77 | 4 | 12 | 249 |
| a10 + b30 base | 1008 | 1090 | 463 | 259 | 83 | 7 | 14 | 350 |
| a10 + b30 fix | **752** | 1346 | 459 | 255 | 113 | 8 | 15 | 349 |

- **Footprint.** The peak is 255-265 MB lower in every pair.
  - That is the guest's memory: the window not cleared, and the smaller
    game cache.
  - Metal is the same, within a few MB. The rings were already 24 MiB in
    the base.
  - None of these sessions reached the host texture budget on the TV:
    textures stayed at 190-260 MB of Metal's count.
- **The 96 MiB game cache** caused no more reads from the map and no more
  conversions. During b30 traversal: 12 file reads in both builds, all at
  the load. Conversions were the same (246-250 against 249). So it stays
  at 96.
- **Map change.** The a10 -> b30 change took 23 missed slots in both builds
  (the load's own frame); no texture deletes in either.
  - Only one map change was made: the scene's later map_name commands all
    fired at once. Fixed in 396e5770, not yet re-run on the TV.
  - The Mac's seven-map run, with the budget full, evicted up to 184
    textures a second for a few seconds after each load, and those seconds
    missed no slots.
- **Not mine: stretches at ~30 fps.** Every b30long run but the first had a
  stretch of 28-127 s at ~30 fps: base 127 s, fix 39 and 28 s.
  - In those seconds guest and host time both went ~4x and the GPU to
    22 ms; the views' time rose with nothing else changing.
  - Footprint and available memory were flat, so it was not reclaim.
  - It began and ended mid-play, in both builds, as the bot player walked
    the beach's slope.
  - It looks like the A12 holding a lower clock for a while. Thermal state
    stayed "nominal" throughout.
  - It is excluded from the missed-slot column above, and left for the
    thermal and CPU work.

# CPU (A12)

Branch `tv/cpu` (from `tv/integrate` 20d1333f). The wasm2c guest's CPU
cost, above all for two-player split screen. What was measured, what
changed and what tvOS allows. Mac numbers: M4, 1080 render, warm,
`tools/tvos_bench.py --target mac` (now `--app` picks another build of
the Mac app), steady window, guest ms = wall - host renderer calls -
main-thread sleeps, ABBA order. `<scratch>/cpu/` has the runs.

## Bounds checks: what they cost (Mac)

The TV build compiles the translated game with explicit bounds checks
(`WASM_RT_MEMCHECK_BOUNDS_CHECK`: a compare and branch on every guest
load and store, ~152,000 call sites) because tvOS refused wasm2c's 8 GiB
guard-page reservation. The Mac build uses guard pages. The same C, the
same objects but the 37 compiled with wasm2c's flags (the translated
code, its runtime, the host files that include it), built both ways:

| scene | guard pages (2 runs) | bounds checks (2 runs) | cost, mean | guest p95 |
|---|---|---|---|---|
| a10 bridge | 0.76, 0.78 | 0.82, 0.93 | +0.10 ms (+14%) | 1.15 -> 1.36 |
| b30 beach | 1.50, 1.56 | 1.81, 1.77 | +0.26 ms (+17%) | 2.40 -> 2.75 |
| a10 cryo | 0.80, 1.04 | 1.21, 1.25 | +0.31 ms (+34%) | 1.11 -> 1.58 |
| split (2 views) | 1.38, 1.40 | 1.53, 1.57 | +0.16 ms (+12%) | 1.83 -> 1.91 |

So explicit checks are 12-34% of the guest's main-thread time (the
upper bound guard pages could give back). They are also code: the TV
executable's `__text` is 8.85 MB with checks, 6.52 MB with guard pages
(-26%; one translated file, `halo_5.c`, 375 KB -> 200 KB), less for the
A12's instruction cache and branch predictors to hold. The A12 is ~3-4x slower per
frame than the M4 here (guest 4.6-7 ms there), so on the TV the same
share is roughly 0.5-1.5 ms a frame. Host ms and draws were the same
in both builds.

## Guard pages in 4 GiB + 64 MiB (opt-in: `tvos_build.py --memcheck guard`)

Guard pages do not need 8 GiB. A guest address is a 32-bit index plus a
static offset that the load or store adds (wasm2c: `(u64)index + offset`,
no wrap). The largest static offset in this guest is **0x2ece338
(~47 MiB)**, so a 4 GiB + 64 MiB reservation covers every access that
can be made, the 2.5 GiB memory at its start and the rest PROT_NONE.
`host_main.c` makes that reservation itself when `HALO_GUEST_RESERVE_BYTES`
is defined (the TV's CMake does it for `guard`), and `tools/tvos_build.py`
scans the translated code's offsets before building and refuses if one
could reach past the end (`tools/test_tvos_build.py`).

Correctness: wasm2c's own signal handler calls only `SEGV_ACCERR` out of
bounds and anything else "call stack exhausted"; on Apple platforms a
PROT_NONE page faults as SIGBUS, so an out-of-bounds read reported the
wrong trap (on the Mac's 8 GiB build too). A handler in `host_main.c`
now decides by the fault address (inside the reservation: out of
bounds; anything else: the default action, a crash report).
`HALO_OOB_TEST=<hex>` (guest_main.c) reads that guest address at start:

| build | 0xa0000000 (just past) | 0xffffffff | 0x9ffffffc (last word) |
|---|---|---|---|
| Mac, 8 GiB guard | trapped: out of bounds | trapped: out of bounds | read 0 (in bounds) |
| Mac, 4 GiB + 64 MiB guard | trapped: out of bounds | trapped: out of bounds | read 0 |
| **A12, 4 GiB + 64 MiB guard** | **trapped: out of bounds** | **trapped: out of bounds** | **read 0** |

### What tvOS allows (A12, tvOS 27.0, `HALO_VM_PROBE=1` in the TV app)

| reservation | A12 |
|---|---|
| mmap 8 GiB (wasm2c's guard layout), also with MAP_NORESERVE; vm_allocate 8 GiB | **refused** (ENOMEM / KERN_NO_SPACE) |
| mmap 6, 5, 4.25, 4.0625, 4, 3, 2.5 GiB | ok |
| vm_allocate ANYWHERE 4 GiB + 64 MiB | ok |
| largest single reservation | **6.00 GiB**; all at once 7.50 GiB |
| the guard layout (4 GiB + 64 MiB, 2.5 GiB made accessible, an Xbox-window page written) | ok, at 0x7000000000 |
| with it held: further reservations | largest 1.94 GiB, 3.25 GiB in all (Metal and the rest have room) |
| reads past the accessible end, at 4 GiB - 1 and at the reservation's last 16 bytes | SIGBUS caught at the faulting address each time |

So the process's address-space budget is ~7.5 GiB with no single block
above 6 GiB: wasm2c's 8 GiB can never fit, but the 4.06 GiB guard layout
does, leaving 3.25 GiB. Reservation is not commitment: the footprint is
what is touched, the same in both modes.

**Decision: the default stays `--memcheck bounds`.** The guard layout
works on the A12 and traps correctly, but the A12 gain was small (below)
and the owner asked for no risky memory-layout change at the project's
end. `--memcheck guard` (or `OPENCE_TV_MEMCHECK=guard`) builds it.

## Where the guest's time goes (Mac profile)

`sample` (1 ms) on a build with function names (the driver's wasm-opt
re-run with `-g`), 30 s of the split scene with 2 views and 30 s of
b30; main thread, % of all its samples (~63% is the wait for the
drawable, `__semwait_signal`):

| | split | b30 |
|---|---|---|
| render_window (all rendering, guest + host) | 31.8% | 30.1% |
| D3DDevice_DrawIndexedVertices (inclusive) | 16.8% | 15.0% |
| host_metal_draw (host encoding, inclusive) | 15.5% | 13.3% |
| prepare_draw (the D3D8 layer's draw setup) | 6.5% | 7.4% |
| xgpu_texture_get (texture lookup per bound texture) | 3.3% | 4.6% |
| **of which texture_entry_result: config_boolean("display.high_res_hud")** | **2.1%** | **1.8%** |
| **of which menu_art_texture (linear scan)** | - | **1.9%** |
| memmove in host_metal_draw (vertex streams into the ring) | 3.8% | 1.4% |
| clock_gettime in host_metal_draw/set_targets (frame log timing) | 1.9% | 1.6% |
| game_tick (AI, objects, physics, collision) | 0.7% | 3.2% |

(In the split scene player 2 stands still and the cryo bay is quiet: the
simulation share there is a floor, not what late-level co-op with both
players moving costs. b30's firefight shows it rising to ~3%.)

The game's simulation is small; the cost is the draw path: per draw the
D3D8 layer's setup, the texture lookups and the host's encoding. Split
screen doubles draws (~950/frame). Two cheap guest-side wins stood out,
both lookups done for every texture of every draw:

1. **`display.high_res_hud` read per bound texture** (`xbox_textures.c`
   `texture_entry_result`): `config_boolean` takes a mutex and finds the
   setting by `strcmp` over ~150 names. Now kept, re-read when
   `config_changes()` moves.
2. **Menu art found by a linear scan** (`menu_files.c`
   `menu_art_texture`, up to 1024 entries, almost always a miss in play).
   Now an open-addressed table by data address.

Mac A/B, ABBA (guest ms mean, steady window):

| scene | before (1st, 4th) | after (2nd, 3rd) | change |
|---|---|---|---|
| split | 1.39, 1.38 | 1.30, 1.23 | -0.12 ms (-9%) |
| b30 | 1.72, 1.52 | 1.41, 1.44 | -0.20 ms (-12%) |

Looked at and left:
- **Vertex streaming copies** (memmove into the Metal ring, 1.4-3.8%):
  writing straight into the ring needs the guest to know ring addresses
  (a protocol change in `host_metal.m`/`d3d8_metal.c`, where #98, #105,
  #106 and #110 also work); not done here.
- **prepare_draw's memcmp** of uniforms (0.8%): it is the redundant-state
  filter itself (it saves uploads); left.
- **Frame-log clocks** (`clock_gettime` per host call, 1.6-1.9%): the
  TV's telemetry needs them; a cheaper clock (`mach_absolute_time`
  directly, or timing scopes rather than calls) is a follow-up.
- **Compile flags**: the TV build already compiles the translated code
  with `-O3` (CMake's Release `-O3 -DNDEBUG` comes before the target's
  `-O2`; the translated files get `-O3` last). No LTO: one 99 MB file
  (`halo_0.c`) dominates and LTO across 24 files would cost minutes per
  build for cross-file inlining between guest functions that wasm2c
  already keeps in one module. `-mcpu=apple-a12` (the SDK default for
  tvOS 17.4 is apple-a7): compiles, gives LSE atomics, not measured; it
  would drop the Apple TV HD, so not made default. PGO: the profiles in
  `pgo/` are for the Linux/Windows native builds and clang 22, not for
  the wasm2c C.

## A12 results (bedroom TV, split + b30, 2160 render)

One session, warm (one shader salt), in order: warm-up (bounds), A =
before (`tv/cpu` with the two texture-lookup fixes reverted, bounds
checks), B = fixes + bounds checks (the default build), C = fixes +
guard pages, C, B, A. The TV's output was **1080p** for passes 0-5
(drawable 1920x1080, `screen: 852x480 drawn at 3840x2160`). **Pass 6
(A) is excluded**: its frame pacing changed (wall p10 12.7 ms against
16.5 in every other pass, GPU p50 39-55 ms against 15-30), which
matches the TV's output being switched to 4K Dolby Vision during the
session; it is not comparable. So "before" is one pass. Steady window,
ms per frame:

| scene | build | passes | guest p50 | guest p95 | guest mean | host mean | wall p95 | >33 ms | >50 ms |
|---|---|---|---|---|---|---|---|---|---|
| split | A before | 1 | 4.47 | 5.42 | 4.45 | 3.14 | 16.87 | 0 | 0 |
| split | B fixes, bounds | 2 | 4.22 | 5.14 | 4.30 | 3.28 | 16.89 | 0 | 0 |
| split | C fixes, guard | 2 | 4.21 | 5.04 | 4.09 | 3.48 | 16.89 | 0 | 0 |
| b30 | A before | 1 | 6.99 | 9.43 | 6.79 | 2.24 | 33.60 | 731 | 0 |
| b30 | B fixes, bounds | 2 | 6.89 | 9.21 | 6.54 | 2.43 | 33.63 | 729 | 0 |
| b30 | C fixes, guard | 2 | 6.49 | 9.10 | 6.27 | 2.66 | 33.62 | 698 | 0 |

- The texture-lookup fixes: guest mean -0.15 ms in split (-3%), -0.25
  ms in b30 (-4%); p50 -0.25/-0.1 ms. One "before" pass, so within
  ~0.1-0.2 ms of noise; the direction matches the Mac's.
- Guard pages over bounds checks: guest mean -0.21 ms (split) and
  -0.27 ms (b30), ~5%, much less than the Mac's 12-34%; but host ms
  rose ~0.2 ms in both scenes in both guard passes, so the frame's CPU
  total is about unchanged. Not worth a memory-layout change now.
- Neither scene is CPU-bound on the A12: split holds 60 fps (wall p95
  16.9) and b30 at 2160 is GPU-bound (GPU p50 30 ms, ~700 frames over
  33 ms in all builds alike).
- None of these passes had a slow stretch (guest > 15 and host > 6 ms
  for more than one frame: none in any pass).

## #110's 30 fps stretches in b30long: the main thread got slower, not the game

#110's 15-minute b30 split-screen runs with a bot (`b30long`, A12, 2160
render into a 1080p drawable, before the TV's output went to 4K Dolby
Vision) had stretches at ~30 fps in 3 of 4 runs (39, 28 and 127 s; both
builds). From their telemetry and frame logs
(`<scratch>/latelag/tvmem2/run-*-b30long-*`), stretch seconds against
the run's 60 fps seconds:

| run | guest ms/frame | host ms/frame | telemetry's own ms/s | GPU p50 | draws/frame | game ticks/s |
|---|---|---|---|---|---|---|
| fix 1 | 5.85 -> 23.6 (x4.0) | 2.89 -> 9.5 (x3.3) | 0.10 -> 0.24 (x2.3) | 14.5 -> 21.8 | x0.85 | 30 -> 30.6 |
| fix 2 | 5.95 -> 21.3 (x3.6) | 2.87 -> 8.0 (x2.8) | 0.10 -> 0.24 (x2.3) | 14.5 -> 21.0 | x0.74 | 30 -> 29.8 |
| base 2 | 6.17 -> 25.1 (x4.1) | 2.98 -> 9.6 (x3.2) | 0.10 -> 0.23 (x2.4) | 14.5 -> 22.0 | x0.87 | 30 -> 30.6 |

- **It switches between two consecutive frames, both ways**, with the
  same draw count (run 3: frame at 666.29 s 16.9 ms, guest 6.6, host 2.9;
  next 30.3 ms, guest 20.4, host 9.8; and back at 796.69 s), mid-play,
  no event, no texture uploads or conversions, no file reads, thermal
  nominal, memory flat.
- **Everything on the main thread slows by the same factor**: the
  guest's translated code, the host's Metal encoding (Objective-C, the
  driver) and even the telemetry's fixed per-second bookkeeping. A
  guest code path (AI, path search, collision, sound) would raise guest
  time only, and with the work (draws, ticks) unchanged it cannot make
  the host's encoding 3x slower.
- GPU command-buffer time also rises 1.5x (14.5 -> 22 ms): either the
  GPU's clock dropped with the CPU's (a system performance state), or
  the GPU times include waiting for a CPU-side encoder.

So the main thread ran on a slower core or at a much lower clock for
those stretches: an A12 Tempest (efficiency) core is roughly 3-4x slower
than a Vortex core, which fits the factor. It is not a guest hot spot
and nothing in the guest's code would show it in a Mac profile (the Mac
was not tried: there is no slow game path to find). Why tvOS moved the
thread is open: the game's main thread has the default QoS of a UIKit
main thread; the TV may down-clock the whole SoC (a power or display
state; the stretch's ends are not on 1 s boundaries).

To confirm on the next device session, each telemetry second now carries
**`probe_us`** (a fixed chain of 2^16 dependent multiply-adds on the main
thread, ~40-60 us on a fast core; Mac M4: 111 us on a performance core,
182 us when clamped with `taskpolicy -b`) and **`cpu`**
(`pthread_cpu_number_np`). If `probe_us` rises ~3-4x in a stretch it is
the CPU; `cpu` says whether it moved to another core. Possible fixes
then, none tried: give the main thread a `QOS_CLASS_USER_INTERACTIVE`
override and the render work an `os_workgroup` (the audio/Metal frame
interval workgroup API) so the scheduler keeps it on performance cores,
and tvOS's Game Mode (`GCSupportsGameMode` in Info.plist) where the TV
honours it.

## Shadows (A12)

### What "shadows" is

One technique, per object (`rasterizer_xbox_shadows.c`, `render_objects.c`):
each shadow-casting object is drawn from its light into a 128x128 R5G6B5
map (a BGRA8 128x128 Metal texture here), the map is blurred into a second
128x128 target, and the shadow is *applied* by drawing again every level
(BSP) triangle inside the object's shadow volume, onto the 4K screen, with
a multiply blend (`dst *= 1 - a * map * (1 - shadow colour)`), depth EQUAL,
stencil reject. There is no full-screen pass and no static-shadow cache to
add (every shadow follows a moving object; the map is redrawn because the
object or its light moved).

A12 split per phase (the bandwidth session's pass-split log, 3840x2160,
fragment ms a frame; the split's 4K load+store, ~0.5 ms a phase pass, is
in the apply's figure):

| scene | shadow-map passes (draw + clear) | blur passes | screen apply | apply draws/frame |
|---|---|---|---|---|
| bridge | 0.04 | 0.03 | 2.67 | 17 |
| b30 | 0.06 | 0.04 | 3.45 | 6 |
| cryo | 0.06 | 0.04 | 2.86 | 34 |

So the maps cost nothing worth saving (they are tiny, cleared once, hoisted
out of the screen pass, and already only read by the apply): no load/store,
format or memoryless change can win anything there. All of the cost is the
apply: shading and blending the whole of each level triangle under a
shadow, on a 4K screen, when the shadow covers a small part of it.

### Change: clip the apply to where it can change a pixel (default on)

`2e1c4813`. The apply leaves a pixel *exactly* as it was wherever the map
(border colour 0) is 0 or the light-axis fade is 1, i.e. outside the box
forward/left in [-r, r] (plus 4 map texels), up in [-0.5r, 4r] around the
object. That fade (`linear_corner_fade`) was read from the map: a 16x16
`max(s, t) * 0x11` ramp, 1 for `d >= 3.875r` and `d <= -0.484r` along the
light axis, inside the volume's own planes. `rasterizer_shadow_screen_bounds`
projects the box, cut at the near plane, with the frustum's own world to
clip transform; `halo_draw_bounds` gives the rectangle (plus 2 units of
slack) to the Metal device, which intersects the draws' scissors with it.
No clip when the box cannot be bounded on screen, in `rasterizer_shadows_debug`,
or on the GL renderer. `HALO_SHADOW_BOUNDS=0` turns it off;
`toggle:N` alternates in N-frame blocks (diagnostic). `HALO_GPU_STATS=1`
logs the clipped draws' scissor area before and after.

Byte-identical (Mac M4, 3840x2160, game time frozen with `game_speed 0`
14 s in, a shot every 60 frames with `HALO_SHADOW_BOUNDS=toggle:60`, so
consecutive shots alternate clip on/off): **every pair after the freeze
settles is identical (max diff 0)**: bridge 26/26, b30 26/26, cryo 26/26
(the one differing pair in bridge and b30 straddles the freeze itself, as
it does with the hoist toggle). b30 split screen (two viewports, no bot,
frozen 100 s in, toggle:240, a shot every 240 frames): 24/24 identical
after the freeze settled.

Second step (`8f7b0f3c`, the same switch): the screen rectangle
bounds the whole box, which for close shadows (the player's own, the
marines beside him: a box 4.5 r long, often around the camera) is most of
the view. So the game also hands the box itself (`halo_draw_bounds_box`,
its 6 planes), and the device cuts each apply draw's triangles (positions
read from the level's vertex buffer, as the GPU will) to the box and the
near plane, projects them with the shadow's own world-to-clip constants
(c[-96..-93]) and narrows that draw's scissor to what is left, plus
half a game unit and 2 pixels of slack. A draw with nothing left in the
box is skipped. Same argument: a pixel only changes inside the box.

Re-checked after the second step, same method: bridge 26/26, b30 26/26,
cryo 25/25 identical after the freeze; b30 split (frozen, no bot,
toggle:240) 24/24.

Scissor area the apply's draws cover (Mac, frozen, `HALO_GPU_STATS=1`):

| scene | apply draws/frame | full viewport | box rectangle | each draw cut to the box | draws skipped |
|---|---|---|---|---|---|
| bridge | 7 | 58.1 Mpx | 58.1 Mpx | 3.5 Mpx (-94%) | 5 |
| b30 | 6 | 49.8 Mpx | 41.6 Mpx | 6.7 Mpx (-87%) | 3 |
| cryo | 34 | 282 Mpx | 116 Mpx | 4.4 Mpx (-98%) | 18 |
| b30 split (frozen view) | 10 | 41.5 Mpx | 41.5 Mpx | 8.4 Mpx (-80%) | 6 |

The scissor is a bound, not the shaded area: fragments are shaded only
where the triangles are and the depth/stencil tests pass. Mac (M4,
pass per phase, the on/off blocks of one frozen toggle:60 run), shadow
phase fragment ms a frame (includes the split's 4K load+store):

| scene | clip off | clip on |
|---|---|---|
| bridge | 1.11 | 0.65 (-42%) |
| b30 | 0.94 | 0.72 (-23%) |
| cryo | 1.13 | 0.73 (-35%) |

CPU cost: a few hundred triangles a frame clipped on the guest (the apply
draws are small index ranges); not visible in the Mac's frame times.

### A12 (bedroom TV, 2026-10-23)

One signed binary, `3a4a603a` (tv/shadows on tv/integrate `9b8dcc0a`:
dynres on by default). A = `HALO_SHADOW_BOUNDS=0`, B = default. Warm,
`HALO_RESOLUTION_SCALING=2160`, pass log on in every run, order A B B A.

**Output mode: 1920x1080** for r1-A, r2-B and r3-B (bridge, b30). The owner
switched the TV to 4K Dolby Vision during r3-B, ~20:25 UTC: r3-B's
b30split ran at 3840x2160 (dynres 75% of 2160 lines, not comparable), and
r3-B's split did not reach two views. The 1080p pairs are complete for
bridge and b30 (A, B, B); r4-A was not run.

At 1080p output dynres holds the scene at its floor, 50% of 2160 lines
(1080 lines), and the frame at ~60 fps either way, so fps and frame-time
counts cannot show the change; the screen pass's fragment time can:

| scene (1080p output, 1080 lines) | run | fps | >33.3 | >50 | GPU ms/frame | screen pass fragment ms | shadow-map passes ms |
|---|---|---|---|---|---|---|---|
| bridge | r1-A | 59.67 | 1 | 1 | 14.71 | 13.36 | 0.11 |
| bridge | r2-B | 59.99 | 0 | 0 | 14.74 | 13.28 | 0.12 |
| bridge | r3-B | 59.92 | 1 | 0 | 14.77 | 13.28 | 0.12 |
| b30 | r1-A | 59.95 | 1 | 1 | 14.65 | 12.35 | 0.13 |
| b30 | r2-B | 59.95 | 1 | 1 | 14.63 | 12.26 | 0.14 |
| b30 | r3-B | 59.95 | 1 | 1 | 14.65 | 12.31 | 0.14 |
| split (a10) | r1-A | 59.96 | 0 | 0 | 14.81 | 12.92 | 0.17 |
| split (a10) | r2-B | 59.94 | 0 | 0 | 14.88 | 12.78 | 0.20 |
| b30split | r1-A | 59.96 | 0 | 0 | 14.69 | 12.43 | 0.16 |
| b30split | r2-B | 59.96 | 0 | 0 | 14.69 | 13.08 (p50 12.42) | 0.19 |

(GPU ms is the frame's span, which dynres steers to ~14.7 ms; the screen
pass holds the apply.) With the clip the screen pass is 0.05-0.14 ms
cheaper (bridge -0.08, b30 -0.04/-0.09, split -0.14; b30split's mean is
pulled by one hitch, its p50 equal). At 1080 lines the apply covers a
quarter of the 4K pixels, so this is the small end of the gain; at 1080p
output the A12 already holds 60 with dynres at its floor, so the saving
there buys headroom, not frames.

**4K output: not measured.** The rerun at 3840x2160 (same binary) stopped
at its first pass: 3 of 4 cold launches came up inactive and stopped
(`lifecycle: launched inactive`, `stopped: GPU idle`; the saved state or
the split views never reached), the one that ran (b30, cold) is not an
A/B. Not retried (the coordinator's rule for a TV in a mode prompt or
black screen), and the project was wrapped up after it. The Mac's
4K numbers above are the evidence for 4K; an A12 4K ABBA (and the b30
`HALO_DYNRES=0` pair) is what remains.


### Not done, and why

- **Map formats, clears, stores, memoryless**: the maps total < 0.1 ms.
- **Skipping zero-pixel shadows**: done by the second step (a draw with
  nothing in the box, or an empty scissor, is not sent).
- **Static shadow cache**: nothing static to cache (the game draws shadows
  only for objects, and redraws when they or their light move).
- **Per-triangle stencil/scissor beyond a rectangle per draw**: a draw's
  rectangle still contains its triangles' parts outside the box's
  silhouette; finer cuts would need geometry changes (no visual risk, but
  more CPU), not attempted.
- **Opt-in lower shadow resolution / no blur**: the maps are not where the
  time goes; the apply's cost is per screen pixel, so a smaller map would
  not reduce it.

# Bundled shader manifest (A12)

Branch `tv/shaderpack` (from tv/integrate 85a49369). Goal: no shader-compile
stall even on the very first launch after an install, and none in a level
the player has never visited.

## What was built

1. **Per-map records** (`host_shader_manifest.c`). The manifest now has map
   records and shader-use records, so each shader and pipeline is grouped
   by the map that drew it. The guest's `game_load` calls `host_metal_map`
   before the scenario loads. The app's own record
   (`Library/Caches/<bundle>/metal_shaders.manifest`) is written that way
   too.
2. **Preparation at map load, not at launch** (`host_metal.m`, "the shader
   cache"). Nothing is compiled at app start.
   - While the loading screen is up, the loading map's set (from the
     bundled pack and the app's record) is compiled: 4 at a time through
     Metal's asynchronous compile, then its pipelines.
   - 5 s later, every other recorded shader is compiled at utility priority,
     2 at a time, and kept. These are shaders only another map's visit
     recorded; about 260 for the whole campaign.
   - 120 s into a map, the next map's pipelines (campaign order) are made at
     low priority and dropped, which leaves them in the system's caches.
   - A new map's load cuts all of this short and drops the old map's
     unused functions.
   - Tuning: `HALO_METAL_SHADER_PREPARE_JOBS`, `_PREPARE_REST`, `_PREPARE_NEXT`.
3. **The bundled pack** (`tools/shader_pack.py`, `metal_shaders.pack` in the
   app bundle).
   - `generate` runs the Mac app once per map (ui, a10 … d40) with the
     *shader tour* (`port/linux/game/shader_tour.c`, `HALO_SHADER_TOUR=on`).
     The tour:
     - puts a scripted camera at up to 3 subcluster middles of every
       cluster of every BSP, looking 6 ways, with the game at 3x speed;
     - makes every object tag and every effect tag in front of the camera;
     - runs the screen effects (convolution, filter, video, fade);
     - gives the player every weapon in turn while the bot input fires,
       throws and jumps, with camo, flashlight and shield damage.
   - Generation uses the TV's texture path (`HALO_METAL_NO_BC=1`).
   - 11 runs take about 15 min on the M4 and are cached per translator hash
     under `build/shader_pack/<hash>/`. It is a separate step, not part of
     the build.
   - The result: 263 distinct shaders, 1074 pipeline records, **119 KiB**
     zlib (7.6 MiB of MSL source expanded). That is the bundle size added.
4. **Keyed to the build.**
   - The pack's header names a hash of the sources that write shader text
     (`nv2a_psh.c`, `nv2a_vsh.c`, `xgpu.h`, `xgpu_text.c`, `d3d8_metal.c`,
     `metal_protocol.h`).
   - `tools/tvos_build.py` bundles it (after `add_icons`) only if that hash
     is the tree's, and removes a stale one otherwise.
   - The host also checks the compile-option key (MSL 2.2, invariance,
     record layout).
   - Missing or mismatched means it is silently not used. The log says why.
5. **Correctness unchanged.**
   - A draw takes a prepared function only for exactly the text it would
     compile (`shader_text` of the same source and kind). Anything else
     compiles on the main thread as before.
   - Prepared pipelines are dropped; draws make their own.
   - So a wrong or stale pack can cost time but cannot change a picture.

## Mac and TV shader text are the same

The TV's own records, pulled from the bedroom A12 (`<scratch>/shaderpack/tv-pull/`),
were compared byte for byte with the Mac-generated pack:
- the tvbench record (bridge + b30 runs): **60 / 60 shaders** and **43 / 43
  pipelines** (formats and blend) are in the pack;
- the TV's long-play record (several sessions, a10 + b30 and more): **132 /
  147 shaders (90%)**, 124 / 152 pipelines.

So the MSL text does not depend on Mac vs TV or on BC handling. The 15
missing shaders are pixel-shader variants: other combiner inputs, an
alpha-test variant, a cube-map stage. They come from play the tour does not
reproduce, such as particular weapon or effect states. The low-priority
whole-pack pass compiles what the pack has; anything else still compiles in
the draw, as before.

## Coverage in play (Mac M4, 1080, `tvos_bench --target mac`, nopack vs cold-with-pack, 1 run each)

From the host's per-map log ("of the N new shader texts its draws compiled,
P were prepared …"), with the pack, in the cold run:

| scene | new texts drawn | prepared | compiled by the draw first | in no record |
|---|---|---|---|---|
| a10 bridge | 76 | 73 | 1 | 2 |
| a10 cryo | 48 | 44 | 3 | 1 |
| b30 beach | 79 | 70 | 2 | 7 |
| **b40, fresh launch (never visited)** | 80 | 77 | 3 | **0** |

The frame-log results (60 s windows; "compile ms" is the window's sum):

| scene | mode | worst frame ms | frames >33 ms | >50 ms | compile ms in window |
|---|---|---|---|---|---|
| bridge | nopack | 93 | 1 | 1 | 142 |
| bridge | **pack** | **11.5** | 0 | 0 | **2** |
| cryo | nopack | 402 | 3 | 2 | 719 |
| cryo | **pack** | 71 | 1 | 1 | **30** |
| b30 | nopack | 257 | 3 | 3 | 305 |
| b30 | **pack** | 84 | 1 | 1 | **51** |
| b40 new level | nopack | 753 | 8 | 6 | 1305 |
| b40 new level | **pack** | 402* | 3 | 3 | **420** |

\* b40's remaining stall is its first frame, 0.4 s in, with 91 compiles. The
fresh launch's window starts while the map's own set is still being
prepared: the preparation is 490 ms on the M4 and the frame waits for it.
That frame is part of the load (the insertion cinematic's first frame); the
rest of the b40 window has no compile frame over 13 ms. Without the pack,
b40 stalled 219 ms at 11 s and 113 ms at 31 s in play.

(These are Mac numbers, not TV numbers. The M4 compiles about 3x faster than
the A12.)

## A12 device

Pending. The bedroom TV was booked by other chats' sessions (#113, #106,
#110) until at least ~21:45 UTC on 2026-10-23. The device comparison is
`tvos_bench.py run --target device --scenes bridge,b30,cryo,b40,a30 --modes
nopack,cold --repeats 2`, with nopack and cold interleaved by the plan. It
has not been run yet; see "What remains".

## What remains

- The A12 runs above (first launch with and without the pack, the new-level
  load, and the load-frame cost of the preparation).
- Whether reinstalling clears the system shader cache on tvOS. Not verified;
  the bench uses the salt, which is the conservative case.
- The 10% of the TV's long-play shaders the tour misses: pixel-shader
  combiner variants from states the tour does not reach. A richer tour
  (vehicles driven, AI fights) or an offline enumerator over the shader tags
  could close it.
- `MTLBinaryArchive` for the pipelines remains untested.
