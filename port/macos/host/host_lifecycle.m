/*
HOST_LIFECYCLE.M

The app's foreground state and the memory pressure (host_lifecycle.h).

The state is SDL's app events, which come from UIKit's scene delegate and
notifications on the main thread, as they happen (SDL_AddEventWatch: they
are never queued). The app is "away" from WILL_ENTER_BACKGROUND (UIKit's
willResignActive: the Home button, Control Center, the screensaver, sleep)
until DID_ENTER_FOREGROUND (didBecomeActive). It starts away on the TV until
UIKit says otherwise, so that a launch while another app is shown (or the TV
is asleep) submits nothing until the app is really on the screen.

The game runs on the main thread, inside SDL's call of it from UIKit's run
loop: while it is stopped in host_lifecycle_gate that run loop is run here,
which is how the events that end the stop arrive.
*/

#import <Foundation/Foundation.h>
#include <TargetConditionals.h>
#if TARGET_OS_TV || TARGET_OS_IOS
#import <UIKit/UIKit.h>
#include <os/proc.h>
#endif
#include <mach/mach.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

#include <SDL3/SDL.h>

#include "host.h"
#include "host_frame_log.h"
#include "host_lifecycle.h"
#include "host_telemetry.h"

/* 1 while the app is not active (the gate stops the game) */
static _Atomic int away;
/* the stops so far (host_lifecycle_stops), not counting a wait at the
launch, before the app was ever active (there is no game yet to pause) */
static _Atomic unsigned int stops;
static _Atomic int ever_active;
/* the memory warnings so far, and the newest's level (1 warning, 2
critical: dispatch's memory pressure, or UIKit's warning as 1) */
static _Atomic unsigned int warnings;
static _Atomic int warning_level;
/* (the main thread's) the warnings answered, and the trim's next step */
static unsigned int warnings_seen;
static int trim_step = -1;
static int64_t trim_freed;
static uint64_t trim_started_ns;
static dispatch_source_t pressure;
/* another thread waiting at the gate (a texture conversion's command
buffer, an audio thread): the main thread wakes it */
static pthread_mutex_t away_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t away_changed = PTHREAD_COND_INITIALIZER;

static uint64_t footprint_bytes(void)
{
	task_vm_info_data_t information;
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;

	return task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&information, &count) == KERN_SUCCESS ?
		information.phys_footprint : 0;
}

static uint64_t available_bytes(void)
{
#if TARGET_OS_TV || TARGET_OS_IOS
	if (@available(tvOS 13.0, iOS 13.0, *))
		return os_proc_available_memory();
#endif
	return 0;
}

static void away_set(int value, const char *why)
{
	if (!value)
		atomic_store(&ever_active, 1);
	if (atomic_exchange(&away, value) == value)
		return;
	host_log("lifecycle: %s (%s)", value ? "inactive: GPU work and the game stop" : "active again", why);
	host_frame_log_event("lifecycle", why);
	pthread_mutex_lock(&away_lock);
	pthread_cond_broadcast(&away_changed);
	pthread_mutex_unlock(&away_lock);
}

static void memory_warned(int level, const char *source)
{
	atomic_store(&warning_level, level);
	atomic_fetch_add(&warnings, 1);
	/* (the log line now, from whichever thread: the event and the trim on
	the main thread's next frame, host_lifecycle_frame) */
	host_log("lifecycle: memory %s (%s): footprint %.0f MB, %.0f MB more available", level >= 2 ? "critical" :
		"warning", source, footprint_bytes() / 1048576.0, available_bytes() / 1048576.0);
}

static bool SDLCALL app_event(void *userdata, SDL_Event *event)
{
	(void)userdata;
	switch (event->type)
	{
	case SDL_EVENT_WILL_ENTER_BACKGROUND:
		away_set(1, "will-resign-active");
		break;
	case SDL_EVENT_DID_ENTER_BACKGROUND:
		host_log("lifecycle: in the background");
		host_frame_log_event("lifecycle", "background");
		away_set(1, "background");
		break;
	case SDL_EVENT_WILL_ENTER_FOREGROUND:
		host_log("lifecycle: entering the foreground");
		host_frame_log_event("lifecycle", "will-enter-foreground");
		break;
	case SDL_EVENT_DID_ENTER_FOREGROUND:
		away_set(0, "did-become-active");
		break;
	case SDL_EVENT_TERMINATING:
		host_log("lifecycle: terminating");
		host_frame_log_event("lifecycle", "terminating");
		host_telemetry_flush();
		break;
	case SDL_EVENT_LOW_MEMORY:
		memory_warned(1, "low-memory");
		break;
	default:
		break;
	}
	return true;
}

void host_lifecycle_start(void)
{
	static int started;

	if (started)
		return;
	started = 1;
	SDL_AddEventWatch(app_event, NULL);
#if TARGET_OS_TV || TARGET_OS_IOS
	/* (the app's state now: a launch behind another app, or with the TV
	asleep, is not active, and its first frames must wait) */
	{
		UIApplicationState state = [UIApplication sharedApplication].applicationState;

		host_log("lifecycle: launched %s", state == UIApplicationStateActive ? "active" :
			state == UIApplicationStateInactive ? "inactive (not yet on the screen)" : "in the background");
		if (state != UIApplicationStateActive)
			away_set(1, state == UIApplicationStateInactive ? "launched-inactive" : "launched-background");
		else
			atomic_store(&ever_active, 1);
	}
#endif
	/* (the kernel's pressure, which may come before or without UIKit's
	warning: both are logged) */
	pressure = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
		DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL, dispatch_get_main_queue());
	if (pressure)
	{
		dispatch_source_set_event_handler(pressure, ^
		{
			unsigned long flags = dispatch_source_get_data(pressure);

			if (flags & (DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL))
				memory_warned(flags & DISPATCH_MEMORYPRESSURE_CRITICAL ? 2 : 1, "pressure");
		});
		dispatch_resume(pressure);
	}
}

unsigned int host_lifecycle_stops(void)
{
	return atomic_load(&stops);
}

static int memory_event(void);

/* (main thread) the game stopped until the app is active again */
static void stop(void)
{
	uint64_t started = host_frame_log_now(), stopped_ns;
	int64_t freed = 0, step_freed;
	int step;
	char detail[48];
	int counted = atomic_load(&ever_active);

	if (counted)
		atomic_fetch_add(&stops, 1);
	host_sdl_audio_pause_all(1);
	/* (what the GPU has been given finishes: tvOS lets an app that is
	resigning complete its work, not start more) */
	host_metal_drain();
	/* (all of the trim at once: nothing is drawn meanwhile, and the
	frames' buffers go too) */
	for (step = 0; (step_freed = host_metal_trim(step, 1)) >= 0; step++)
		freed += step_freed;
	host_log("lifecycle: stopped: GPU idle, audio paused, %.1f MB released; footprint %.0f MB", freed / 1048576.0,
		footprint_bytes() / 1048576.0);
	host_telemetry_flush();
	while (atomic_load(&away))
	{
		@autoreleasepool
		{
			/* (UIKit's events, and SDL's with them, come only from the run
			loop: the game is inside a run of it, SDL's call of the game) */
			SDL_PumpEvents();
			CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);
			/* (a warning meanwhile: logged; the trim is done already) */
			if (memory_event())
				host_telemetry_flush();
#if TARGET_OS_TV || TARGET_OS_IOS
			/* (and UIKit's state itself: at a launch the scene may have
			become active before the watch was added, its event missed) */
			if (atomic_load(&away) && [UIApplication sharedApplication].applicationState == UIApplicationStateActive)
				away_set(0, "state-active");
#endif
		}
	}
	stopped_ns = host_frame_log_now() - started;
	/* (the time away is not the frame's, nor the telemetry's second's) */
	host_frame_log_skip(stopped_ns);
	host_telemetry_skip(stopped_ns);
	host_sdl_audio_pause_all(0);
	snprintf(detail, sizeof(detail), "resumed after %.1f s", stopped_ns / 1e9);
	host_frame_log_event("lifecycle", detail);
	host_log("lifecycle: going on after %.1f s%s", stopped_ns / 1e9, counted ? "" : " (the launch's: not a pause)");
}

int host_lifecycle_gate(void)
{
	if (!atomic_load_explicit(&away, memory_order_relaxed))
		return 0;
	if (!pthread_main_np())
	{
		/* (the renderer is the main thread's: another thread here is not
		expected. It waits, but not for ever: the main thread may be what
		it holds up, and only the main thread hears that the app is back) */
		struct timespec until;
		int waits = 0;

		pthread_mutex_lock(&away_lock);
		while (atomic_load(&away) && waits++ < 20)
		{
			clock_gettime(CLOCK_REALTIME, &until);
			until.tv_sec += 1;
			pthread_cond_timedwait(&away_changed, &away_lock, &until);
		}
		pthread_mutex_unlock(&away_lock);
		if (atomic_load(&away))
			host_log("lifecycle: GPU work from another thread while not active (waited 20 s)");
		return 1;
	}
	stop();
	return 1;
}

/* (main thread) a memory warning since the last look, as a telemetry event
with the footprint then (whether the system warns before it takes memory
back, PERFORMANCE.md); 1 if there was one */
static int memory_event(void)
{
	unsigned int seen = atomic_load(&warnings);
	char detail[64];
	int level;

	if (seen == warnings_seen)
		return 0;
	warnings_seen = seen;
	level = atomic_load(&warning_level);
	snprintf(detail, sizeof(detail), "%s footprint %.0f MB avail %.0f MB", level >= 2 ? "critical" : "warning",
		footprint_bytes() / 1048576.0, available_bytes() / 1048576.0);
	host_frame_log_event("memory", detail);
	return 1;
}

void host_lifecycle_frame(void)
{
	/* HALO_MEMORY_WARNING_AFTER=seconds (diagnostic): a warning as UIKit's
	once, that long after the first frame (devicectl's sendMemoryWarning
	fails on the TV, and the simulator's menu item needs a person) */
	static uint64_t warn_at_ns = 1;

	if (warn_at_ns == 1)
	{
		const char *value = getenv("HALO_MEMORY_WARNING_AFTER");

		warn_at_ns = value && atof(value) > 0 ? host_frame_log_now() + (uint64_t)(atof(value) * 1e9) : 0;
	}
	if (warn_at_ns && host_frame_log_now() >= warn_at_ns)
	{
		warn_at_ns = 0;
		memory_warned(1, "HALO_MEMORY_WARNING_AFTER");
	}
	if (memory_event() && trim_step < 0)
	{
		trim_step = 0;
		trim_freed = 0;
		trim_started_ns = host_frame_log_now();
	}
	if (trim_step >= 0)
	{
		/* (a step a frame: never one long stall on the main thread) */
		int64_t freed = host_metal_trim(trim_step, 0);

		if (freed < 0)
		{
			char detail[48];

			snprintf(detail, sizeof(detail), "trimmed %.1f MB footprint %.0f MB", trim_freed / 1048576.0,
				footprint_bytes() / 1048576.0);
			host_frame_log_event("memory", detail);
			host_log("lifecycle: memory trim: %.1f MB released over %d frames (%.0f ms); footprint now %.0f MB",
				trim_freed / 1048576.0, trim_step, (host_frame_log_now() - trim_started_ns) / 1e6,
				footprint_bytes() / 1048576.0);
			trim_step = -1;
		}
		else
		{
			trim_freed += freed;
			trim_step++;
		}
	}
}
