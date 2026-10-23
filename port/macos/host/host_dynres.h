/*
HOST_DYNRES.H

Dynamic resolution (display.dynamic_resolution; port/tvos/PERFORMANCE.md,
"Dynamic resolution (A12)"): the controller that picks, once a frame, the
fraction of the screen's resolution the player views' 3D scenes are drawn
at. The HUD, menus and text stay at the screen's: the scene is drawn into a
target of the screen's size, in its top left part (viewports scaled,
nothing allocated again), and scaled up into the back buffer before the HUD
is drawn over it (port/macos/guest/d3d8_metal.c; host_metal.m's
host_metal_scene_upscale).

What it reads, a few frames late and never waiting on the GPU: each frame's
GPU time (its main command buffer's, from the completed handler: the time
the GPU spent on it after it finished the frame before, so frames queued
behind each other are not counted twice), and when the display showed it
(a drawable's presented handler; 0 if it was not shown).

How it decides (host_dynres.c has the reasons): down at once when the
GPU is the reason frames are late, as far as a model of the cost says;
up a step at a time by trying, with a wait after any step down that
doubles each time a step up fails. Steps are quantized (host_dynres_steps).

Testable by itself (tools/test_dynres.py): no Metal, no clock of its own.
*/

#ifndef __HALO_MACOS_HOST_DYNRES_H
#define __HALO_MACOS_HOST_DYNRES_H

#include <stdint.h>

/* the steps, as fractions of the ceiling's lines: 2160, 2016, 1872, 1800,
1728, 1620, 1512, 1440, 1344, 1260, 1152, 1080, 960, 864, 720 of 2160 */
#define HOST_DYNRES_STEPS 15
extern const double host_dynres_steps[HOST_DYNRES_STEPS];

/* the controller's numbers (fractions of the display's refresh interval
where they are times) */
struct host_dynres_parameters
{
	/* a frame's GPU time past this share of the interval is "hot" (at the
	fastest clock, or nearly); the model aims a step down at target_share */
	double hot_share, target_share;
	/* the share of a frame's GPU time that does not shrink with the pixels */
	double fixed_share;
	/* frames at a step before a step down can happen; frames in a row
	clear before a step up is tried */
	uint32_t down_frames, up_frames;
	/* after a step up, the frames that decide whether it holds */
	uint32_t probe_frames;
	/* the wait after a step down before a step up (ms), the most it
	doubles to, and how long a step up must hold to reset it */
	double hold_ms, hold_most_ms, stable_ms;
};
extern const struct host_dynres_parameters host_dynres_defaults;

/* on or off, and the fewest lines a scene is drawn at with one view and
in split screen */
void host_dynres_configure(int enabled, double floor_lines, double split_floor_lines);
int host_dynres_enabled(void);
/* a fixed fraction instead (diagnostic, HALO_DYNRES_FIXED; 0: none), or
one that alternates every toggle_frames frames with other */
void host_dynres_fixed(double fraction, uint32_t toggle_frames, double other);

/* the fraction of the target's lines the frame being drawn uses (one of
host_dynres_steps): views is the player views it draws, thermal
NSProcessInfo's thermal state (0 nominal to 3 critical), slot_ns the
display's refresh interval, now_ns the clock (host_frame_log_now's), lines
the back buffer's height and ceiling_lines the display's (the drawable's).
Called once a frame, at its first view; the same answer for that serial */
double host_dynres_frame_fraction(uint64_t serial, int views, int thermal, uint64_t slot_ns, uint64_t now_ns,
	double lines, double ceiling_lines);
/* frame serial presented by the host (main thread): the fraction it drew
its scene at, or -1 (no scene drawn: a menu) */
void host_dynres_frame_done(uint64_t serial, double fraction);
/* frame serial's GPU time (its command buffers' completed handler, any
thread: the time the GPU was busy with them after the frame before was
done, not their wait in the queue), and when the display showed it
(presented handler; 0: skipped) */
void host_dynres_gpu(uint64_t serial, uint64_t busy_ns);
void host_dynres_shown(uint64_t serial, uint64_t shown_ns);

/* what the controller is doing (the log, the telemetry, the tests) */
struct host_dynres_status
{
	double fraction;
	uint32_t step;
	uint64_t steps_down, steps_up, probes_failed;
	/* frames counted, those late on the display, and of them the GPU's */
	uint64_t frames, late, late_gpu;
	double hold_ms;
	/* the recent frames' GPU ms (a moving average) */
	double gpu_ms;
};
void host_dynres_status(struct host_dynres_status *status);

/* back to its start (tests) */
void host_dynres_reset(void);

#endif
