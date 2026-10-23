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
#include "host_telemetry.h"

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
	/* command buffers hoisted ahead of the frame's (host_metal.m): their
	span, which includes waiting behind the frame before, apart */
	_Atomic uint64_t hoisted_ns;
	_Atomic uint32_t hoisted_buffers;
	/* the fraction of the screen's resolution its 3D scenes were drawn at
	(display.dynamic_resolution; 1 without it, 0 for none drawn) */
	float render_scale;
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
/* the frame being presented's render scale (host_frame_log_render_scale) */
static float render_scale = 1.0f;

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
		"compiles,fog_draws,transparent_draws,hoisted_ms,hoisted_buffers,render_scale\n");
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
		fprintf(file, "%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%.3f,%.3f,%.3f,%u,%u,%u,%u,%u,%u,%.3f,%u,%.4f\n",
			(unsigned long long)serial, ms(record->present_ns - start_ns), ms(record->wall_ns),
			host_frame_log_level >= 2 && host > 0 ? host / 1000000.0 : 0.0,
			ms(record->compile_ns), ms(record->wait_inflight_ns), ms(record->wait_drawable_ns),
			ms(record->wait_readback_ns), ms(record->sleep_ns),
			host_frame_log_level >= 2 ? (guest > 0 ? guest : 0) / 1000000.0 : 0.0,
			ms(atomic_load(&record->gpu_ns)), atomic_load(&record->gpu_buffers),
			atomic_load(&record->gpu_failed),
			gpu_start > start_ns ? ms(gpu_start - start_ns) : 0.0, gpu_end > start_ns ? ms(gpu_end - start_ns) : 0.0,
			presented > start_ns ? ms(presented - start_ns) : 0.0, record->draws, record->passes, record->copies,
			record->compiles, record->fog_draws, record->transparent_draws, ms(atomic_load(&record->hoisted_ns)),
			atomic_load(&record->hoisted_buffers), record->render_scale);
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

/* ---------- the pass log (HALO_PASS_LOG, host_frame_log.h) */

int host_pass_log_enabled;
static struct host_pass_record *pass_records;
static uint32_t pass_capacity;
static uint64_t pass_count;
static char pass_path[1024];
static _Atomic uint64_t gpu_clock_first_ns, gpu_clock_first_ticks, gpu_clock_last_ns, gpu_clock_last_ticks;

struct host_pass_record *host_frame_log_pass_new(uint64_t serial, uint32_t index)
{
	struct host_pass_record *record;

	if (!host_pass_log_enabled || !pass_records)
		return NULL;
	record = &pass_records[pass_count++ % pass_capacity];
	atomic_store(&record->serial, 0);
	memset((char *)record + sizeof(record->serial), 0, sizeof(*record) - sizeof(record->serial));
	record->index = index;
	record->first_phase = record->last_phase = -1;
	atomic_store(&record->serial, serial);
	return record;
}

void host_frame_log_gpu_clock(uint64_t now_ns, uint64_t gpu_ticks)
{
	if (!gpu_ticks)
		return;
	if (!atomic_load(&gpu_clock_first_ticks))
	{
		atomic_store(&gpu_clock_first_ns, now_ns);
		atomic_store(&gpu_clock_first_ticks, gpu_ticks);
	}
	atomic_store(&gpu_clock_last_ns, now_ns);
	atomic_store(&gpu_clock_last_ticks, gpu_ticks);
}

static void write_pass_csv(void)
{
	FILE *file;
	uint64_t index, first = pass_count > pass_capacity ? pass_count - pass_capacity : 0;
	uint64_t first_ns = atomic_load(&gpu_clock_first_ns), first_ticks = atomic_load(&gpu_clock_first_ticks);
	uint64_t last_ns = atomic_load(&gpu_clock_last_ns), last_ticks = atomic_load(&gpu_clock_last_ticks);
	/* (ns a counter tick: from the first and latest pairs, else 1) */
	double ratio = last_ticks > first_ticks + 1000 ? (double)(last_ns - first_ns) / (double)(last_ticks - first_ticks) :
		1.0;

	if (!pass_path[0] || !pass_records)
		return;
	file = fopen(pass_path, "w");
	if (!file)
	{
		host_log("pass log: cannot write %s", pass_path);
		return;
	}
	fprintf(file, "# OpenCE pass log, %llu passes, gpu tick %.6f ns\n", (unsigned long long)pass_count, ratio);
	fprintf(file, "serial,index,color,depth,width,height,color_format,depth_format,color_load,depth_load,stencil_load,"
		"color_store,depth_store,stencil_store,reason,draws,clears,fog_draws,transparent_draws,target_samples,"
		"first_phase,last_phase,phase_mask,gpu_ms,vertex_start_ms,vertex_ms,fragment_start_ms,fragment_ms\n");
	for (index = first; index < pass_count; index++)
	{
		struct host_pass_record *record = &pass_records[index % pass_capacity];
		uint64_t serial = atomic_load(&record->serial);
		uint64_t gpu_start = atomic_load(&record->gpu_start_ns), gpu_end = atomic_load(&record->gpu_end_ns);
		uint64_t vertex_start = atomic_load(&record->vertex_start), vertex_end = atomic_load(&record->vertex_end);
		uint64_t fragment_start = atomic_load(&record->fragment_start);
		uint64_t fragment_end = atomic_load(&record->fragment_end);
		/* (a stage's times: both valid, in order) */
		int vertex = vertex_start && vertex_end >= vertex_start && vertex_end != ~0ull;
		int fragment = fragment_start && fragment_end >= fragment_start && fragment_end != ~0ull;

		if (!serial)
			continue;
		fprintf(file, "%llu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%d,%u,%.4f,%.4f,%.4f,%.4f,%.4f\n",
			(unsigned long long)serial, record->index, record->color, record->depth, record->width, record->height,
			record->color_format, record->depth_format, record->color_load, record->depth_load, record->stencil_load,
			record->color_store, record->depth_store, record->stencil_store, record->reason, record->draws,
			record->clears, record->fog_draws, record->transparent_draws, record->target_samples, record->first_phase,
			record->last_phase, record->phase_mask, gpu_end > gpu_start && gpu_start ? ms(gpu_end - gpu_start) : 0.0,
			vertex && first_ticks ? ((double)vertex_start - (double)first_ticks) * ratio / 1e6 +
				ms(first_ns > start_ns ? first_ns - start_ns : 0) : 0.0,
			vertex ? (double)(vertex_end - vertex_start) * ratio / 1e6 : 0.0,
			fragment && first_ticks ? ((double)fragment_start - (double)first_ticks) * ratio / 1e6 +
				ms(first_ns > start_ns ? first_ns - start_ns : 0) : 0.0,
			fragment ? (double)(fragment_end - fragment_start) * ratio / 1e6 : 0.0);
	}
	fclose(file);
	host_log("pass log: %s", pass_path);
}

static void pass_log_start(void)
{
	const char *setting = getenv("HALO_PASS_LOG");
	const char *value;

	if (!setting || !*setting || !strcmp(setting, "0"))
		return;
	value = getenv("HALO_PASS_LOG_PASSES");
	pass_capacity = value && atoi(value) > 0 ? (uint32_t)atoi(value) : 150000;
	pass_records = calloc(pass_capacity, sizeof(*pass_records));
	if (!pass_records)
		return;
	value = getenv("HALO_PASS_LOG_PATH");
	if (value && *value)
		snprintf(pass_path, sizeof(pass_path), "%s", value);
	else if (path[0] && strlen(path) > 4 && !strcmp(path + strlen(path) - 4, ".csv"))
		snprintf(pass_path, sizeof(pass_path), "%.*s-passes.csv", (int)strlen(path) - 4, path);
	else if (getenv("HALO_DATA_ROOT"))
		snprintf(pass_path, sizeof(pass_path), "%s/pass_log.csv", getenv("HALO_DATA_ROOT"));
	host_pass_log_enabled = 1;
	host_log("pass log: %u passes kept, to %s", pass_capacity, pass_path[0] ? pass_path : "(nowhere)");
}

static void finish(void)
{
	if (!host_frame_log_level || !records)
		return;
	summarize();
	write_csv();
	write_pass_csv();
	fflush(stderr);
}

/* the telemetry's alone (host_telemetry.h): the breakdown, in a ring of a
few seconds (the GPU's times of a second are read a second later), no CSV
and no lines in the log */
static void telemetry_only_start(void)
{
	host_frame_log_level = 2;
	capacity = 1024;
	interval = 0;
	records = calloc(capacity, sizeof(*records));
	if (!records)
	{
		host_frame_log_level = 0;
		return;
	}
	start_ns = last_present_ns = host_frame_log_now();
	memset(&host_frame_acc, 0, sizeof(host_frame_acc));
	host_frame_acc.view = HOST_VIEWS - 1;
	host_telemetry_start();
}

void host_frame_log_start(void)
{
	const char *setting = getenv("HALO_FRAME_LOG");
	const char *value;

	if (records)
		return;
	if (!setting || !*setting || !strcmp(setting, "0"))
	{
		if (host_telemetry_wanted())
			telemetry_only_start();
		return;
	}
	/* (the telemetry needs the breakdown: summary is the whole of it then) */
	host_frame_log_level = !strcmp(setting, "summary") && !host_telemetry_wanted() ? 1 : 2;
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
	host_frame_acc.view = HOST_VIEWS - 1;
	if (host_frame_log_level >= 2)
		pass_log_start();
	atexit(finish);
	host_log("frame log: level %d, %u frames kept, a line every %u, to %s", host_frame_log_level, capacity,
		interval, path[0] ? path : "(nowhere)");
	/* (the telemetry beside it, if it is on: it needs the breakdown) */
	if (host_frame_log_level >= 2)
		host_telemetry_start();
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

/* (host_lifecycle.h) the time the game was stopped, not the next frame's:
out of its wall time, and of its host time (the renderer's call it stopped
in counts it, HOST_FRAME_LOG_SCOPE) */
static uint64_t skipped_ns;

void host_frame_log_skip(uint64_t nanoseconds)
{
	last_present_ns += nanoseconds;
	skipped_ns += nanoseconds;
}

void host_frame_log_present(uint64_t serial)
{
	struct frame_record *record;
	uint64_t now;

	if (!host_frame_log_level || !records)
		return;
	now = host_frame_log_now();
	if (skipped_ns)
	{
		host_frame_acc.host_ns = host_frame_acc.host_ns > skipped_ns ? host_frame_acc.host_ns - skipped_ns : 0;
		skipped_ns = 0;
	}
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
	atomic_store(&record->hoisted_ns, 0);
	atomic_store(&record->hoisted_buffers, 0);
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
	record->render_scale = render_scale;
	atomic_store(&record->serial, serial);
	if (host_telemetry_enabled)
	{
		/* (the last view's time runs to the present) */
		if (host_frame_acc.view_started_ns)
			host_frame_acc.views[host_frame_acc.view].ns += now - host_frame_acc.view_started_ns;
		host_telemetry_frame(serial, now, record->wall_ns, &host_frame_acc);
	}
	memset(&host_frame_acc, 0, sizeof(host_frame_acc));
	host_frame_acc.view = HOST_VIEWS - 1;
	last_present_ns = now;
	current_serial = serial;
	frames++;
	if (interval && serial % interval == 0)
		interval_line(serial);
}

void host_frame_log_render_scale(float fraction)
{
	render_scale = fraction < 0.0f ? 0.0f : fraction;
	if (host_telemetry_enabled)
		host_telemetry_render_scale(render_scale);
}

uint64_t host_frame_log_presented_of(uint64_t serial)
{
	struct frame_record *record = record_of(serial);

	return record ? atomic_load(&record->presented_ns) : 0;
}

float host_frame_log_render_scale_of(uint64_t serial)
{
	struct frame_record *record = record_of(serial);

	return record ? record->render_scale : 1.0f;
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

void host_frame_log_gpu_hoisted(uint64_t serial, double start_seconds, double end_seconds)
{
	struct frame_record *record;
	uint64_t start, end;

	if (!host_frame_log_level || !records)
		return;
	record = &records[serial % capacity];
	if (atomic_load(&record->serial) != serial)
		return;
	start = ns_of_seconds(start_seconds);
	end = ns_of_seconds(end_seconds);
	if (end > start && start)
	{
		uint64_t previous = atomic_load(&record->gpu_start_ns);

		atomic_fetch_add(&record->hoisted_ns, end - start);
		/* (in the frame's GPU interval, gpu_start_ms to gpu_end_ms, whose
		union over a run is the GPU's busy time) */
		if (!previous || start < previous)
			atomic_store(&record->gpu_start_ns, start);
		if (end > atomic_load(&record->gpu_end_ns))
			atomic_store(&record->gpu_end_ns, end);
	}
	atomic_fetch_add(&record->hoisted_buffers, 1);
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

int host_frame_log_gpu_of(uint64_t serial, uint64_t *gpu_ns, uint32_t *buffers, uint64_t *hoisted_ns,
	uint64_t *span_ns)
{
	struct frame_record *record = record_of(serial);
	uint64_t start, end;

	*gpu_ns = *hoisted_ns = *span_ns = 0;
	*buffers = 0;
	if (!record)
		return 0;
	*gpu_ns = atomic_load(&record->gpu_ns);
	*buffers = atomic_load(&record->gpu_buffers);
	*hoisted_ns = atomic_load(&record->hoisted_ns);
	start = atomic_load(&record->gpu_start_ns);
	end = atomic_load(&record->gpu_end_ns);
	*span_ns = start && end > start ? end - start : 0;
	return 1;
}

void host_frame_log_event(const char *kind, const char *detail)
{
	if (host_telemetry_enabled)
		host_telemetry_event(kind, detail);
	/* (the telemetry's alone: no CSV to list it in, nor a line in the log) */
	if (!host_frame_log_level || event_count >= EVENTS || !path[0])
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
