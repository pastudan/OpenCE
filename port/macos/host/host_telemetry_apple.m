/*
HOST_TELEMETRY_APPLE.M

What the telemetry (host_telemetry.h) needs of the system: the thermal
state, the process's memory footprint, whether the app is active, the
display's refresh rate and the device's name. Sampled once a second on the
main thread; the notifications only log an event.
*/

#import <Foundation/Foundation.h>
#include <TargetConditionals.h>
#if TARGET_OS_TV || TARGET_OS_IOS
#import <UIKit/UIKit.h>
#include <os/proc.h>
#else
#import <AppKit/AppKit.h>
#endif
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <sys/sysctl.h>

#include "host.h"
#include "host_telemetry.h"

static _Atomic int platform_active = 1;

/* 2^16 dependent multiply-adds: ~30-60 us at an A12's or M4's full clock */
static uint64_t speed_probe_ns(void)
{
	static mach_timebase_info_data_t timebase;
	volatile uint64_t seed = 0x9e3779b97f4a7c15ull;
	uint64_t value = seed, started, ended;
	int index;

	if (!timebase.denom)
		mach_timebase_info(&timebase);
	started = mach_absolute_time();
	for (index = 0; index < 65536; index++)
		value = value * 6364136223846793005ull + 1442695040888963407ull;
	ended = mach_absolute_time();
	seed = value;
	return (ended - started) * timebase.numer / timebase.denom;
}

void host_telemetry_platform_sample(struct host_telemetry_platform *platform)
{
	task_vm_info_data_t information;
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	size_t cpu = 0;

	memset(platform, 0, sizeof(*platform));
	platform->probe_ns = speed_probe_ns();
	platform->cpu = pthread_cpu_number_np(&cpu) == 0 ? (int)cpu : -1;
	platform->thermal = (int)[NSProcessInfo processInfo].thermalState;
	platform->active = platform_active;
	if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&information, &count) == KERN_SUCCESS)
		platform->footprint = information.phys_footprint;
#if TARGET_OS_TV || TARGET_OS_IOS
	if (@available(tvOS 13.0, iOS 13.0, *))
		platform->available = os_proc_available_memory();
#endif
	platform->metal_allocated = host_metal_allocated_bytes();
}

void host_telemetry_platform_start(void)
{
	NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
	NSOperationQueue *main = [NSOperationQueue mainQueue];

	[center addObserverForName:NSProcessInfoThermalStateDidChangeNotification object:nil queue:main
		usingBlock:^(NSNotification *note)
	{
		static const char *const names[] = { "nominal", "fair", "serious", "critical" };
		NSInteger state = [NSProcessInfo processInfo].thermalState;

		(void)note;
		host_log("telemetry: thermal state %s", state >= 0 && state < 4 ? names[state] : "unknown");
	}];
#if TARGET_OS_TV || TARGET_OS_IOS
	[center addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:main
		usingBlock:^(NSNotification *note)
	{
		(void)note;
		platform_active = 1;
		host_telemetry_event("active", "1");
	}];
	[center addObserverForName:UIApplicationWillResignActiveNotification object:nil queue:main
		usingBlock:^(NSNotification *note)
	{
		(void)note;
		platform_active = 0;
		host_telemetry_event("active", "0");
	}];
#else
	[center addObserverForName:NSApplicationDidBecomeActiveNotification object:nil queue:main
		usingBlock:^(NSNotification *note)
	{
		(void)note;
		platform_active = 1;
		host_telemetry_event("active", "1");
	}];
	[center addObserverForName:NSApplicationDidResignActiveNotification object:nil queue:main
		usingBlock:^(NSNotification *note)
	{
		(void)note;
		platform_active = 0;
		host_telemetry_event("active", "0");
	}];
#endif
}

double host_telemetry_platform_refresh_hz(void)
{
	double hz = 0.0;

#if TARGET_OS_TV || TARGET_OS_IOS
	hz = (double)[UIScreen mainScreen].maximumFramesPerSecond;
#else
	if (@available(macOS 12.0, *))
		hz = (double)[NSScreen mainScreen].maximumFramesPerSecond;
#endif
	return hz > 0.0 ? hz : 60.0;
}

void host_telemetry_platform_describe(char *text, unsigned int size)
{
	char model[64] = "";
	size_t length = sizeof(model);
	NSString *system = [NSProcessInfo processInfo].operatingSystemVersionString;

	if (sysctlbyname("hw.machine", model, &length, NULL, 0) || !model[0])
	{
		length = sizeof(model);
		sysctlbyname("hw.model", model, &length, NULL, 0);
	}
#if TARGET_OS_TV || TARGET_OS_IOS
	{
		/* (the screen's own pixels and scale: whether a 1920x1080 drawable
		is the TV's output mode or a layer drawn at 1x) */
		UIScreen *screen = [UIScreen mainScreen];

		snprintf(text, size, "%s, %s, screen %.0fx%.0f points, native %.0fx%.0f at %.1fx, mode %.0fx%.0f", model,
			system.UTF8String ? system.UTF8String : "", screen.bounds.size.width, screen.bounds.size.height,
			screen.nativeBounds.size.width, screen.nativeBounds.size.height, screen.nativeScale,
			screen.currentMode.size.width, screen.currentMode.size.height);
	}
#else
	snprintf(text, size, "%s, %s", model, system.UTF8String ? system.UTF8String : "");
#endif
	/* (a JSON string: no quotes) */
	for (char *c = text; *c; c++)
	{
		if (*c == '"' || *c == '\\' || (unsigned char)*c < 32)
			*c = '_';
	}
}
