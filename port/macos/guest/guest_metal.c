/*
GUEST_METAL.C

The guest's side of the Metal device (port/linux/src/metal_device.h): its
calls, made to the host (port/macos/host/host_metal.m) with the window's
handle (port/macos/guest/guest_sdl.c).
*/

#include <stdint.h>

#include "metal_device.h"
#include "guest_host.h"

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

void metal_device_profile_phase(int profile, int begin)
{
	host_metal_profile_phase(profile, begin);
}
