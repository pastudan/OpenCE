/*
HOST_FRAME_LOG.H

HALO_FRAME_LOG: per-frame timing for performance work (port/tvos/PERFORMANCE.md),
off by default. host_frame_log.c keeps one record per presented frame in
memory and writes them out once, at exit, as CSV (HALO_FRAME_LOG_PATH, else
<data root>/frame_log.csv), with a summary and a histogram in the log and a
short line every HALO_FRAME_LOG_INTERVAL frames (default 300, 0: none).

HALO_FRAME_LOG=summary (level 1) times only present to present: one clock
read a frame. HALO_FRAME_LOG=1 (level 2) also splits the frame: Metal
encoding (host), shader/pipeline compiles, the waits for a free frame, a
drawable and a readback, the main thread's sleeps, real GPU time
(GPUStartTime/GPUEndTime of the frame's command buffers) and when the frame
reached the display (the drawable's presented handler; not on tvOS, whose
SDK does not have it: 0 there). What is left of the
wall time is the guest's own work (the translated game, SDL, audio, files).
*/

#ifndef __HALO_MACOS_HOST_FRAME_LOG_H
#define __HALO_MACOS_HOST_FRAME_LOG_H

#include <pthread.h>
#include <stdint.h>
#include <time.h>

/* 0 off, 1 wall time only, 2 the whole breakdown. The telemetry recorder
(host_telemetry.h) turns the breakdown on by itself, without the CSV. */
extern int host_frame_log_level;

/* the rasterizer's phases the frame log counts draws of (rasterizer.h's
_rasterizer_profile_*, given by host_metal_profile_phase) */
enum
{
	HOST_PHASE_SHADOWS = 4,
	HOST_PHASE_FOG = 17,
	HOST_PHASE_TRANSPARENTS = 22,
};

/* views of a frame told apart: the players' (split screen: up to four)
and the rest (the overlay window, menus), whose index is HOST_VIEWS - 1 */
#define HOST_VIEWS 5
struct host_view_accumulators
{
	uint32_t draws, passes, shadow_draws, fog_draws, transparent_draws, windows;
	/* from the view's start to the next's (or the present): the guest's
	and the host's time drawing it */
	uint64_t ns;
};

/* what the frame has spent so far (the main thread's), reset at each present */
struct host_frame_accumulators
{
	uint64_t host_ns;
	uint64_t compile_ns;
	uint64_t wait_inflight_ns, wait_drawable_ns, wait_readback_ns;
	uint64_t sleep_ns;
	/* the GPU's time of the frame's command buffers already waited for
	(a readback's: host_metal_texture_read) */
	uint64_t gpu_early_ns;
	uint32_t gpu_early_buffers;
	uint32_t draws, passes, copies, compiles;
	/* draws in the rasterizer's fog-plane and queued-transparent phases
	(host_metal_profile_phase): where the opening's fog is */
	uint32_t fog_draws, transparent_draws;
	/* the telemetry's (host_telemetry.h): textures written (pixels into a
	level, or made from DXT blocks converted here), dropped; the bytes the
	frame streamed through its ring (vertices, indices, staged pixels) and
	buffers made past it; the game's files read and written on this thread;
	the shader manifest's records */
	uint32_t texture_uploads, texture_conversions, texture_deletes, ring_overflows;
	uint64_t texture_upload_bytes, texture_conversion_bytes, texture_conversion_ns, stream_bytes;
	uint32_t file_reads, file_writes, manifest_writes;
	uint64_t file_read_bytes, file_write_bytes, file_ns, manifest_bytes, manifest_ns;
	uint32_t shadow_draws;
	/* the view drawn now (host_telemetry_view), and each view's counts */
	int32_t view;
	uint64_t view_started_ns;
	struct host_view_accumulators views[HOST_VIEWS];
};
extern struct host_frame_accumulators host_frame_acc;

/* the counts of a draw and of a render pass begun, in the rasterizer's
phase (-1: none) */
static inline void host_frame_log_count_draw(int phase)
{
	struct host_view_accumulators *view = &host_frame_acc.views[host_frame_acc.view];

	host_frame_acc.draws++;
	host_frame_acc.fog_draws += phase == HOST_PHASE_FOG;
	host_frame_acc.transparent_draws += phase == HOST_PHASE_TRANSPARENTS;
	host_frame_acc.shadow_draws += phase == HOST_PHASE_SHADOWS;
	view->draws++;
	view->fog_draws += phase == HOST_PHASE_FOG;
	view->transparent_draws += phase == HOST_PHASE_TRANSPARENTS;
	view->shadow_draws += phase == HOST_PHASE_SHADOWS;
}

static inline void host_frame_log_count_pass(void)
{
	host_frame_acc.passes++;
	host_frame_acc.views[host_frame_acc.view].passes++;
}

static inline uint64_t host_frame_log_now(void)
{
	/* (mach_absolute_time in nanoseconds: the clock of CACurrentMediaTime,
	GPUStartTime and a drawable's presentedTime) */
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

/* whether to split this frame's time here: the breakdown is on, and this
is the main thread, which runs the game and presents (the game's other
threads call the host too: their time is not the frame's) */
static inline int host_frame_log_detail(void)
{
	return host_frame_log_level >= 2 && pthread_main_np();
}

/* reads the settings, once: from host_metal_attach */
void host_frame_log_start(void);
/* whether this is the thread that presents */
int host_frame_log_main_thread(void);
/* the end of a present (after its commit) of frame serial */
void host_frame_log_present(uint64_t serial);
/* a command buffer of frame serial finished on the GPU (any thread) */
void host_frame_log_gpu(uint64_t serial, double start_seconds, double end_seconds, int failed);
/* a command buffer hoisted ahead of frame serial's finished (its span
includes waiting behind the frame before: not added to the GPU time) */
void host_frame_log_gpu_hoisted(uint64_t serial, double start_seconds, double end_seconds);
/* frame serial reached the display (0: it never did) */
void host_frame_log_presented(uint64_t serial, double presented_seconds);
/* something to find the frames by ("map-open", "a10.map") */
void host_frame_log_event(const char *kind, const char *detail);
/* the GPU's times of frame serial so far (its command buffers', the
hoisted ones' apart, and the span from the first's start to the last's
end); 0 if its record is gone */
int host_frame_log_gpu_of(uint64_t serial, uint64_t *gpu_ns, uint32_t *buffers, uint64_t *hoisted_ns,
	uint64_t *span_ns);

/* HALO_PASS_LOG=1 (with HALO_FRAME_LOG=1): a record of every render pass,
kept in a ring (HALO_PASS_LOG_PASSES, default 150000) and written at exit
as CSV (HALO_PASS_LOG_PATH, else <data root>/pass_log.csv): its targets,
load and store actions, why it ended, its draws and the rasterizer phases
they were in, and its GPU time (host_metal.m: the GPU's timestamps at the
pass's vertex and fragment stages where the device can sample them, else,
with HALO_METAL_PASS_TIMING=buffers, a command buffer a pass). Diagnostic
only: off by default. */
struct host_pass_record
{
	_Atomic uint64_t serial;
	uint32_t index;
	uint32_t color, depth, width, height, color_format, depth_format;
	uint8_t color_load, depth_load, stencil_load, color_store, depth_store, stencil_store;
	uint8_t reason;
	int8_t first_phase, last_phase;
	uint32_t phase_mask;
	uint32_t draws, clears, fog_draws, transparent_draws, target_samples;
	/* a command buffer of its own (CLOCK_UPTIME_RAW ns), or the GPU's
	counter ticks of its stages (converted when written) */
	_Atomic uint64_t gpu_start_ns, gpu_end_ns;
	_Atomic uint64_t vertex_start, vertex_end, fragment_start, fragment_end;
};
extern int host_pass_log_enabled;
/* a new record of frame serial's (NULL with the log off) */
struct host_pass_record *host_frame_log_pass_new(uint64_t serial, uint32_t index);
/* a pair of the CPU's clock and the GPU's counter ticks, for converting the
ticks (the first and the latest are kept) */
void host_frame_log_gpu_clock(uint64_t now_ns, uint64_t gpu_ticks);

/* the time of a host call, less what it waited and compiled: one at the
top of each of the renderer's entry points */
static inline void host_frame_log_scope_end(uint64_t *started)
{
	if (*started)
		host_frame_acc.host_ns += host_frame_log_now() - *started;
}
#define HOST_FRAME_LOG_SCOPE \
	uint64_t host_frame_log_scope __attribute__((cleanup(host_frame_log_scope_end))) = \
		host_frame_log_detail() ? host_frame_log_now() : 0

#endif
