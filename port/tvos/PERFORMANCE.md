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

## Device: pending

Batch prepared (`<scratch>/bw/device-session.sh`): one install of the
private signed copy (`<scratch>/tv-deploy-bandwidth/OpenCETV.app`, commit
c3ad3460), then on the same binary: hoisting off vs on vs on + depth
discard, warm, bridge/b30/cryo at 3840x2160, two repeats in alternating
order, plus the pass log (phase split, and hoisted) for the A12 per-phase
table. Waiting for the owner.
