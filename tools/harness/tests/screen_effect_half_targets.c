/*
SCREEN_EFFECT_HALF_TARGETS.C (test)

The real rasterizer_screen_effect_half_target (rasterizer_xbox_screen_effect.c):
with display.half_res_convolution, which target each pass of a convolution
reads and draws into. The loop picks the screen (0) and its copy (7) in
turn, as the Xbox did; the half-resolution pair stands in for them. Every
pass must read what the pass before it drew, the first must read the screen
at full resolution, the last must draw into the screen, and no pass may read
the target it draws into. test_screen_effect_half_targets.py takes the code
(under_test.inc).
*/

#include "harness.h"

#define HALO_METAL
enum
{
	_rasterizer_target_render_primary = 0,
	_rasterizer_target_screen_effect = 7
};

#include "under_test.inc"

/* where a pass's image is: the screen (SCREEN), the copy (COPY), or the
pair's 0 or 1 */
enum { SCREEN = 100, COPY = 107 };

static int place(short target, boolean destination, short pass, short pass_count)
{
	short half = rasterizer_screen_effect_half_target(target, destination, pass, pass_count);

	if (half != NONE)
		return half;
	return target == _rasterizer_target_render_primary ? SCREEN : COPY;
}

int main(int argc, char **argv)
{
	const char *case_name = argc > 1 ? argv[1] : "";
	short pass_count;

	/* the loop's targets, as rasterizer_xbox_screen_effect.c picks them */
#define SOURCE(pass) ((pass) & 1 ? _rasterizer_target_screen_effect : _rasterizer_target_render_primary)
#define DESTINATION(pass) ((pass) & 1 ? _rasterizer_target_render_primary : _rasterizer_target_screen_effect)

	/* each pass reads what the one before drew; the first reads the
	screen, the last draws into it; none reads what it draws into */
	CASE("chain")
	{
		for (pass_count = 2; pass_count <= 12; pass_count += 2)
		{
			short pass;
			int previous = SCREEN;

			for (pass = 0; pass < pass_count; pass++)
			{
				int source = place(SOURCE(pass), FALSE, pass, pass_count);
				int destination = place(DESTINATION(pass), TRUE, pass, pass_count);

				CHECK(source == previous, "pass %d of %d reads %d, the one before drew %d", pass, pass_count,
					source, previous);
				CHECK(source != destination, "pass %d of %d reads and draws %d", pass, pass_count, source);
				previous = destination;
			}
			CHECK(previous == SCREEN, "%d passes end in %d, not the screen", pass_count, previous);
		}
		return 0;
	}
	/* only the screen at the two ends: the passes between are all at half
	resolution, the copy never used */
	CASE("all-between-half")
	{
		for (pass_count = 2; pass_count <= 12; pass_count += 2)
		{
			short pass;

			for (pass = 0; pass < pass_count; pass++)
			{
				int source = place(SOURCE(pass), FALSE, pass, pass_count);
				int destination = place(DESTINATION(pass), TRUE, pass, pass_count);

				CHECK(source != COPY && destination != COPY, "pass %d of %d uses the full-resolution copy",
					pass, pass_count);
				CHECK(pass == 0 || source != SCREEN, "pass %d of %d reads the screen", pass, pass_count);
				CHECK(pass == pass_count - 1 || destination != SCREEN, "pass %d of %d draws the screen",
					pass, pass_count);
			}
		}
		return 0;
	}
	fprintf(stderr, "unknown case '%s'\n", case_name);
	return 2;
}
