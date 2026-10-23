/*
HOST_TELEMETRY.H

Telemetry: an always-available recorder of the game's performance while it
is played, cheap enough to leave on (port/tvos/PERFORMANCE.md, "Play
telemetry"). On by default in the TV build, off on the Mac:
debug.telemetry in config.toml (HALO_TELEMETRY) is "auto", "on" or "off".

It is built on the frame log's breakdown (host_frame_log.h, which it turns
on without the frame log's CSV) and writes JSON lines to
<data root>/telemetry/<session start>.jsonl (HALO_TELEMETRY_DIR), buffered
and written from a background queue, never synced: on the TV,
Library/Caches/OpenCE/telemetry, which tools/tv_telemetry.py pulls. The
folder keeps the newest HALO_TELEMETRY_KEEP sessions (default 40) and at most
HALO_TELEMETRY_MAX_MB (default 200) MB; a session stops writing at 64 MB.
Records, one a line, each with "k" (its kind) and "t" (seconds since the
session started):

- "session": the first: the build, the device, the display's refresh rate.
- "s": a second's aggregate: wall and game time, map, BSP, local players,
  views drawn and their cameras' positions, frames and their times (wall
  p50, p95, max; over 16.7, 33.3 and 50 ms; display slots missed), the
  guest's, the host's and the GPU's ms (p50, p95, max), compiles, waits,
  draws, passes and the per-view counts (shadows, fog, transparents),
  textures uploaded and converted, bytes streamed, files read and written on
  the main thread, shader manifest writes, the process's memory footprint,
  Metal's allocations, the thermal state, whether the app is active, the
  render resolution and the drawable's, and what the recorder itself cost.
- "hitch": a frame over HALO_TELEMETRY_HITCH_MS (default 50), its whole
  breakdown and what it did.
- "event": a map or saved state opened, the app made active or not, a
  thermal state change.
- "end": at a clean exit (tvOS rarely gives one), the totals.

debug.telemetry_live ("<IPv4 address>:<port>", HALO_TELEMETRY_LIVE; empty:
none) also sends each line as a UDP datagram, to tools/tv_telemetry.py
listen.
*/

#ifndef __HALO_MACOS_HOST_TELEMETRY_H
#define __HALO_MACOS_HOST_TELEMETRY_H

#include <stdint.h>

#include "host_frame_log.h"

struct metal_telemetry_game;

/* what only the platform knows (host_telemetry_apple.m) */
struct host_telemetry_platform
{
	/* NSProcessInfo.thermalState: 0 nominal, 1 fair, 2 serious, 3 critical */
	int thermal;
	/* the app active (in the foreground) */
	int active;
	/* task_info's phys_footprint, os_proc_available_memory (0 where there
	is none), MTLDevice.currentAllocatedSize */
	uint64_t footprint, available, metal_allocated;
};

/* whether it records (after host_telemetry_start) */
extern int host_telemetry_enabled;

/* the settings (debug.telemetry, debug.telemetry_live), from the guest
before host_metal_attach; NULL or "" leaves the environment's or the
default */
void host_telemetry_configure(const char *setting, const char *live);
/* whether it is to record: the setting, or the build's default */
int host_telemetry_wanted(void);
/* the session started (host_frame_log_start) */
void host_telemetry_start(void);
/* a presented frame: its serial, the clock at the present, its wall time
and what it spent (host_frame_log_present, before they are reset) */
void host_telemetry_frame(uint64_t serial, uint64_t now_ns, uint64_t wall_ns,
	const struct host_frame_accumulators *frame);
/* the picture's size and the drawable's (host_metal_present) */
void host_telemetry_sizes(uint32_t render_width, uint32_t render_height, uint32_t drawable_width,
	uint32_t drawable_height);
/* the view the rasterizer begins: a player's window index, or -1 */
void host_telemetry_view(int window_index);
/* the game's state, once a frame (port/macos/guest/guest_metal.c) */
void host_telemetry_game(const struct metal_telemetry_game *game);
/* something happened ("map-open", "a10.map"), from any thread */
void host_telemetry_event(const char *kind, const char *detail);
/* what is buffered handed to the writer (from the main thread) */
void host_telemetry_flush(void);
/* everything written, and the session's totals (at exit) */
void host_telemetry_finish(void);

/* host_telemetry_apple.m: the platform's state (main thread), its
notifications (thermal state, activity) watched, its display's refresh
rate (60 where unknown), the device's name; host_metal.m: Metal's
allocations */
void host_telemetry_platform_sample(struct host_telemetry_platform *platform);
void host_telemetry_platform_start(void);
double host_telemetry_platform_refresh_hz(void);
void host_telemetry_platform_describe(char *text, unsigned int size);
uint64_t host_metal_allocated_bytes(void);

/* ---------- the parts tools/test_tv_telemetry.py runs by themselves */

/* sessions in a folder (*.jsonl) deleted, oldest first (by name: the
start's time), until fewer than keep remain and their bytes are at most
max_bytes (room for a new one); how many were deleted */
int host_telemetry_rotate(const char *directory, int keep, uint64_t max_bytes);
/* display slots a frame of wall_ns missed, at a refresh interval of slot_ns */
uint32_t host_telemetry_missed_slots(uint64_t wall_ns, uint64_t slot_ns);

#endif
