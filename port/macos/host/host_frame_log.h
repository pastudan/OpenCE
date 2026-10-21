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

/* 0 off, 1 wall time only, 2 the whole breakdown */
extern int host_frame_log_level;

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
};
extern struct host_frame_accumulators host_frame_acc;

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
/* frame serial reached the display (0: it never did) */
void host_frame_log_presented(uint64_t serial, double presented_seconds);
/* something to find the frames by ("map-open", "a10.map") */
void host_frame_log_event(const char *kind, const char *detail);

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
