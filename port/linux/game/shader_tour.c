/*
SHADER_TOUR.C

A scripted visit of a map that draws as much of what it can draw as
possible, so that the renderer makes (and the Metal host's shader cache
records) every shader variant the map uses: the bundled shader pack's
generator (tools/shader_pack.py) runs it once per campaign map, on the Mac
(debug.shader_tour, HALO_SHADER_TOUR; a diagnostic, off by default, never
on in play).

Once the map is playing (after its first cinematic, which is skipped) the
tour, for each of the map's structure BSPs:
- puts the camera (a scripted camera, as camera_control does) in every
  cluster in turn, at the middle of each of its subclusters' bounds, looking
  six ways, a game tick each (the game sped up);
- makes in front of the camera (at the first cluster's first point) every object
  the map has a definition of (bipeds, vehicles, weapons, equipment,
  scenery, machines, ... as cheat_all_weapons does with one), a few at a
  time, each drawn for some frames and deleted again, and the same for every
  effect (effect_new's), so that their shaders, lit and in fog, are made;
- with the screen effects on (the convolution, the filter, the video
  effect, a fade), so that their passes are made too;
- then plays: the player's own first-person view, every weapon of the map
  in turn in its hands, while the scripted input (debug.test_input "bot")
  fires, throws grenades and jumps: the weapons' first-person models,
  muzzle flashes, projectiles, impacts and decals as play draws them, with
  camouflage, the flashlight and the shield's damage for the second half.
Then it quits the game (debug.exit_after should be set as a limit too).

Every step is logged ("shader tour: ..."), and the host's frame log gets an
event, so the generator can see how far a run got. Nothing here runs unless
the setting is on.

Called from the main loop every frame (main.c), as network_test_update.
*/

#include "cseries.h"
#include "main/main.h"
#include "game/game.h"
#include "game/cheats.h"
#include "game/players.h"
#include "units/units.h"
#include "items/weapon_definitions.h"
#include "objects/objects.h"
#include "objects/object_definitions.h"
#include "effects/effects.h"
#include "effects/effect_definitions.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "structures/structure_bsp_definitions.h"
#include "camera/camera_scripting.h"
#include "camera/director.h"
#include "cutscene/cinematics.h"
#include "cache/cache_files.h"
#include "hs/hs.h"
#include "tag_files/tag_groups.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the platform layer's (port/linux/src/port_config.c, xbox_kernel.c) */
const char *config_string(char const *name);
void platform_log(char const *format, ...);

/* (structure_visibility.c's layout of a cluster and a subcluster) */
struct shader_tour_cluster
{
	short sky_index;
	short fog_designator;
	short background_sound_palette_index;
	short sound_environment_palette_index;
	short weather_palette_index;
	short transition_structure_bsp_index;
	short first_runtime_decal_index;
	word runtime_decal_count;
	long unused[6];
	struct tag_block predicted_resources;
	struct tag_block subclusters;
	word first_lens_flare_marker_index;
	word lens_flare_marker_count;
	struct tag_block surface_indices;
	struct tag_block mirrors;
	struct tag_block portal_indices;
};

struct shader_tour_subcluster
{
	real_rectangle3d world_bounds;
	struct tag_block surface_indices;
};

/* ---------- constants */

/* game ticks each view is drawn: what is drawn between ticks is the camera
of the last ticks (render_interpolation.c), so a view needs a tick; the
game runs at TOUR_GAME_SPEED to make them come sooner */
#define TICKS_PER_VIEW 1
#define VIEWS_PER_POINT 6
/* points a cluster at most (the middles of its subclusters, spread) */
#define POINTS_PER_CLUSTER 3
/* objects made at once, in a row in front of the camera, and the ticks
they are drawn */
#define OBJECTS_AT_ONCE 6
#define TICKS_PER_OBJECT_BATCH 4
#define EFFECTS_AT_ONCE 4
#define TICKS_PER_EFFECT_BATCH 12
/* each weapon's time in the player's hands (the scripted input fires every
three seconds), at the game's own speed */
#define TICKS_PER_WEAPON (TICKS_PER_SECOND * 7 / 2)
/* ticks waited after a BSP switch, a cinematic's end and the start */
#define SETTLE_TICKS 20
#define TOUR_GAME_SPEED "3"
/* at most this many frames' wait for the game to be playable */
#define START_TIMEOUT_FRAMES (60 * 60 * 3)

enum
{
	_tour_off,
	_tour_waiting,
	_tour_bsp,
	_tour_views,
	_tour_objects,
	_tour_effects,
	_tour_screen_effects,
	_tour_play,
	_tour_done,
};

/* the object groups made (the leaves of 'obje': the iterator matches a
tag's own group and its parent's, not its grandparent's) */
static const long tour_object_groups[] =
{
	'bipd', 'vehi', 'weap', 'eqip', 'garb', 'proj', 'scen', 'mach', 'ctrl', 'lifi', 'plac', 'ssce',
};

static struct
{
	int state;
	boolean checked;
	long frames;
	/* the game tick that ends the current step (or 0) */
	long wait_until;
	short bsp_index;
	short cluster_index;
	short point_index;
	short view_index;
	short point_count;
	real_point3d points[POINTS_PER_CLUSTER];
	/* the object groups and tags made so far */
	short group_index;
	struct tag_iterator iterator;
	boolean iterator_started;
	long objects[OBJECTS_AT_ONCE];
	long effects[EFFECTS_AT_ONCE];
	long objects_made, effects_made, views_drawn;
	/* where the objects and effects are made: the first cluster's point */
	real_point3d stage_point;
	boolean stage_set;
	short screen_effect_step;
	/* the play's weapons so far, and how many the map has */
	short weapon_index, weapon_count;
} shader_tour;

/* ---------- private code */

static void shader_tour_event(const char *detail)
{
	platform_log("shader tour: %s (map %s, frame %ld)", detail, main_get_map_name(), shader_tour.frames);
}

static void shader_tour_command(const char *command)
{
	char text[256];

	/* (hs_compile_and_evaluate wants a writable copy) */
	snprintf(text, sizeof(text), "%s", command);
	hs_compile_and_evaluate(text);
}

/* the camera at a point, looking the view's way: the four sides, up and
down (a little off, so that the up vector is not along it) */
static void shader_tour_look(real_point3d const *point, short view)
{
	static const real directions[VIEWS_PER_POINT][3] =
	{
		{ 1.f, 0.f, -0.1f }, { 0.f, 1.f, -0.1f }, { -1.f, 0.f, -0.1f }, { 0.f, -1.f, -0.1f },
		{ 0.2f, 0.f, -1.f }, { 0.2f, 0.f, 0.6f },
	};
	real_vector3d forward, up;
	real length;

	forward.i = directions[view][0];
	forward.j = directions[view][1];
	forward.k = directions[view][2];
	length = (real)sqrt(forward.i * forward.i + forward.j * forward.j + forward.k * forward.k);
	forward.i /= length;
	forward.j /= length;
	forward.k /= length;
	/* (perpendicular to forward, as near the world's up as it can be) */
	up.i = -forward.k * forward.i;
	up.j = -forward.k * forward.j;
	up.k = forward.i * forward.i + forward.j * forward.j;
	length = (real)sqrt(up.i * up.i + up.j * up.j + up.k * up.k);
	up.i /= length;
	up.j /= length;
	up.k /= length;
	scripted_camera_set_camera_point_relative(point, &forward, &up, 0.f, 0, NONE);
}

/* the points of a cluster of the current BSP: its subclusters' middles */
static void shader_tour_cluster_points(struct structure_bsp *structure, short cluster_index)
{
	struct shader_tour_cluster *cluster = TAG_BLOCK_GET_ELEMENT(&structure->clusters, cluster_index,
		struct shader_tour_cluster);
	long subcluster_count = cluster->subclusters.count, index, step;

	shader_tour.point_count = 0;
	if (subcluster_count <= 0 || !cluster->subclusters.address)
		return;
	/* (no more than POINTS_PER_CLUSTER of them, spread over the cluster) */
	step = subcluster_count > POINTS_PER_CLUSTER ? (subcluster_count + POINTS_PER_CLUSTER - 1) / POINTS_PER_CLUSTER : 1;
	for (index = 0; index < subcluster_count && shader_tour.point_count < POINTS_PER_CLUSTER; index += step)
	{
		struct shader_tour_subcluster *subcluster = TAG_BLOCK_GET_ELEMENT(&cluster->subclusters, (short)index,
			struct shader_tour_subcluster);
		real_point3d *point = &shader_tour.points[shader_tour.point_count];

		point->x = (subcluster->world_bounds.x0 + subcluster->world_bounds.x1) * 0.5f;
		point->y = (subcluster->world_bounds.y0 + subcluster->world_bounds.y1) * 0.5f;
		point->z = (subcluster->world_bounds.z0 + subcluster->world_bounds.z1) * 0.5f;
		/* (the flying camera's limits) */
		if (fabs(point->x) < 4000.f && fabs(point->y) < 4000.f && fabs(point->z) < 4000.f)
			shader_tour.point_count++;
	}
}

static void shader_tour_delete_made(void)
{
	short index;

	for (index = 0; index < OBJECTS_AT_ONCE; index++)
	{
		if (shader_tour.objects[index] != NONE && object_try_and_get(shader_tour.objects[index]))
			object_delete(shader_tour.objects[index]);
		shader_tour.objects[index] = NONE;
	}
	for (index = 0; index < EFFECTS_AT_ONCE; index++)
	{
		if (shader_tour.effects[index] != NONE)
			effect_delete(shader_tour.effects[index]);
		shader_tour.effects[index] = NONE;
	}
}

/* the next tag of the object groups, or NONE when there are none more */
static long shader_tour_next_object_tag(void)
{
	while (shader_tour.group_index < (short)NUMBEROF(tour_object_groups))
	{
		long tag_index;

		if (!shader_tour.iterator_started)
		{
			tag_iterator_new(&shader_tour.iterator, tour_object_groups[shader_tour.group_index]);
			shader_tour.iterator_started = TRUE;
		}
		tag_index = tag_iterator_next(&shader_tour.iterator);
		if (tag_index != NONE)
			return tag_index;
		shader_tour.group_index++;
		shader_tour.iterator_started = FALSE;
	}
	return NONE;
}

/* a row of objects (or effects) in front of the stage's camera */
static void shader_tour_stage_position(short slot, short count, real_point3d *position)
{
	*position = shader_tour.stage_point;
	position->x += 2.0f;
	position->y += ((real)slot - (real)(count - 1) * 0.5f) * 1.2f;
}

static void shader_tour_make_objects(void)
{
	short slot;

	for (slot = 0; slot < OBJECTS_AT_ONCE; slot++)
	{
		long tag_index = shader_tour_next_object_tag();
		struct object_placement_data data;

		if (tag_index == NONE)
			break;
		object_placement_data_new(&data, tag_index, NONE);
		shader_tour_stage_position(slot, OBJECTS_AT_ONCE, &data.position);
		data.forward.i = -1.f;
		data.forward.j = 0.f;
		data.forward.k = 0.f;
		data.up.i = 0.f;
		data.up.j = 0.f;
		data.up.k = 1.f;
		shader_tour.objects[slot] = object_new(&data);
		shader_tour.objects_made += shader_tour.objects[slot] != NONE;
	}
}

static boolean shader_tour_make_effects(void)
{
	short slot;
	boolean any = FALSE;

	for (slot = 0; slot < EFFECTS_AT_ONCE; slot++)
	{
		long tag_index = tag_iterator_next(&shader_tour.iterator);
		real_point3d position;
		real_vector3d forward = { { 0.f, 0.f, 1.f } };

		if (tag_index == NONE)
			break;
		any = TRUE;
		shader_tour_stage_position(slot, EFFECTS_AT_ONCE, &position);
		shader_tour.effects[slot] = effect_new_unattached_from_markers(tag_index, NONE, global_zero_vector3d, 1,
			NULL, &position, &forward, 1.f, 1.f, NULL, NULL, TRUE);
		shader_tour.effects_made += shader_tour.effects[slot] != NONE;
	}
	return any;
}

/* the camera at the stage, looking at the row */
static void shader_tour_look_at_stage(void)
{
	shader_tour_look(&shader_tour.stage_point, 0);
}

static boolean shader_tour_bsp_start(void)
{
	struct scenario *scenario = global_scenario_get();
	char text[96];

	if (shader_tour.bsp_index >= scenario->structure_bsp_references.count)
		return FALSE;
	if (shader_tour.bsp_index != global_structure_bsp_index)
		main_switch_structure_bsp(shader_tour.bsp_index);
	snprintf(text, sizeof(text), "bsp %d of %ld", shader_tour.bsp_index,
		(long)scenario->structure_bsp_references.count);
	shader_tour_event(text);
	shader_tour.cluster_index = 0;
	shader_tour.point_index = 0;
	shader_tour.view_index = 0;
	shader_tour.point_count = 0;
	shader_tour.wait_until = game_time_get() + SETTLE_TICKS;
	return TRUE;
}

static void shader_tour_finish(void)
{
	char text[160];

	shader_tour_delete_made();
	snprintf(text, sizeof(text), "done: %ld views, %ld objects, %ld effects", shader_tour.views_drawn,
		shader_tour.objects_made, shader_tour.effects_made);
	shader_tour_event(text);
	shader_tour.state = _tour_done;
	/* (the host's atexit writes its logs) */
	exit(0);
}

/* the local player's unit, or NONE */
static long shader_tour_player_unit(void)
{
	long player_index = local_player_get_player_index(0);
	struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;

	return player && player->unit_index != NONE && object_try_and_get(player->unit_index) ? player->unit_index : NONE;
}

/* the map's weapon tag of this number, or NONE */
static long shader_tour_weapon_tag(short number)
{
	struct tag_iterator iterator;
	long tag_index;
	short index = 0;

	tag_iterator_new(&iterator, WEAPON_DEFINITION_TAG);
	while ((tag_index = tag_iterator_next(&iterator)) != NONE)
	{
		if (index++ == number)
			return tag_index;
	}
	return NONE;
}

/* the next weapon in the player's hands (made at its feet and picked up,
the others dropped); FALSE when there are none more */
static boolean shader_tour_next_weapon(void)
{
	long unit_index = shader_tour_player_unit();
	long tag_index;
	struct object_placement_data data;
	long weapon_index;

	if (unit_index == NONE)
		return FALSE;
	for (;;)
	{
		tag_index = shader_tour_weapon_tag(shader_tour.weapon_index++);
		if (tag_index == NONE)
			return FALSE;
		object_placement_data_new(&data, tag_index, NONE);
		object_get_origin(unit_index, &data.position);
		data.position.z += 0.3f;
		weapon_index = object_new(&data);
		if (weapon_index == NONE)
			continue;
		if (unit_add_weapon_to_inventory(unit_index, weapon_index, _unit_add_weapon_replace))
			return TRUE;
		object_delete(weapon_index);
	}
}

/* ---------- public code */

void shader_tour_update(
	boolean main_menu_loaded)
{
	if (!shader_tour.checked)
	{
		const char *setting = config_string("debug.shader_tour");

		shader_tour.checked = TRUE;
		shader_tour.state = setting && *setting && strcmp(setting, "off") && strcmp(setting, "0") &&
			strcmp(setting, "false") ? _tour_waiting : _tour_off;
		if (shader_tour.state != _tour_off)
		{
			short index;

			for (index = 0; index < OBJECTS_AT_ONCE; index++)
				shader_tour.objects[index] = NONE;
			for (index = 0; index < EFFECTS_AT_ONCE; index++)
				shader_tour.effects[index] = NONE;
			platform_log("shader tour: on (debug.shader_tour)");
		}
	}
	if (shader_tour.state == _tour_off || shader_tour.state == _tour_done)
		return;
	shader_tour.frames++;
	if (main_menu_loaded || !game_in_progress() || !game_time_initialized())
	{
		if (shader_tour.frames > START_TIMEOUT_FRAMES)
		{
			shader_tour_event("the map never started");
			exit(1);
		}
		return;
	}
	/* (the player cannot die, nor the scripts move the camera) */
	cheat.deathless_player = TRUE;
	if (cinematic_in_progress())
	{
		/* (a cinematic's camera and letterbox: what the map's scripts
		show first; one frame of it drawn, then skipped) */
		cinematic_stop();
		shader_tour.wait_until = game_time_get() + SETTLE_TICKS;
	}
	if (game_time_get() < shader_tour.wait_until)
		return;

	switch (shader_tour.state)
	{
	case _tour_waiting:
		shader_tour_event("start");
		shader_tour_command("(ai_erase_all)");
		shader_tour_command("(game_speed " TOUR_GAME_SPEED ")");
		director_script_camera(TRUE);
		shader_tour.bsp_index = 0;
		shader_tour.state = _tour_bsp;
		if (!shader_tour_bsp_start())
			shader_tour_finish();
		break;

	case _tour_bsp:
	{
		struct structure_bsp *structure = global_structure_bsp_get();

		/* (scripts may switch the BSP back: the tour's own wins) */
		if (global_structure_bsp_index != shader_tour.bsp_index)
		{
			main_switch_structure_bsp(shader_tour.bsp_index);
			shader_tour.wait_until = game_time_get() + SETTLE_TICKS;
			break;
		}
		director_script_camera(TRUE);
		if (shader_tour.cluster_index >= structure->clusters.count)
		{
			/* the objects and effects in the first BSP only: they are
			the map's, not the BSP's */
			if (shader_tour.bsp_index == 0 && shader_tour.stage_set)
			{
				shader_tour_event("objects");
				shader_tour.group_index = 0;
				shader_tour.iterator_started = FALSE;
				shader_tour.state = _tour_objects;
				break;
			}
			shader_tour.bsp_index++;
			if (!shader_tour_bsp_start())
			{
				shader_tour.state = _tour_screen_effects;
				shader_tour.screen_effect_step = 0;
			}
			break;
		}
		shader_tour_cluster_points(structure, shader_tour.cluster_index);
		shader_tour.point_index = 0;
		shader_tour.view_index = 0;
		if (!shader_tour.point_count)
		{
			shader_tour.cluster_index++;
			break;
		}
		if (!shader_tour.stage_set && shader_tour.bsp_index == 0)
		{
			shader_tour.stage_point = shader_tour.points[0];
			shader_tour.stage_set = TRUE;
		}
		shader_tour.state = _tour_views;
		/* (falls into the first view) */
	}

	case _tour_views:
		if (shader_tour.view_index >= VIEWS_PER_POINT)
		{
			shader_tour.view_index = 0;
			shader_tour.point_index++;
		}
		if (shader_tour.point_index >= shader_tour.point_count)
		{
			shader_tour.cluster_index++;
			shader_tour.state = _tour_bsp;
			break;
		}
		shader_tour_look(&shader_tour.points[shader_tour.point_index], shader_tour.view_index);
		shader_tour.view_index++;
		shader_tour.views_drawn++;
		shader_tour.wait_until = game_time_get() + TICKS_PER_VIEW;
		break;

	case _tour_objects:
		shader_tour_delete_made();
		if (shader_tour.group_index >= (short)NUMBEROF(tour_object_groups))
		{
			shader_tour_event("effects");
			tag_iterator_new(&shader_tour.iterator, EFFECT_DEFINITION_TAG);
			shader_tour.state = _tour_effects;
			break;
		}
		shader_tour_look_at_stage();
		shader_tour_make_objects();
		shader_tour.wait_until = game_time_get() + TICKS_PER_OBJECT_BATCH;
		break;

	case _tour_effects:
		shader_tour_delete_made();
		shader_tour_look_at_stage();
		if (!shader_tour_make_effects())
		{
			shader_tour.bsp_index++;
			shader_tour.state = _tour_bsp;
			if (!shader_tour_bsp_start())
			{
				shader_tour.state = _tour_screen_effects;
				shader_tour.screen_effect_step = 0;
			}
			break;
		}
		shader_tour.wait_until = game_time_get() + TICKS_PER_EFFECT_BATCH;
		break;

	case _tour_screen_effects:
	{
		/* the screen effects' passes, each drawn for a while: the
		convolution (blur), the filter (night vision's and the
		desaturation), the video effect and a fade */
		static const char *const steps[] =
		{
			"(cinematic_screen_effect_start true)",
			"(cinematic_screen_effect_set_convolution 2 2 1 2 1)",
			"(cinematic_screen_effect_set_filter 1 1 1 1 true 1)",
			"(cinematic_screen_effect_set_filter_desaturation_tint 1 0.5 0.5)",
			"(cinematic_screen_effect_set_video 2 1)",
			"(cinematic_screen_effect_stop)",
			"(fade_out 1 1 1 15)",
			"(fade_in 1 1 1 15)",
		};

		if (shader_tour.screen_effect_step == 0)
		{
			shader_tour_event("screen effects");
			shader_tour_look_at_stage();
		}
		if (shader_tour.screen_effect_step >= (short)NUMBEROF(steps))
		{
			struct tag_iterator iterator;

			/* the player's own view, at the game's speed, the scripted
			input playing */
			shader_tour_event("play");
			director_script_camera(FALSE);
			shader_tour_command("(game_speed 1)");
			shader_tour_command("(player_enable_input true)");
			shader_tour.weapon_count = 0;
			tag_iterator_new(&iterator, WEAPON_DEFINITION_TAG);
			while (tag_iterator_next(&iterator) != NONE)
				shader_tour.weapon_count++;
			shader_tour.weapon_index = 0;
			shader_tour.state = _tour_play;
			break;
		}
		shader_tour_command(steps[shader_tour.screen_effect_step++]);
		shader_tour.wait_until = game_time_get() + 10;
		break;
	}

	case _tour_play:
	{
		long unit_index;

		/* (the second half with camouflage, the flashlight and the
		shield's damage) */
		if (shader_tour.weapon_index == (shader_tour.weapon_count + 1) / 2 &&
			(unit_index = shader_tour_player_unit()) != NONE)
		{
			cheat_active_camouflage();
			unit_set_desired_flashlight_state(unit_index, TRUE);
			shader_tour_command("(unit_set_current_vitality (player0) 75 0)");
		}
		if (!shader_tour_next_weapon())
		{
			shader_tour_finish();
			break;
		}
		shader_tour.wait_until = game_time_get() + TICKS_PER_WEAPON;
		break;
	}

	default:
		break;
	}
}
