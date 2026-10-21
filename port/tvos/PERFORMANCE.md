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
