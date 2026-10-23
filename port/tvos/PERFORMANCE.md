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
   refused. Tooling detects both; no performance effect measured.

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
