"""Dynamic resolution (port/macos/host/host_dynres.h; port/tvos/PERFORMANCE.md,
"Dynamic resolution (A12)"): the controller run by itself against a model of
the A12, and the plumbing's invariants read from the sources."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "port/macos/host"
GUEST = ROOT / "port/macos/guest"

# A model of the A12 at 60 Hz: a frame's GPU time is a fixed part plus a part
# in proportion to the pixels, at the full clock; below 60 fps the GPU runs at
# its full clock, at 60 it slows its clock to fill ~88% of the interval (the
# measured 14-15 ms at any resolution that holds 60). A frame is shown late
# when its full-clock time is past the interval. `heat` multiplies the cost
# over time (the measured 12-15% over ~25 min).
HARNESS = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "host_dynres.h"

static const double SLOT = 1e9 / 60.0;
static unsigned rng = 12345;
static double noise(void) { rng = rng * 1103515245u + 12345u; return ((rng >> 8) & 0xffff) / 65535.0 - 0.5; }

struct run { double mean_fraction, least_fraction; long late, frames, changes; double seconds_at[15]; };

/* fixed_ms and pixel_ms at the full fraction and clock; heat(t) multiplies */
static struct run simulate(double fixed_ms, double pixel_ms, double minutes, double heat_per_minute, int views,
	double spike_every_s, double spike_ms)
{
	struct run r = { 0, 1.0, 0, 0, 0, { 0 } };
	uint64_t now = 1000000000ull, serial;
	uint64_t frames = (uint64_t)(minutes * 60.0 * 60.0);
	double last = -1.0, sum = 0.0;
	uint64_t shown = now;

	host_dynres_reset();
	host_dynres_configure(1, 1440.0, 1080.0);
	for (serial = 2; serial < frames; serial++)
	{
		double f = host_dynres_frame_fraction(serial, views, 0, (uint64_t)SLOT, now, 2160.0, 2160.0);
		double heat = 1.0 + heat_per_minute * (now / 6e10);
		double cost = (fixed_ms + pixel_ms * f * f) * heat * (1.0 + 0.04 * noise());
		double busy;
		int late;

		if (spike_every_s > 0 && serial % (uint64_t)(spike_every_s * 60.0) == 0)
			cost += spike_ms;
		late = cost > SLOT / 1e6;
		/* (with room, the clock fills ~88% of the interval, and 6% of the
		frames come out past 97%: the device's noise, PERFORMANCE.md) */
		busy = late ? cost : SLOT / 1e6 * (noise() > 0.44 ? 0.975 : 0.88 * (1.0 + 0.02 * noise()));
		now += late ? (uint64_t)(2 * SLOT) : (uint64_t)SLOT;
		shown = now;
		host_dynres_frame_done(serial, f);
		host_dynres_gpu(serial, (uint64_t)(busy * 1e6));
		host_dynres_shown(serial, shown);
		r.frames++;
		r.late += late;
		sum += f;
		if (f < r.least_fraction)
			r.least_fraction = f;
		if (last >= 0 && f != last)
			r.changes++;
		last = f;
		for (int i = 0; i < 15; i++)
			if (fabs(host_dynres_steps[i] - f) < 1e-9)
				r.seconds_at[i] += 1.0 / 60.0;
	}
	r.mean_fraction = sum / r.frames;
	return r;
}

static void print(const char *name, struct run r)
{
	printf("%s mean %.0f least %.0f late %ld of %ld (%.3f%%) changes %ld\n", name, r.mean_fraction * 2160.0,
		r.least_fraction * 2160.0, r.late, r.frames, 100.0 * r.late / r.frames, r.changes);
}

int main(void)
{
	struct run r;
	struct host_dynres_status status;

	/* off: always the whole */
	host_dynres_reset();
	host_dynres_configure(0, 1440.0, 1080.0);
	assert(host_dynres_frame_fraction(5, 1, 0, (uint64_t)SLOT, 1, 2160.0, 2160.0) == 1.0);

	/* fixed (HALO_DYNRES_FIXED), and a toggle */
	host_dynres_reset();
	host_dynres_configure(1, 1440.0, 1080.0);
	host_dynres_fixed(0.75, 30, 1.0);
	assert(host_dynres_frame_fraction(10, 1, 0, (uint64_t)SLOT, 1, 2160.0, 2160.0) == 0.75);
	assert(host_dynres_frame_fraction(40, 1, 0, (uint64_t)SLOT, 1, 2160.0, 2160.0) == 1.0);

	/* the ceiling: a 1080p display draws 2160 lines' scenes at 1080 */
	host_dynres_reset();
	host_dynres_configure(1, 1440.0, 1080.0);
	assert(fabs(host_dynres_frame_fraction(10, 1, 0, (uint64_t)SLOT, 1, 2160.0, 1080.0) - 0.5) < 1e-9);
	/* ... and the floor never above it */
	assert(fabs(host_dynres_frame_fraction(11, 2, 0, (uint64_t)SLOT, 2, 2160.0, 1080.0) - 0.5) < 1e-9);
	/* the thermal state lowers it (serious: two steps) */
	host_dynres_reset();
	host_dynres_configure(1, 1440.0, 1080.0);
	assert(fabs(host_dynres_frame_fraction(10, 1, 2, (uint64_t)SLOT, 1, 2160.0, 2160.0) - 1872.0 / 2160.0) < 1e-9);

	/* a light scene (cryo: 13.5 ms at 2160) with the device's 5% hot noise:
	stays at 2160 */
	r = simulate(3.0, 10.5, 3.0, 0.0, 1, 0, 0);
	print("light", r);
	assert(r.least_fraction == 1.0 && r.late == 0);

	/* b30 (~19.9 ms at 2160, A12 cold): goes down at once, settles, late
	frames only in the first moment */
	r = simulate(4.0, 15.9, 5.0, 0.0, 1, 0, 0);
	print("b30", r);
	assert(r.late < 20);
	assert(r.least_fraction >= 1440.0 / 2160.0 - 1e-9);
	assert(r.mean_fraction > 1600.0 / 2160.0);
	/* no shimmer: a handful of changes in five minutes (failed tries back
	off: 4, 8, 16 ... s) */
	assert(r.changes < 40);

	/* b30 heating up 15% over 25 minutes: still no run of late frames, and
	the fraction follows the heat down */
	r = simulate(4.0, 15.9, 25.0, 0.006, 1, 0, 0);
	print("b30 hot", r);
	assert(r.late < 60 && 100.0 * r.late / r.frames < 0.1);
	host_dynres_status(&status);
	printf("  status: %.0f lines, %llu down %llu up, %llu tries failed\n", status.fraction * 2160.0,
		(unsigned long long)status.steps_down, (unsigned long long)status.steps_up,
		(unsigned long long)status.probes_failed);

	/* a CPU spike (a load, 80 ms every 20 s) is not the GPU's: no step down */
	r = simulate(3.0, 10.5, 3.0, 0.0, 1, 20.0, 0.0);
	assert(r.least_fraction == 1.0);

	/* split screen: two views of b30 (measured in play on the A12: ~26 ms
	of GPU at 2160, stuck at 30 Hz): starts at 1620, its own floor */
	r = simulate(7.0, 19.0, 5.0, 0.0, 2, 0, 0);
	print("split", r);
	assert(r.least_fraction >= 1080.0 / 2160.0 - 1e-9 && r.late < 10);

	/* combat: transparents doubling the scene's cost for 3 s every 20 s
	(the A12's 60 -> 140-199 transparent draws): down within a second */
	{
		uint64_t now = 1000000000ull, serial, late = 0, late_run = 0, worst_run = 0;

		host_dynres_reset();
		host_dynres_configure(1, 1440.0, 1080.0);
		for (serial = 2; serial < 60 * 120; serial++)
		{
			double f = host_dynres_frame_fraction(serial, 1, 0, (uint64_t)SLOT, now, 2160.0, 2160.0);
			int combat = (serial / 60) % 20 < 3;
			double cost = (3.0 + (combat ? 18.0 : 10.0) * f * f) * (1.0 + 0.04 * noise());
			int is_late = cost > SLOT / 1e6;

			now += is_late ? (uint64_t)(2 * SLOT) : (uint64_t)SLOT;
			host_dynres_frame_done(serial, f);
			host_dynres_gpu(serial, (uint64_t)((is_late ? cost : SLOT / 1e6 * (noise() > 0.44 ? 0.975 : 0.88)) * 1e6));
			host_dynres_shown(serial, now);
			late += is_late;
			late_run = is_late ? late_run + 1 : 0;
			if (late_run > worst_run)
				worst_run = late_run;
		}
		printf("combat late %llu, longest run %llu\n", (unsigned long long)late, (unsigned long long)worst_run);
		assert(worst_run <= 6);
	}
	puts("ok");
	return 0;
}
'''


def test_controller_against_a_model_of_the_a12(tmp_path):
    harness = tmp_path / "harness.c"
    harness.write_text(HARNESS)
    program = tmp_path / "harness"
    subprocess.run(["clang", "-std=gnu11", "-O1", "-Wall", "-Werror", f"-I{HOST}", str(harness),
                    str(HOST / "host_dynres.c"), "-o", str(program)], check=True)
    result = subprocess.run([str(program)], capture_output=True, text=True)
    print(result.stdout)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.endswith("ok\n")


def test_controller_never_waits_on_the_gpu():
    source = (HOST / "host_dynres.c").read_text()
    assert "waitUntilCompleted" not in source and "pthread_mutex" not in source
    metal = (HOST / "host_metal.m").read_text()
    # its GPU times come from the completed handler, its display times from the presented handler
    handler = metal[metal.index("[commands addCompletedHandler:^(id<MTLCommandBuffer> buffer)"):]
    assert "host_dynres_gpu(serial," in handler
    assert "host_dynres_shown(serial, shown_ns)" in metal


def test_scale_one_draws_as_before():
    """at a fraction of 1 the scene is not redirected: nothing changes"""
    guest = (GUEST / "d3d8_metal.c").read_text()
    begin = guest[guest.index("void halo_screen_scene_begin("):guest.index("void halo_screen_anti_alias(")]
    assert "if (scene.frame_fraction == 1.0f || scene.frame_fraction <= 0.0f)\n\t\treturn;" in begin
    assert begin.index("return;\n\t/* the scene target") > 0
    # every redirect is behind scene.active
    assert "if (scene.active && surface->Data == device.back_buffer.Data)" in guest
    assert "if (scene.active && data == device.back_buffer.Data)" in guest


def test_frame_log_records_the_render_scale():
    source = (HOST / "host_frame_log.c").read_text()
    assert "hoisted_ms,hoisted_buffers,render_scale\\n" in source
    telemetry = (HOST / "host_telemetry.c").read_text()
    assert '\\"render_scale\\":%.4f' in telemetry and '\\"render_scale_min\\":%.4f' in telemetry


def test_settings_and_tv_default():
    config = (ROOT / "port/linux/src/port_config.c").read_text()
    assert '{ "display.dynamic_resolution", _config_string, "\\"auto\\""' in config
    assert '"display.dynamic_resolution_floor", _config_integer, "1440"' in config
    assert '"display.dynamic_resolution_split_floor", _config_integer, "1080"' in config
    metal = (HOST / "host_metal.m").read_text()
    settings = metal[metal.index("void host_dynres_settings("):metal.index("float host_dynres_fraction(")]
    assert "#ifdef HALO_TVOS\n\t\tenabled = 1;\n#else\n\t\tenabled = 0;" in settings


def test_render_hook_wraps_the_3d_view_only():
    render = (ROOT / "source/render/render.c").read_text()
    # (the definitions: each is declared ahead too)
    start = render.index("static void render_window(", render.index("static void render_window(") + 1)
    window = render[start:render.index("static void render_player_frame(", start)]
    begin = window.index("halo_screen_scene_begin(")
    assert begin < window.index("rasterizer_window_begin(&parameters);")
    assert window.index("halo_screen_anti_alias(") < window.index("interface_draw_screen();")
