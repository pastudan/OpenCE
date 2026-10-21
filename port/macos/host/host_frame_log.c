/*
HOST_FRAME_LOG.C

HALO_FRAME_LOG (host_frame_log.h): one record a presented frame, kept in
memory (a ring of HALO_FRAME_LOG_FRAMES, default 36000: ten minutes at 60
frames a second) and written out once, at exit, as CSV; a summary and a
histogram in the log then, and a compact line every HALO_FRAME_LOG_INTERVAL
frames meanwhile. No line a frame: printing to a console every frame costs
time of its own.

The records are the main thread's (the one that runs the game and
presents), but for the GPU's times and the display's, which Metal's
handlers give on threads of their own: those fields are atomics.
tools/tvos_bench.py reads the CSV (frame_log_parse).
*/

#include "host.h"
#include "host_frame_log.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int host_frame_log_level;
struct host_frame_accumulators host_frame_acc;

struct frame_record
{
	_Atomic uint64_t serial;
	uint64_t present_ns, wall_ns;
	uint64_t host_ns, compile_ns, wait_inflight_ns, wait_drawable_ns, wait_readback_ns, sleep_ns;
	uint32_t draws, passes, copies, compiles, fog_draws, transparent_draws;
	_Atomic uint64_t gpu_ns, gpu_start_ns, gpu_end_ns, presented_ns;
	_Atomic uint32_t gpu_buffers, gpu_failed;
};

#define EVENTS 64

static struct frame_record *records;
static uint32_t capacity;
static uint64_t start_ns, last_present_ns, frames;
static uint32_t interval = 300;
static char path[1024];
static struct
{
	uint64_t at_ns, serial;
	char kind[16], detail[48];
} events[EVENTS];
static uint32_t event_count;
static uint64_t current_serial;

static struct frame_record *record_of(uint64_t serial)
{
	struct frame_record *record;

	if (!records || !capacity)
		return NULL;
	record = &records[serial % capacity];
	return atomic_load(&record->serial) == serial ? record : NULL;
}

int host_frame_log_main_thread(void)
{
	return pthread_main_np() != 0;
}

static int compare_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

/* the value at a fraction of sorted values (nearest rank) */
static uint64_t percentile(const uint64_t *sorted, uint64_t count, double fraction)
{
	uint64_t rank;

	if (!count)
		return 0;
	rank = (uint64_t)(fraction * (double)count + 0.999999);
	if (rank < 1)
		rank = 1;
	if (rank > count)
		rank = count;
	return sorted[rank - 1];
}

static double ms(uint64_t ns)
{
	return ns / 1000000.0;
}

static void write_csv(void)
{
	FILE *file;
	uint64_t serial, first = frames > capacity ? frames - capacity + 1 : 1;
	uint32_t index;

	if (!path[0])
		return;
	file = fopen(path, "w");
	if (!file)
	{
		host_log("frame log: cannot write %s", path);
		return;
	}
	fprintf(file, "# OpenCE frame log, level %d, %llu frames\n", host_frame_log_level,
		(unsigned long long)frames);
	for (index = 0; index < event_count; index++)
	{
		fprintf(file, "# event,%.3f,%llu,%s,%s\n", ms(events[index].at_ns - start_ns),
			(unsigned long long)events[index].serial, events[index].kind, events[index].detail);
	}
	fprintf(file, "serial,t_ms,wall_ms,host_ms,compile_ms,wait_inflight_ms,wait_drawable_ms,wait_readback_ms,"
		"sleep_ms,guest_ms,gpu_ms,gpu_buffers,gpu_failed,gpu_start_ms,gpu_end_ms,presented_ms,draws,passes,copies,"
		"compiles,fog_draws,transparent_draws\n");
	/* (the records are in the order they were presented: serials are the
	frames', from 1) */
	for (serial = first; serial <= current_serial; serial++)
	{
		struct frame_record *record = record_of(serial);
		uint64_t waits, gpu_start, gpu_end, presented;
		int64_t guest, host;

		if (!record || !record->present_ns)
			continue;
		waits = record->wait_inflight_ns + record->wait_drawable_ns + record->wait_readback_ns;
		/* (host_ns is the renderer's calls whole: their compiles and waits
		are taken out of it below, and the rest of the frame is the guest's) */
		guest = (int64_t)record->wall_ns - (int64_t)record->host_ns - (int64_t)record->sleep_ns;
		host = (int64_t)record->host_ns - (int64_t)record->compile_ns - (int64_t)waits;
		gpu_start = atomic_load(&record->gpu_start_ns);
		gpu_end = atomic_load(&record->gpu_end_ns);
		presented = atomic_load(&record->presented_ns);
		fprintf(file, "%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%.3f,%.3f,%.3f,%u,%u,%u,%u,%u,%u\n",
			(unsigned long long)serial, ms(record->present_ns - start_ns), ms(record->wall_ns),
			host_frame_log_level >= 2 && host > 0 ? host / 1000000.0 : 0.0,
			ms(record->compile_ns), ms(record->wait_inflight_ns), ms(record->wait_drawable_ns),
			ms(record->wait_readback_ns), ms(record->sleep_ns),
			host_frame_log_level >= 2 ? (guest > 0 ? guest : 0) / 1000000.0 : 0.0,
			ms(atomic_load(&record->gpu_ns)), atomic_load(&record->gpu_buffers),
			atomic_load(&record->gpu_failed),
			gpu_start > start_ns ? ms(gpu_start - start_ns) : 0.0, gpu_end > start_ns ? ms(gpu_end - start_ns) : 0.0,
			presented > start_ns ? ms(presented - start_ns) : 0.0, record->draws, record->passes, record->copies,
			record->compiles, record->fog_draws, record->transparent_draws);
	}
	fclose(file);
	host_log("frame log: %s", path);
}

/* the summary of the frames still in the ring: percentiles, counts over
16.7/33.3/50 ms and a histogram of the wall time */
static void summarize(void)
{
	static const double edges[] = { 8.0, 12.0, 16.7, 20.0, 25.0, 33.3, 50.0, 100.0, 250.0 };
	uint64_t *wall, *gpu, count = 0, serial, first = frames > capacity ? frames - capacity + 1 : 1;
	uint64_t histogram[sizeof(edges) / sizeof(edges[0]) + 1] = { 0 };
	uint64_t over16 = 0, over33 = 0, over50 = 0, gpu_count = 0, compile_ns = 0;
	char text[512];
	size_t used;
	uint32_t bucket;

	wall = malloc(sizeof(*wall) * capacity);
	gpu = malloc(sizeof(*gpu) * capacity);
	if (!wall || !gpu)
	{
		free(wall);
		free(gpu);
		return;
	}
	/* (the first frame's wall time is from the renderer's start) */
	for (serial = first + 1; serial <= current_serial; serial++)
	{
		struct frame_record *record = record_of(serial);
		double value;

		if (!record || !record->present_ns)
			continue;
		value = ms(record->wall_ns);
		wall[count++] = record->wall_ns;
		over16 += value > 16.7;
		over33 += value > 33.3;
		over50 += value > 50.0;
		for (bucket = 0; bucket < sizeof(edges) / sizeof(edges[0]) && value >= edges[bucket]; bucket++)
			;
		histogram[bucket]++;
		compile_ns += record->compile_ns;
		if (atomic_load(&record->gpu_buffers))
			gpu[gpu_count++] = atomic_load(&record->gpu_ns);
	}
	qsort(wall, count, sizeof(*wall), compare_u64);
	qsort(gpu, gpu_count, sizeof(*gpu), compare_u64);
	host_log("frame log summary: %llu frames; wall p50 %.2f p95 %.2f p99 %.2f max %.2f ms; >16.7 ms %llu, "
		">33.3 ms %llu, >50 ms %llu; GPU p50 %.2f p95 %.2f max %.2f ms (%llu frames); compiles %.1f ms",
		(unsigned long long)count, ms(percentile(wall, count, 0.50)), ms(percentile(wall, count, 0.95)),
		ms(percentile(wall, count, 0.99)), count ? ms(wall[count - 1]) : 0.0, (unsigned long long)over16,
		(unsigned long long)over33, (unsigned long long)over50, ms(percentile(gpu, gpu_count, 0.50)),
		ms(percentile(gpu, gpu_count, 0.95)), gpu_count ? ms(gpu[gpu_count - 1]) : 0.0,
		(unsigned long long)gpu_count, ms(compile_ns));
	used = (size_t)snprintf(text, sizeof(text), "frame log histogram (wall ms):");
	for (bucket = 0; bucket <= sizeof(edges) / sizeof(edges[0]) && used < sizeof(text); bucket++)
	{
		if (bucket < sizeof(edges) / sizeof(edges[0]))
			used += (size_t)snprintf(text + used, sizeof(text) - used, " <%g:%llu", edges[bucket],
				(unsigned long long)histogram[bucket]);
		else
			used += (size_t)snprintf(text + used, sizeof(text) - used, " >=%g:%llu", edges[bucket - 1],
				(unsigned long long)histogram[bucket]);
	}
	host_log("%s", text);
	free(wall);
	free(gpu);
}

static void finish(void)
{
	if (!host_frame_log_level || !records)
		return;
	summarize();
	write_csv();
	fflush(stderr);
}

void host_frame_log_start(void)
{
	const char *setting = getenv("HALO_FRAME_LOG");
	const char *value;

	if (records || !setting || !*setting || !strcmp(setting, "0"))
		return;
	host_frame_log_level = !strcmp(setting, "summary") ? 1 : 2;
	value = getenv("HALO_FRAME_LOG_FRAMES");
	capacity = value && atoi(value) > 0 ? (uint32_t)atoi(value) : 36000;
	value = getenv("HALO_FRAME_LOG_INTERVAL");
	if (value)
		interval = (uint32_t)(atoi(value) > 0 ? atoi(value) : 0);
	records = calloc(capacity, sizeof(*records));
	if (!records)
	{
		host_frame_log_level = 0;
		return;
	}
	value = getenv("HALO_FRAME_LOG_PATH");
	if (value && *value)
		snprintf(path, sizeof(path), "%s", value);
	else if (getenv("HALO_DATA_ROOT"))
		snprintf(path, sizeof(path), "%s/frame_log.csv", getenv("HALO_DATA_ROOT"));
	start_ns = last_present_ns = host_frame_log_now();
	memset(&host_frame_acc, 0, sizeof(host_frame_acc));
	atexit(finish);
	host_log("frame log: level %d, %u frames kept, a line every %u, to %s", host_frame_log_level, capacity,
		interval, path[0] ? path : "(nowhere)");
}

/* a compact line for the last interval's frames */
static void interval_line(uint64_t last)
{
	uint64_t wall[1024], serial, count = 0, gpu_total = 0, gpu_max = 0, compile_ns = 0, over33 = 0, wall_total = 0;
	uint64_t first = last >= interval ? last - interval + 1 : 1;

	for (serial = first; serial <= last && count < 1024; serial++)
	{
		struct frame_record *record = record_of(serial);
		uint64_t gpu;

		if (!record)
			continue;
		wall[count++] = record->wall_ns;
		wall_total += record->wall_ns;
		over33 += record->wall_ns > 33300000u;
		compile_ns += record->compile_ns;
		/* (the GPU's times of frames still on it come later: those of the
		frames done so far) */
		gpu = atomic_load(&record->gpu_ns);
		gpu_total += gpu;
		if (gpu > gpu_max)
			gpu_max = gpu;
	}
	if (!count)
		return;
	qsort(wall, count, sizeof(*wall), compare_u64);
	host_log("frame log %llu-%llu: wall avg %.2f p50 %.2f p95 %.2f max %.2f ms, >33 ms %llu; GPU avg %.2f max "
		"%.2f ms; compile %.1f ms", (unsigned long long)first, (unsigned long long)last, ms(wall_total / count),
		ms(percentile(wall, count, 0.50)), ms(percentile(wall, count, 0.95)), ms(wall[count - 1]),
		(unsigned long long)over33, ms(gpu_total / count), ms(gpu_max), ms(compile_ns));
}

void host_frame_log_present(uint64_t serial)
{
	struct frame_record *record;
	uint64_t now;

	if (!host_frame_log_level || !records)
		return;
	now = host_frame_log_now();
	record = &records[serial % capacity];
	/* (a slot reused: the GPU's fields of the frame it had are cleared
	before it is marked as this one's) */
	atomic_store(&record->serial, 0);
	atomic_store(&record->gpu_ns, host_frame_acc.gpu_early_ns);
	atomic_store(&record->gpu_start_ns, 0);
	atomic_store(&record->gpu_end_ns, 0);
	atomic_store(&record->presented_ns, 0);
	atomic_store(&record->gpu_buffers, host_frame_acc.gpu_early_buffers);
	atomic_store(&record->gpu_failed, 0);
	record->present_ns = now;
	record->wall_ns = now - last_present_ns;
	record->host_ns = host_frame_acc.host_ns;
	record->compile_ns = host_frame_acc.compile_ns;
	record->wait_inflight_ns = host_frame_acc.wait_inflight_ns;
	record->wait_drawable_ns = host_frame_acc.wait_drawable_ns;
	record->wait_readback_ns = host_frame_acc.wait_readback_ns;
	record->sleep_ns = host_frame_acc.sleep_ns;
	record->draws = host_frame_acc.draws;
	record->passes = host_frame_acc.passes;
	record->copies = host_frame_acc.copies;
	record->compiles = host_frame_acc.compiles;
	record->fog_draws = host_frame_acc.fog_draws;
	record->transparent_draws = host_frame_acc.transparent_draws;
	atomic_store(&record->serial, serial);
	memset(&host_frame_acc, 0, sizeof(host_frame_acc));
	last_present_ns = now;
	current_serial = serial;
	frames++;
	if (interval && serial % interval == 0)
		interval_line(serial);
}

static uint64_t ns_of_seconds(double seconds)
{
	/* (Metal's times, GPUStartTime and presentedTime, are seconds of the
	clock CACurrentMediaTime reads: mach_absolute_time converted, as
	CLOCK_UPTIME_RAW's nanoseconds are, whatever the timebase) */
	return seconds > 0.0 ? (uint64_t)(seconds * 1e9) : 0;
}

void host_frame_log_gpu(uint64_t serial, double start_seconds, double end_seconds, int failed)
{
	struct frame_record *record;
	uint64_t start, end, previous;

	if (!host_frame_log_level || !records)
		return;
	record = &records[serial % capacity];
	/* (the frame's command buffer completes after its present: its record
	is there, unless the ring went round) */
	if (atomic_load(&record->serial) != serial)
		return;
	start = ns_of_seconds(start_seconds);
	end = ns_of_seconds(end_seconds);
	if (failed)
		atomic_fetch_add(&record->gpu_failed, 1);
	if (end > start && start)
	{
		atomic_fetch_add(&record->gpu_ns, end - start);
		previous = atomic_load(&record->gpu_start_ns);
		if (!previous || start < previous)
			atomic_store(&record->gpu_start_ns, start);
		if (end > atomic_load(&record->gpu_end_ns))
			atomic_store(&record->gpu_end_ns, end);
	}
	atomic_fetch_add(&record->gpu_buffers, 1);
}

void host_frame_log_presented(uint64_t serial, double presented_seconds)
{
	struct frame_record *record;

	if (!host_frame_log_level || !records)
		return;
	record = &records[serial % capacity];
	if (atomic_load(&record->serial) == serial)
		atomic_store(&record->presented_ns, ns_of_seconds(presented_seconds));
}

void host_frame_log_event(const char *kind, const char *detail)
{
	if (!host_frame_log_level || event_count >= EVENTS)
		return;
	/* (the same file opened again and again: once) */
	if (event_count && !strcmp(events[event_count - 1].kind, kind) &&
		!strncmp(events[event_count - 1].detail, detail, sizeof(events[0].detail) - 1))
	{
		return;
	}
	events[event_count].at_ns = host_frame_log_now();
	events[event_count].serial = current_serial;
	snprintf(events[event_count].kind, sizeof(events[0].kind), "%s", kind);
	snprintf(events[event_count].detail, sizeof(events[0].detail), "%s", detail);
	event_count++;
	host_log("frame log event: %s %s at frame %llu", kind, detail, (unsigned long long)current_serial);
}
