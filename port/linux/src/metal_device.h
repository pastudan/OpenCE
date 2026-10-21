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

#endif
