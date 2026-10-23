/*
HOST_DYNRES.C

The dynamic resolution controller (host_dynres.h).

What a frame's GPU time does and does not say. The A12 runs its GPU's
clock as slowly as the work allows: a scene that holds 60 frames a second
shows 14-15 ms of GPU time a frame whether it is drawn at 1080 lines or at
2160 (port/tvos/PERFORMANCE.md, "Dynamic resolution (A12)"). So the GPU
time cannot say how much room is left. It can say when there is none: a
frame whose GPU time is past the display's interval (hot_share of it) was
drawn at the fastest clock and still did not fit, and that time is its
real cost. The display's own times (when it showed each frame) say which
frames were late; a late frame whose GPU time was short was late for the
CPU's sake (a load, a shader compile), which no resolution helps.

So the controller
- goes down when the GPU is late (hot_frames of the last 8 frames hot, or a
  frame shown late whose GPU time was hot): at once, as far as a model of the
  cost says (a fixed part, fixed_share, and a part in proportion to the
  pixels, from the late frames' times), at least a step;
- goes up only by trying: a step, after up_frames frames in a row clear of
  the interval and hold_ms since the last step down (four times as long
  while the device is hot: thermalState serious); if within probe_frames the
  step is too much (its first frame past the interval) it goes back and
  waits twice as long before trying that step again (8, 16, 32 ... s, at
  most hold_most_ms), and a step that held stable_ms resets its wait. A
  failed try costs one frame late, a handful in the first minutes and then
  one in minutes; never shimmer between two steps;
- keeps the ceiling at the display's lines (a 2160-line picture shown on a
  1080p output is drawn at 1080: the rest would be scaled away), two steps
  lower while the device is hot (thermalState serious; critical: four),
  and the floor where the settings put it (split screen has its own; never
  above the ceiling).
A frame's times count only against the fraction it was drawn at: the
frames queued when it changes are skipped, as are frames with no 3D view
(menus).
*/

#include "host_dynres.h"

#include <math.h>
#include <stdatomic.h>
#include <string.h>

const double host_dynres_steps[HOST_DYNRES_STEPS] =
{
	2160.0 / 2160.0, 2016.0 / 2160.0, 1872.0 / 2160.0, 1800.0 / 2160.0, 1728.0 / 2160.0, 1620.0 / 2160.0,
	1512.0 / 2160.0, 1440.0 / 2160.0, 1344.0 / 2160.0, 1260.0 / 2160.0, 1152.0 / 2160.0, 1080.0 / 2160.0,
	960.0 / 2160.0, 864.0 / 2160.0, 720.0 / 2160.0,
};

const struct host_dynres_parameters host_dynres_defaults =
{
	.hot_share = 0.97,
	.target_share = 0.85,
	.clear_share = 0.94,
	.fixed_share = 0.25,
	.hot_frames = 3,
	.down_frames = 4,
	.up_frames = 180,
	.probe_frames = 120,
	.hold_ms = 8000.0,
	.hold_most_ms = 256000.0,
	.stable_ms = 60000.0,
};

/* frames remembered (more than are ever in flight) */
#define RING 256
/* split screen's first step, before it has measured its own: 1620 of
2160 lines (56% of the pixels: two views of b30 at 2160 took ~26 ms of GPU
a frame on the A12, measured in play, PERFORMANCE.md) */
#define SPLIT_FIRST_STEP 5
/* a frame's GPU time past this many display intervals steps down at once */
#define VERY_HOT 1.5
/* a display slot is missed by a frame shown this much of an interval or
more after the one before it */
#define LATE_SLOTS 1.5

static struct
{
	int enabled;
	/* the floors, in lines */
	double floor, split_floor;
	double fixed, fixed_other;
	uint32_t fixed_toggle;

	/* each frame's: the fraction it drew its scene at (-2: unknown, -1:
	no scene), its GPU times and when it was shown (any thread) */
	uint64_t frame_serial[RING];
	double fraction_of[RING];
	_Atomic uint64_t gpu_serial[RING], gpu_ns[RING];
	_Atomic uint64_t shown_serial[RING], shown_ns[RING];
	/* the next frame to count, and the display's time of the one before */
	uint64_t read_serial, previous_shown;

	/* this frame's answer */
	uint64_t answered_serial;
	double answer;

	uint32_t step;
	/* the views of the frames counted, and each case's step when it was
	left (one view, split screen: a split-screen frame costs nearly twice
	one view's, so each starts again where it was) */
	int split;
	uint32_t step_of[2];
	uint32_t frames_at_step, recent_hot, clear_frames;
	double clear_ms[256];
	uint64_t step_changed_ns, last_down_ns;
	double hold[HOST_DYNRES_STEPS];
	int probing;
	uint32_t probe_frames;
	uint64_t steps_down, steps_up, probes_failed, frames, late, late_gpu;
	double gpu_ms;
} dynres;

static void holds_reset(void)
{
	int index;

	for (index = 0; index < HOST_DYNRES_STEPS; index++)
		dynres.hold[index] = host_dynres_defaults.hold_ms;
}

void host_dynres_reset(void)
{
	memset(&dynres, 0, sizeof(dynres));
	holds_reset();
	dynres.step_of[1] = SPLIT_FIRST_STEP;
}

void host_dynres_configure(int enabled, double floor_lines, double split_floor_lines)
{
	if (!dynres.hold[0])
	{
		holds_reset();
		dynres.step_of[1] = SPLIT_FIRST_STEP;
	}
	dynres.enabled = enabled != 0;
	dynres.floor = floor_lines;
	dynres.split_floor = split_floor_lines;
}

int host_dynres_enabled(void)
{
	return dynres.enabled;
}

void host_dynres_fixed(double fraction, uint32_t toggle_frames, double other)
{
	dynres.fixed = fraction;
	dynres.fixed_toggle = toggle_frames;
	dynres.fixed_other = other;
}

void host_dynres_gpu(uint64_t serial, uint64_t busy_ns)
{
	uint32_t slot = (uint32_t)(serial % RING);

	atomic_store(&dynres.gpu_ns[slot], busy_ns);
	atomic_store(&dynres.gpu_serial[slot], serial);
}

void host_dynres_shown(uint64_t serial, uint64_t shown_ns)
{
	uint32_t slot = (uint32_t)(serial % RING);

	atomic_store(&dynres.shown_ns[slot], shown_ns);
	atomic_store(&dynres.shown_serial[slot], serial);
}

void host_dynres_frame_done(uint64_t serial, double fraction)
{
	uint32_t slot = (uint32_t)(serial % RING);

	dynres.frame_serial[slot] = serial;
	dynres.fraction_of[slot] = fraction;
}

/* the highest step allowed: the first of at most ceiling_lines of the
target's lines (the display's: more than it shows is drawn for nothing),
two or four steps lower while the device is hot (thermalState serious,
critical) */
static uint32_t highest_step(double lines, double ceiling_lines, int thermal)
{
	uint32_t step = 0;

	while (step + 1 < HOST_DYNRES_STEPS && host_dynres_steps[step] * lines > ceiling_lines + 0.5)
		step++;
	step += thermal >= 3 ? 4 : thermal >= 2 ? 2 : 0;
	return step < HOST_DYNRES_STEPS ? step : HOST_DYNRES_STEPS - 1;
}

/* the lowest step allowed: the last of at least the floor's lines (one
player's, or split screen's), and not above the highest */
static uint32_t lowest_step(double lines, int views, uint32_t highest)
{
	double floor = views > 1 ? dynres.split_floor : dynres.floor;
	uint32_t step = 0;

	while (step + 1 < HOST_DYNRES_STEPS && host_dynres_steps[step + 1] * lines >= floor - 0.5)
		step++;
	return step > highest ? step : highest;
}

static void step_to(uint32_t step, uint64_t now_ns)
{
	if (step == dynres.step)
		return;
	if (step > dynres.step)
	{
		dynres.steps_down++;
		dynres.last_down_ns = now_ns;
	}
	else
	{
		dynres.steps_up++;
	}
	dynres.step = step;
	dynres.frames_at_step = dynres.recent_hot = dynres.clear_frames = 0;
	dynres.step_changed_ns = now_ns;
}

/* the 90th percentile of the clear frames' times (a few hundred: sorted
by insertion, once a step up is in question) */
static double clear_p90(void)
{
	uint32_t count = dynres.clear_frames < 256 ? dynres.clear_frames : 256, index, other;
	double sorted[256];

	for (index = 0; index < count; index++)
	{
		double value = dynres.clear_ms[index];

		for (other = index; other > 0 && sorted[other - 1] > value; other--)
			sorted[other] = sorted[other - 1];
		sorted[other] = value;
	}
	return count ? sorted[(count * 9) / 10] : 0.0;
}

/* a frame drawn at the current step: its GPU ms, and whether the display
showed it late */
static void frame_counted(double gpu_ms, int late, double slot_ms, uint32_t lowest, uint32_t highest, int thermal,
	uint64_t now_ns)
{
	const struct host_dynres_parameters *p = &host_dynres_defaults;
	double hot_ms = p->hot_share * slot_ms;
	int hot = gpu_ms > hot_ms;
	int down;

	dynres.frames++;
	dynres.frames_at_step++;
	dynres.gpu_ms = dynres.gpu_ms ? dynres.gpu_ms * 0.95 + gpu_ms * 0.05 : gpu_ms;
	dynres.recent_hot = (dynres.recent_hot << 1 | (uint32_t)hot) & 0xff;
	if (late)
	{
		dynres.late++;
		dynres.late_gpu += hot;
	}
	if (dynres.probing)
		dynres.probe_frames++;

	if (dynres.probing && dynres.probe_frames <= p->probe_frames)
	{
		/* (a step being tried: its first hot frame, and it goes back) */
		down = hot;
	}
	else
	{
		/* (a frame of more than VERY_HOT intervals: at once, whatever came
		before it: split screen's 4-5x spikes in combat) */
		down = gpu_ms > VERY_HOT * slot_ms || (dynres.frames_at_step >= p->down_frames &&
			((uint32_t)__builtin_popcount(dynres.recent_hot) >= p->hot_frames || (late && hot)));
	}
	if (down && dynres.step < lowest)
	{
		uint32_t step = dynres.step + 1;

		if (dynres.probing && dynres.probe_frames <= p->probe_frames)
		{
			/* a failed try: back the one step, and wait longer */
			uint32_t tried = dynres.step;

			dynres.probes_failed++;
			dynres.hold[tried] *= 2.0;
			if (dynres.hold[tried] > p->hold_most_ms)
				dynres.hold[tried] = p->hold_most_ms;
		}
		else
		{
			/* as far as the model says: the pixels' part of the late
			frame's time scaled to target_share of the interval */
			double area = host_dynres_steps[dynres.step] * host_dynres_steps[dynres.step];
			double fixed = p->fixed_share * gpu_ms;
			double wanted = area * (p->target_share * slot_ms - fixed) / (gpu_ms - fixed);

			while (step < lowest && host_dynres_steps[step] * host_dynres_steps[step] > wanted)
				step++;
		}
		dynres.probing = 0;
		step_to(step, now_ns);
		return;
	}
	if (dynres.probing && dynres.probe_frames > p->probe_frames &&
		(double)(now_ns - dynres.step_changed_ns) / 1e6 >= p->stable_ms)
	{
		dynres.hold[dynres.step] = p->hold_ms;
		dynres.probing = 0;
	}
	/* up: frames in a row clear of the interval, none late */
	if (!hot && !late)
	{
		dynres.clear_ms[dynres.clear_frames % 256] = gpu_ms;
		dynres.clear_frames++;
	}
	else
	{
		dynres.clear_frames = 0;
	}
	if (dynres.step > highest && dynres.clear_frames >= p->up_frames &&
		(!dynres.probing || dynres.probe_frames > p->probe_frames) && clear_p90() <= p->clear_share * slot_ms)
	{
		uint32_t up = dynres.step - 1;
		double hold = dynres.hold[up] * (thermal >= 2 ? 4.0 : 1.0);

		if ((double)(now_ns - dynres.last_down_ns) / 1e6 >= hold)
		{
			step_to(up, now_ns);
			dynres.probing = 1;
			dynres.probe_frames = 0;
		}
	}
}

double host_dynres_frame_fraction(uint64_t serial, int views, int thermal, uint64_t slot_ns, uint64_t now_ns,
	double lines, double ceiling_lines)
{
	uint32_t lowest, highest;
	double slot_ms = slot_ns ? slot_ns / 1e6 : 1000.0 / 60.0;

	if (dynres.answered_serial == serial && serial)
		return dynres.answer;
	dynres.answered_serial = serial;
	if (!dynres.enabled)
		return dynres.answer = 1.0;
	if (dynres.fixed > 0.0)
	{
		return dynres.answer = dynres.fixed_toggle && (serial / dynres.fixed_toggle) % 2 ? dynres.fixed_other :
			dynres.fixed;
	}
	/* one view to split screen or back: the step that case was left at
	(the frames queued at the other case's are not counted: fraction_of) */
	if ((views > 1) != dynres.split)
	{
		dynres.step_of[dynres.split] = dynres.step;
		dynres.split = views > 1;
		dynres.probing = 0;
		step_to(dynres.step_of[dynres.split], now_ns);
	}
	if (lines <= 0.0)
		lines = 2160.0;
	if (ceiling_lines <= 0.0 || ceiling_lines > lines)
		ceiling_lines = lines;
	highest = highest_step(lines, ceiling_lines, thermal);
	lowest = lowest_step(lines, views, highest);
	/* the frames done since, in order, as far as both their GPU time and
	their display time (where the display gives one) are in */
	if (!dynres.read_serial)
		dynres.read_serial = serial;
	while (dynres.read_serial < serial)
	{
		uint64_t read = dynres.read_serial;
		uint32_t slot = (uint32_t)(read % RING);
		int gpu_in = atomic_load(&dynres.gpu_serial[slot]) == read;
		int shown_in = atomic_load(&dynres.shown_serial[slot]) == read;
		uint64_t shown;
		double gpu_ms;
		int late = 0;

		/* (the display's time comes a frame or two after the GPU's; one
		far behind, or never given (no presented handler), is not waited
		for) */
		if (!gpu_in || (!shown_in && serial - read < 6))
		{
			if (serial - read < RING / 2)
				break;
			dynres.read_serial++;
			continue;
		}
		gpu_ms = atomic_load(&dynres.gpu_ns[slot]) / 1e6;
		shown = shown_in ? atomic_load(&dynres.shown_ns[slot]) : 0;
		if (shown)
		{
			late = dynres.previous_shown && (double)(shown - dynres.previous_shown) / 1e6 >= LATE_SLOTS * slot_ms;
			dynres.previous_shown = shown;
		}
		if (dynres.frame_serial[slot] == read && dynres.fraction_of[slot] >= 0.0 &&
			fabs(dynres.fraction_of[slot] - host_dynres_steps[dynres.step]) < 1e-9)
		{
			frame_counted(gpu_ms, late, slot_ms, lowest, highest, thermal, now_ns);
		}
		else if (dynres.frame_serial[slot] != read || dynres.fraction_of[slot] < 0.0)
		{
			/* (no scene: a menu, a load; the display's gap after it
			is not the scene's) */
			dynres.previous_shown = 0;
		}
		dynres.read_serial++;
	}
	if (dynres.step > lowest)
		step_to(lowest, now_ns);
	else if (dynres.step < highest)
		step_to(highest, now_ns);
	return dynres.answer = host_dynres_steps[dynres.step];
}

void host_dynres_status(struct host_dynres_status *status)
{
	memset(status, 0, sizeof(*status));
	status->fraction = !dynres.enabled ? 1.0 : dynres.fixed > 0.0 ? dynres.answer : host_dynres_steps[dynres.step];
	status->step = dynres.step;
	status->steps_down = dynres.steps_down;
	status->steps_up = dynres.steps_up;
	status->probes_failed = dynres.probes_failed;
	status->frames = dynres.frames;
	status->late = dynres.late;
	status->late_gpu = dynres.late_gpu;
	status->hold_ms = dynres.step ? dynres.hold[dynres.step - 1] : 0.0;
	status->gpu_ms = dynres.gpu_ms;
}
