/*
HOST_LIFECYCLE.H

The app leaving the foreground and coming back, and the system short of
memory (host_lifecycle.m).

On the Apple TV the app stops being active for its Home button, Control
Center, a screensaver, the TV going to sleep, or a launch while another app
is shown. tvOS refuses GPU work from an app that is not in the foreground
("Insufficient Permission (to submit GPU work from background)"), so no
command buffer is committed then: every commit in host_metal.m, and the
present, pass host_lifecycle_gate first. On the main thread the gate stops
the game there until the app is active again: the frames in flight finished,
the audio paused, what is cheap to make again released, UIKit's run loop run
(the game's main thread is the app's: SDL calls the game from UIKit, and its
events come only while the run loop runs). The game is told when it goes on
(host_lifecycle_stops): its next frame is a frame's time, not the time away
(source/main/main.c), and a game in progress opens its pause menu
(port/linux/src/sdl_platform.c). The guest's clocks are not touched: its
threads (the vertical blank's, the audio's, the network's) run on through a
stop, and a clock moved back under them would stall them.

The Mac's app is never stopped (SDL sends no such events there): the gate is
one atomic load. Memory pressure is watched on both, logged (and in the play
telemetry, an "event" of kind "memory" with the footprint then), and answered
by a trim of a step a frame (host_lifecycle_frame).
*/

#ifndef __HALO_MACOS_HOST_LIFECYCLE_H
#define __HALO_MACOS_HOST_LIFECYCLE_H

#include <stdint.h>

/* the app's state watched (host_metal_attach, the main thread, once): SDL's
app events, the memory pressure */
void host_lifecycle_start(void);

/* before GPU work is committed: returns at once while the app is active;
otherwise, on the main thread, stops the game here until it is (1), and on
another thread waits for it */
int host_lifecycle_gate(void);

/* at the end of a present (main thread): a memory warning's telemetry event,
and a step of the trim it asked for */
void host_lifecycle_frame(void);

/* how many times the game has been stopped (the guest's: it compares) */
unsigned int host_lifecycle_stops(void);

/* host_metal.m's side: the frames committed finished on the GPU, and a step
of a trim (the bytes freed, about; -1 past the last step; stopped: the game
is stopped, and the frames' buffers may go too) */
void host_metal_drain(void);
int64_t host_metal_trim(int step, int stopped);

/* host_sdl.c's: every audio device paused (1), or those so paused resumed */
void host_sdl_audio_pause_all(int pause);

/* host_frame_log.c's and host_telemetry.c's: the time away left out of the
frame's wall and host times and the telemetry's second */
void host_frame_log_skip(uint64_t nanoseconds);
void host_telemetry_skip(uint64_t nanoseconds);

#endif
