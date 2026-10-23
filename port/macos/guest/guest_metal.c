/*
GUEST_METAL.C

The guest's side of the Metal device (port/linux/src/metal_device.h): its
calls, made to the host (port/macos/host/host_metal.m) with the window's
handle (port/macos/guest/guest_sdl.c).
*/

#include <stdint.h>
#include <string.h>

#include "metal_device.h"
#include "guest_host.h"
#include "metal_protocol.h"

int metal_device_attach(SDL_Window *window)
{
	return host_metal_attach((unsigned int)(uintptr_t)window);
}

void metal_device_set_vsync(int vsync)
{
	host_metal_set_vsync(vsync);
}

void metal_device_present(void)
{
	host_metal_present();
}

void metal_device_screen_effect(int type, int passes, float radius, int video)
{
	host_metal_screen_effect(type, passes, radius, video);
}

/* (d3d8_metal.c: the queued transparents at half resolution) */
void halo_half_res_phase(int profile, int begin);

void metal_device_profile_phase(int profile, int begin)
{
	host_metal_profile_phase(profile, begin);
	halo_half_res_phase(profile, begin);
}

unsigned int metal_device_lifecycle_stops(void)
{
	return host_lifecycle_stops();
}

void metal_device_telemetry_configure(const char *setting, const char *live)
{
	host_telemetry_configure(setting, live);
}

void metal_device_telemetry_window(long window_index)
{
	host_telemetry_view((int)window_index);
}

void metal_device_telemetry_game(long game_ticks, long local_players, long windows, long bsp, long cinematic,
	const float *positions, const char *map)
{
	struct metal_telemetry_game game;
	const char *name = map;
	const char *at;

	memset(&game, 0, sizeof(game));
	game.game_ticks = (int)game_ticks;
	game.local_players = (int)local_players;
	game.windows = (int)windows;
	game.bsp = (int)bsp;
	game.cinematic = (int)cinematic;
	if (positions)
		memcpy(game.positions, positions, sizeof(game.positions));
	/* (the scenario's last part: "levels\\a10\\a10" is "a10") */
	for (at = map; at && *at; at++)
	{
		if (*at == '\\' || *at == '/')
			name = at + 1;
	}
	if (name)
		strncpy(game.map, name, sizeof(game.map) - 1);
	host_telemetry_game(&game);
}
