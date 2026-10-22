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

# Texture formats (A12)

Branch `tv/textures`. **No device numbers yet**: everything below was
measured on the Mac mini (M4) or in the tvOS simulator. It shows correctness,
memory and fidelity, not A12 GPU time. The device A/B (guest / exact / astc,
warm x2 and one cold each, 4K bridge and b30) is planned but has not been run.

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

## Planned device runs (not yet run)

    python3 tools/tvos_deploy.py sign --output <scratch>/tv-deploy-textures/OpenCETV.app
    python3 tools/tvos_bench.py run --target device --install --signed <scratch>/tv-deploy-textures/OpenCETV.app \
        --maps <scratch>/halo_data/maps --plan bridge/2160/2,b30/2160/2 --modes warm --env HALO_TEXTURE_FORMAT=guest --out OUT-guest
    # likewise without --env (exact) and with HALO_TEXTURE_FORMAT=astc; then one cold launch of each

Label the cold runs **tv/bench-salt cold**: on this branch
`HALO_METAL_SHADER_SALT` changes only a comment, so the system's pipeline
cache may still hit. The real salt fix is on tv/shaders.
