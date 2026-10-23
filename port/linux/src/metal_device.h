/*
METAL_DEVICE.H

The macOS build's window drawing (HALO_METAL; port/macos/README.md), which
sdl_platform.c calls in place of OpenGL's context and swap. The device
draws with Metal in the host (port/macos/host/host_metal.m); the guest's
side is port/macos/guest/guest_metal.c.
*/

#ifndef __HALO_METAL_DEVICE_H
#define __HALO_METAL_DEVICE_H

#include <SDL3/SDL.h>

/* the window's drawable (a CAMetalLayer); FALSE if Metal cannot draw in it */
int metal_device_attach(SDL_Window *window);
/* each frame waits for the display (display.vsync) */
void metal_device_set_vsync(int vsync);
/* the frame drawn so far, shown */
void metal_device_present(void);
/* Opt-in diagnostic marker; does not change the effect or draw state. */
void metal_device_screen_effect(int type, int passes, float radius, int video);
void metal_device_profile_phase(int profile, int begin);
/* the play telemetry (port/macos/host/host_telemetry.h): its settings, the
view the rasterizer begins (rasterizer_xbox_profile.c), the game's state
(main.c): its ticks (-1: none), local players, player views, structure BSP
(-1: none), whether a cinematic plays, each view's camera (four of x, y, z)
and the map */
void metal_device_telemetry_configure(const char *setting, const char *live);
void metal_device_telemetry_window(long window_index);
void metal_device_telemetry_game(long game_ticks, long local_players, long windows, long bsp, long cinematic,
	const float *positions, const char *map);

#endif
