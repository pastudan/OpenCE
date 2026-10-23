/*
HOST_TELEMETRY.C

The play telemetry (host_telemetry.h). The main thread does the counting:
a frame's numbers are kept in the second's arrays at its present, and a
second's record is made one second late, once the GPU has finished its
frames (their times come from the frame log's ring, host_frame_log_gpu_of).
The lines go into a buffer, which is handed once a second to a serial
background queue that appends it to the session's file (no fsync) and, if
asked, sends it over UDP. Nothing here waits on the GPU, a lock the GPU
holds, or the file.
*/

#include "host.h"
#include "host_frame_log.h"
#include "host_telemetry.h"
#include "metal_protocol.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int host_telemetry_enabled;

/* a second's frames at most (more are counted, not kept for percentiles) */
#define SECOND_FRAMES 512
/* hitches kept for a second's record, at most */
#define SECOND_HITCHES 16
/* a session's file, at most */
#define SESSION_LIMIT (64ull << 20)

struct frame_numbers
{
	uint64_t serial;
	uint32_t wall_us, guest_us, host_us;
};

struct hitch
{
	uint64_t serial, at_ns;
	struct host_frame_accumulators frame;
	uint64_t wall_ns;
	uint32_t events;
	char event[48];
};

/* a second's numbers */
struct second
{
	uint64_t start_ns, end_ns;
	uint32_t frames, kept;
	struct frame_numbers numbers[SECOND_FRAMES];
	uint32_t over16, over33, over50, missed_slots;
	uint64_t wall_ns, guest_ns, host_ns, compile_ns, wait_drawable_ns, wait_inflight_ns, wait_readback_ns, sleep_ns;
	uint32_t compiles, draws, passes, copies, shadow_draws, fog_draws, transparent_draws;
	uint32_t texture_uploads, texture_conversions, texture_deletes, ring_overflows;
	uint64_t texture_upload_bytes, texture_conversion_bytes, texture_conversion_ns, stream_bytes, stream_most;
	uint32_t file_reads, file_writes, manifest_writes;
	uint64_t file_read_bytes, file_write_bytes, file_ns, manifest_bytes, manifest_ns;
	/* the most views a frame drew, and the counts of each view */
	uint32_t views_most;
	struct host_view_accumulators views[HOST_VIEWS];
	uint32_t hitch_count;
	struct hitch hitches[SECOND_HITCHES];
	/* the game's state at the second's last frame */
	struct metal_telemetry_game game;
	uint32_t render_width, render_height, drawable_width, drawable_height;
	uint64_t self_ns;
};

static struct
{
	int configured; /* -1 off, 0 auto, 1 on */
	char live[64];
	uint64_t start_ns, slot_ns;
	double refresh_hz;
	time_t start_wall;
	char directory[1024], path[1200];
	/* the second being counted, and the one waiting for its GPU times */
	struct second *current, *waiting;
	/* the lines not yet handed to the writer */
	char *buffer;
	size_t used, size;
	dispatch_queue_t queue;
	/* the writer's (its queue's) */
	FILE *file;
	uint64_t written;
	int socket;
	struct sockaddr_in live_address;
	_Atomic uint64_t writer_ns, writer_bytes, dropped_bytes;
	/* the game's state, as the guest last gave it */
	struct metal_telemetry_game game;
	uint32_t render_width, render_height, drawable_width, drawable_height;
	/* events since the last frame (for a hitch), from any thread */
	_Atomic uint32_t events;
	char last_event[48];
	int thermal, active;
	uint64_t seconds, frames, hitches, self_ns;
	uint64_t footprint_first, footprint_most;
} telemetry = { .socket = -1 };

static pthread_mutex_t event_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------- small things */

uint32_t host_telemetry_missed_slots(uint64_t wall_ns, uint64_t slot_ns)
{
	uint64_t slots;

	if (!slot_ns)
		return 0;
	/* (a frame of one slot is on time; of 1.5 to 2.5 slots it missed one) */
	slots = (wall_ns + slot_ns / 2) / slot_ns;
	return slots > 1 ? (uint32_t)(slots - 1) : 0;
}

static int compare_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return x < y ? -1 : x > y;
}

/* a percentile of values (nearest rank), which it sorts */
static uint32_t percentile(uint32_t *values, uint32_t count, double fraction)
{
	uint32_t rank;

	if (!count)
		return 0;
	qsort(values, count, sizeof(*values), compare_u32);
	rank = (uint32_t)(fraction * count + 0.999999);
	if (rank < 1)
		rank = 1;
	if (rank > count)
		rank = count;
	return values[rank - 1];
}

static double ms_of_us(uint32_t us)
{
	return us / 1000.0;
}

static double ms(uint64_t ns)
{
	return ns / 1000000.0;
}

static double seconds_since_start(uint64_t ns)
{
	return ns > telemetry.start_ns ? (ns - telemetry.start_ns) / 1e9 : 0.0;
}

/* text fit for a JSON string: printable, no quotes or backslashes */
static void clean(char *out, size_t size, const char *in)
{
	size_t used = 0;

	for (; in && *in && used + 1 < size; in++)
	{
		unsigned char c = (unsigned char)*in;

		out[used++] = c >= 32 && c < 127 && c != '"' && c != '\\' ? (char)c : '_';
	}
	out[used] = 0;
}

static void append(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void append(const char *format, ...)
{
	va_list arguments;
	int length;

	for (;;)
	{
		size_t room = telemetry.size - telemetry.used;

		va_start(arguments, format);
		length = vsnprintf(telemetry.buffer ? telemetry.buffer + telemetry.used : NULL, telemetry.buffer ? room : 0,
			format, arguments);
		va_end(arguments);
		if (length < 0)
			return;
		if (telemetry.buffer && (size_t)length < room)
		{
			telemetry.used += (size_t)length;
			return;
		}
		{
			size_t size = telemetry.size ? telemetry.size * 2 : 65536;
			char *grown;

			while (size - telemetry.used <= (size_t)length)
				size *= 2;
			if (!(grown = realloc(telemetry.buffer, size)))
				return;
			telemetry.buffer = grown;
			telemetry.size = size;
		}
	}
}

/* ---------- the writer (its queue) */

struct chunk
{
	size_t size;
	char data[];
};

int host_telemetry_rotate(const char *directory, int keep, uint64_t max_bytes)
{
	struct entry
	{
		char name[256];
		uint64_t bytes;
	} *entries = NULL;
	size_t count = 0, capacity = 0, index;
	uint64_t total = 0;
	int deleted = 0;
	DIR *folder = opendir(directory);
	struct dirent *item;

	if (!folder)
		return 0;
	while ((item = readdir(folder)))
	{
		size_t length = strlen(item->d_name);
		char path[1400];
		struct stat status;

		if (length < 7 || strcmp(item->d_name + length - 6, ".jsonl") || length >= sizeof(entries->name))
			continue;
		snprintf(path, sizeof(path), "%s/%s", directory, item->d_name);
		if (stat(path, &status) || !S_ISREG(status.st_mode))
			continue;
		if (count == capacity)
		{
			struct entry *grown = realloc(entries, (capacity = capacity ? capacity * 2 : 32) * sizeof(*entries));

			if (!grown)
				break;
			entries = grown;
		}
		snprintf(entries[count].name, sizeof(entries[count].name), "%s", item->d_name);
		entries[count].bytes = (uint64_t)status.st_size;
		total += entries[count].bytes;
		count++;
	}
	closedir(folder);
	/* (oldest first: the names are the sessions' start times) */
	for (index = 1; index < count; index++)
	{
		struct entry entry = entries[index];
		size_t at = index;

		while (at > 0 && strcmp(entries[at - 1].name, entry.name) > 0)
		{
			entries[at] = entries[at - 1];
			at--;
		}
		entries[at] = entry;
	}
	for (index = 0; index < count && (count - index >= (size_t)(keep > 0 ? keep : 1) || total > max_bytes); index++)
	{
		char path[1400];

		snprintf(path, sizeof(path), "%s/%s", directory, entries[index].name);
		if (!unlink(path))
			deleted++;
		total -= entries[index].bytes;
	}
	free(entries);
	return deleted;
}

static void writer_open(void *context)
{
	const char *value = getenv("HALO_TELEMETRY_KEEP");
	int keep = value && atoi(value) > 0 ? atoi(value) : 40;
	uint64_t max_bytes;

	(void)context;
	value = getenv("HALO_TELEMETRY_MAX_MB");
	max_bytes = (uint64_t)(value && atoi(value) > 0 ? atoi(value) : 200) << 20;
	mkdir(telemetry.directory, 0755);
	host_telemetry_rotate(telemetry.directory, keep, max_bytes);
	telemetry.file = fopen(telemetry.path, "a");
	if (!telemetry.file)
		host_log("telemetry: cannot write %s (%s)", telemetry.path, strerror(errno));
}

static void writer_write(void *context)
{
	struct chunk *chunk = context;
	uint64_t started = host_frame_log_now();

	if (telemetry.file && telemetry.written + chunk->size <= SESSION_LIMIT)
	{
		if (fwrite(chunk->data, 1, chunk->size, telemetry.file) == chunk->size)
			telemetry.written += chunk->size;
		/* (to the system, not to the disk: no fsync) */
		fflush(telemetry.file);
		atomic_fetch_add(&telemetry.writer_bytes, chunk->size);
	}
	else
	{
		atomic_fetch_add(&telemetry.dropped_bytes, chunk->size);
	}
	if (telemetry.socket >= 0)
	{
		const char *line = chunk->data, *end = chunk->data + chunk->size;

		while (line < end)
		{
			const char *next = memchr(line, '\n', (size_t)(end - line));
			size_t length = next ? (size_t)(next - line + 1) : (size_t)(end - line);

			sendto(telemetry.socket, line, length, 0, (const struct sockaddr *)&telemetry.live_address,
				sizeof(telemetry.live_address));
			line += length;
		}
	}
	atomic_fetch_add(&telemetry.writer_ns, host_frame_log_now() - started);
	free(chunk);
}

static void writer_close(void *context)
{
	(void)context;
	if (telemetry.file)
		fclose(telemetry.file);
	telemetry.file = NULL;
}

void host_telemetry_flush(void)
{
	struct chunk *chunk;

	if (!telemetry.queue || !telemetry.used)
		return;
	if (!(chunk = malloc(sizeof(*chunk) + telemetry.used)))
		return;
	chunk->size = telemetry.used;
	memcpy(chunk->data, telemetry.buffer, telemetry.used);
	telemetry.used = 0;
	dispatch_async_f(telemetry.queue, chunk, writer_write);
}

/* ---------- the settings */

void host_telemetry_configure(const char *setting, const char *live)
{
	if (setting && *setting)
	{
		telemetry.configured = !strcmp(setting, "off") || !strcmp(setting, "0") || !strcmp(setting, "false") ? -1 :
			!strcmp(setting, "on") || !strcmp(setting, "1") || !strcmp(setting, "true") ? 1 : 0;
	}
	if (live)
		snprintf(telemetry.live, sizeof(telemetry.live), "%s", live);
}

int host_telemetry_wanted(void)
{
	/* (the host's own environment first: a run that sets it, before the
	guest has read its settings) */
	const char *value = getenv("HALO_TELEMETRY");

	if (value && *value)
		host_telemetry_configure(value, NULL);
	if (telemetry.configured)
		return telemetry.configured > 0;
#ifdef HALO_TVOS
	return 1;
#else
	return 0;
#endif
}

static void live_start(void)
{
	const char *setting = telemetry.live[0] ? telemetry.live : getenv("HALO_TELEMETRY_LIVE");
	char address[64];
	const char *colon;
	int port;

	if (!setting || !*setting || !(colon = strrchr(setting, ':')))
		return;
	snprintf(address, sizeof(address), "%.*s", (int)(colon - setting), setting);
	port = atoi(colon + 1);
	memset(&telemetry.live_address, 0, sizeof(telemetry.live_address));
	telemetry.live_address.sin_family = AF_INET;
	telemetry.live_address.sin_port = htons((uint16_t)port);
	if (port <= 0 || port > 65535 || inet_pton(AF_INET, address, &telemetry.live_address.sin_addr) != 1)
	{
		host_log("telemetry: debug.telemetry_live is not \"<IPv4 address>:<port>\": %s", setting);
		return;
	}
	telemetry.socket = socket(AF_INET, SOCK_DGRAM, 0);
	host_log("telemetry: live to %s:%d", address, port);
}

/* ---------- the session */

static const char *thermal_name(int thermal)
{
	static const char *const names[] = { "nominal", "fair", "serious", "critical" };

	return thermal >= 0 && thermal < 4 ? names[thermal] : "unknown";
}

void host_telemetry_start(void)
{
	char description[400], stamp[32];
	struct tm utc;
	const char *value;

	if (host_telemetry_enabled || !host_telemetry_wanted())
		return;
	telemetry.current = calloc(1, sizeof(*telemetry.current));
	telemetry.waiting = calloc(1, sizeof(*telemetry.waiting));
	if (!telemetry.current || !telemetry.waiting)
		return;
	value = getenv("HALO_TELEMETRY_DIR");
	if (value && *value)
		snprintf(telemetry.directory, sizeof(telemetry.directory), "%s", value);
	else
		snprintf(telemetry.directory, sizeof(telemetry.directory), "%s/telemetry",
			getenv("HALO_DATA_ROOT") ? getenv("HALO_DATA_ROOT") : ".");
	telemetry.start_wall = time(NULL);
	gmtime_r(&telemetry.start_wall, &utc);
	strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", &utc);
	snprintf(telemetry.path, sizeof(telemetry.path), "%s/%s-%d.jsonl", telemetry.directory, stamp, (int)getpid());
	telemetry.start_ns = host_frame_log_now();
	telemetry.current->start_ns = telemetry.start_ns;
	telemetry.refresh_hz = host_telemetry_platform_refresh_hz();
	telemetry.slot_ns = (uint64_t)(1e9 / (telemetry.refresh_hz > 0 ? telemetry.refresh_hz : 60.0));
	telemetry.game.game_ticks = -1;
	telemetry.game.bsp = -1;
	telemetry.thermal = telemetry.active = -1;
	telemetry.queue = dispatch_queue_create("opence.telemetry", DISPATCH_QUEUE_SERIAL);
	dispatch_set_target_queue(telemetry.queue, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
	live_start();
	dispatch_async_f(telemetry.queue, NULL, writer_open);
	host_telemetry_enabled = 1;
	host_telemetry_platform_start();
	host_telemetry_platform_describe(description, sizeof(description));
	append("{\"k\":\"session\",\"v\":1,\"t\":0,\"start\":\"%s\",\"unix\":%lld,\"device\":\"%s\",\"refresh_hz\":%.2f,"
		"\"pid\":%d,\"hitch_ms\":50}\n", stamp, (long long)telemetry.start_wall, description, telemetry.refresh_hz,
		(int)getpid());
	host_telemetry_flush();
	atexit(host_telemetry_finish);
	host_log("telemetry: recording to %s", telemetry.path);
}

void host_telemetry_event(const char *kind, const char *detail)
{
	char clean_kind[24], clean_detail[64];
	uint64_t now;

	if (!host_telemetry_enabled)
		return;
	clean(clean_kind, sizeof(clean_kind), kind);
	clean(clean_detail, sizeof(clean_detail), detail);
	now = host_frame_log_now();
	pthread_mutex_lock(&event_lock);
	snprintf(telemetry.last_event, sizeof(telemetry.last_event), "%s %s", clean_kind, clean_detail);
	atomic_fetch_add(&telemetry.events, 1);
	/* (the buffer is the main thread's: an event of another thread waits
	in last_event for the next frame's hitch, and is written now only from
	the main thread) */
	if (pthread_main_np())
	{
		append("{\"k\":\"event\",\"t\":%.3f,\"kind\":\"%s\",\"detail\":\"%s\"}\n", seconds_since_start(now), clean_kind,
			clean_detail);
	}
	pthread_mutex_unlock(&event_lock);
}

/* ---------- frames, views and the game */

void host_telemetry_sizes(uint32_t render_width, uint32_t render_height, uint32_t drawable_width,
	uint32_t drawable_height)
{
	telemetry.render_width = render_width;
	telemetry.render_height = render_height;
	telemetry.drawable_width = drawable_width;
	telemetry.drawable_height = drawable_height;
}

void host_telemetry_view(int window_index)
{
	uint64_t now;
	int view;

	if (!host_telemetry_enabled || !host_frame_log_detail())
		return;
	now = host_frame_log_now();
	view = window_index >= 0 && window_index < HOST_VIEWS - 1 ? window_index : HOST_VIEWS - 1;
	if (host_frame_acc.view_started_ns)
		host_frame_acc.views[host_frame_acc.view].ns += now - host_frame_acc.view_started_ns;
	host_frame_acc.view = view;
	host_frame_acc.view_started_ns = now;
	host_frame_acc.views[view].windows++;
}

void host_telemetry_game(const struct metal_telemetry_game *game)
{
	if (!game)
		return;
	/* (the player views drawn changed: an event, the frame log's too,
	which is where tools/tvos_bench.py's split-screen scene begins) */
	if (game->windows != telemetry.game.windows && (host_telemetry_enabled || host_frame_log_level))
	{
		char detail[16];

		snprintf(detail, sizeof(detail), "%d", game->windows);
		telemetry.game.windows = game->windows;
		host_frame_log_event("views", detail);
	}
	if (host_telemetry_enabled)
		telemetry.game = *game;
}

static void second_add(struct second *second, uint64_t serial, uint64_t wall_ns,
	const struct host_frame_accumulators *frame)
{
	uint64_t waits = frame->wait_inflight_ns + frame->wait_drawable_ns + frame->wait_readback_ns;
	int64_t guest = (int64_t)wall_ns - (int64_t)frame->host_ns - (int64_t)frame->sleep_ns;
	int64_t host = (int64_t)frame->host_ns - (int64_t)frame->compile_ns - (int64_t)waits;
	uint32_t index, views = 0;

	if (guest < 0)
		guest = 0;
	if (host < 0)
		host = 0;
	if (second->kept < SECOND_FRAMES)
	{
		struct frame_numbers *numbers = &second->numbers[second->kept++];

		numbers->serial = serial;
		numbers->wall_us = (uint32_t)(wall_ns / 1000);
		numbers->guest_us = (uint32_t)(guest / 1000);
		numbers->host_us = (uint32_t)(host / 1000);
	}
	second->frames++;
	second->over16 += wall_ns > 16700000u;
	second->over33 += wall_ns > 33300000u;
	second->over50 += wall_ns > 50000000u;
	second->missed_slots += host_telemetry_missed_slots(wall_ns, telemetry.slot_ns);
	second->wall_ns += wall_ns;
	second->guest_ns += (uint64_t)guest;
	second->host_ns += (uint64_t)host;
	second->compile_ns += frame->compile_ns;
	second->wait_drawable_ns += frame->wait_drawable_ns;
	second->wait_inflight_ns += frame->wait_inflight_ns;
	second->wait_readback_ns += frame->wait_readback_ns;
	second->sleep_ns += frame->sleep_ns;
	second->compiles += frame->compiles;
	second->draws += frame->draws;
	second->passes += frame->passes;
	second->copies += frame->copies;
	second->shadow_draws += frame->shadow_draws;
	second->fog_draws += frame->fog_draws;
	second->transparent_draws += frame->transparent_draws;
	second->texture_uploads += frame->texture_uploads;
	second->texture_conversions += frame->texture_conversions;
	second->texture_deletes += frame->texture_deletes;
	second->ring_overflows += frame->ring_overflows;
	second->texture_upload_bytes += frame->texture_upload_bytes;
	second->texture_conversion_bytes += frame->texture_conversion_bytes;
	second->texture_conversion_ns += frame->texture_conversion_ns;
	second->stream_bytes += frame->stream_bytes;
	if (frame->stream_bytes > second->stream_most)
		second->stream_most = frame->stream_bytes;
	second->file_reads += frame->file_reads;
	second->file_writes += frame->file_writes;
	second->file_read_bytes += frame->file_read_bytes;
	second->file_write_bytes += frame->file_write_bytes;
	second->file_ns += frame->file_ns;
	second->manifest_writes += frame->manifest_writes;
	second->manifest_bytes += frame->manifest_bytes;
	second->manifest_ns += frame->manifest_ns;
	for (index = 0; index < HOST_VIEWS; index++)
	{
		const struct host_view_accumulators *view = &frame->views[index];
		struct host_view_accumulators *sum = &second->views[index];

		views += index < HOST_VIEWS - 1 && view->windows;
		sum->draws += view->draws;
		sum->passes += view->passes;
		sum->shadow_draws += view->shadow_draws;
		sum->fog_draws += view->fog_draws;
		sum->transparent_draws += view->transparent_draws;
		sum->windows += view->windows;
		sum->ns += view->ns;
	}
	if (views > second->views_most)
		second->views_most = views;
}

/* a second's record (and its hitches'), with the GPU's times now in */
static void second_write(struct second *second)
{
	struct host_telemetry_platform platform;
	uint32_t wall[SECOND_FRAMES], guest[SECOND_FRAMES], host[SECOND_FRAMES], gpu[SECOND_FRAMES], span[SECOND_FRAMES];
	uint32_t index, gpu_count = 0, kept = second->kept;
	uint64_t gpu_total = 0, hoisted_total = 0;
	double frames = second->frames ? (double)second->frames : 1.0;
	double duration = second->end_ns > second->start_ns ? (second->end_ns - second->start_ns) / 1e9 : 1.0;
	const struct metal_telemetry_game *game = &second->game;
	char map[40];
	int players, view_count;

	for (index = 0; index < kept; index++)
	{
		uint64_t gpu_ns, hoisted_ns, span_ns;
		uint32_t buffers;

		wall[index] = second->numbers[index].wall_us;
		guest[index] = second->numbers[index].guest_us;
		host[index] = second->numbers[index].host_us;
		if (host_frame_log_gpu_of(second->numbers[index].serial, &gpu_ns, &buffers, &hoisted_ns, &span_ns) && buffers)
		{
			gpu[gpu_count] = (uint32_t)(gpu_ns / 1000);
			span[gpu_count] = (uint32_t)(span_ns / 1000);
			gpu_count++;
			gpu_total += gpu_ns;
			hoisted_total += hoisted_ns;
		}
	}
	host_telemetry_platform_sample(&platform);
	if (platform.footprint)
	{
		if (!telemetry.footprint_first)
			telemetry.footprint_first = platform.footprint;
		if (platform.footprint > telemetry.footprint_most)
			telemetry.footprint_most = platform.footprint;
	}
	if (telemetry.thermal >= 0 && platform.thermal != telemetry.thermal)
	{
		append("{\"k\":\"event\",\"t\":%.3f,\"kind\":\"thermal\",\"detail\":\"%s\",\"from\":\"%s\"}\n",
			seconds_since_start(second->end_ns), thermal_name(platform.thermal), thermal_name(telemetry.thermal));
	}
	telemetry.thermal = platform.thermal;
	telemetry.active = platform.active;
	clean(map, sizeof(map), game->map);
	players = game->local_players;
	view_count = (int)second->views_most;
	append("{\"k\":\"s\",\"t\":%.3f,\"dur\":%.3f,\"unix\":%lld,\"game_ticks\":%d,\"map\":\"%s\",\"bsp\":%d,"
		"\"players\":%d,\"windows\":%d,\"views\":%d,\"cinematic\":%d,\"pos\":[",
		seconds_since_start(second->start_ns), duration,
		(long long)telemetry.start_wall + (long long)seconds_since_start(second->start_ns), game->game_ticks, map,
		game->bsp, players, game->windows, view_count, game->cinematic);
	for (index = 0; index < METAL_TELEMETRY_VIEWS && (int)index < (players > 0 ? players : 1); index++)
	{
		append("%s[%.1f,%.1f,%.1f]", index ? "," : "", game->positions[index][0], game->positions[index][1],
			game->positions[index][2]);
	}
	append("],\"frames\":%u,\"fps\":%.2f,\"wall_p50\":%.2f,\"wall_p95\":%.2f,\"wall_max\":%.2f,"
		"\"over16\":%u,\"over33\":%u,\"over50\":%u,\"missed\":%u,",
		second->frames, second->frames / duration, ms_of_us(percentile(wall, kept, 0.50)),
		ms_of_us(percentile(wall, kept, 0.95)), kept ? ms_of_us(wall[kept - 1]) : 0.0, second->over16,
		second->over33, second->over50, second->missed_slots);
	append("\"guest_ms\":%.3f,\"guest_p95\":%.2f,\"guest_max\":%.2f,\"host_ms\":%.3f,\"host_p95\":%.2f,"
		"\"gpu_ms\":%.3f,\"gpu_p50\":%.2f,\"gpu_p95\":%.2f,\"gpu_max\":%.2f,\"gpu_span_p95\":%.2f,\"gpu_frames\":%u,"
		"\"hoisted_ms\":%.3f,",
		ms(second->guest_ns) / frames, ms_of_us(percentile(guest, kept, 0.95)), kept ? ms_of_us(guest[kept - 1]) : 0.0,
		ms(second->host_ns) / frames, ms_of_us(percentile(host, kept, 0.95)),
		gpu_count ? ms(gpu_total) / gpu_count : 0.0, ms_of_us(percentile(gpu, gpu_count, 0.50)),
		ms_of_us(percentile(gpu, gpu_count, 0.95)), gpu_count ? ms_of_us(gpu[gpu_count - 1]) : 0.0,
		ms_of_us(percentile(span, gpu_count, 0.95)), gpu_count, gpu_count ? ms(hoisted_total) / gpu_count : 0.0);
	append("\"compile_ms\":%.3f,\"compiles\":%u,\"wait_drawable_ms\":%.3f,\"wait_inflight_ms\":%.3f,"
		"\"wait_readback_ms\":%.3f,\"sleep_ms\":%.3f,\"draws\":%.0f,\"passes\":%.1f,\"copies\":%.1f,"
		"\"shadow_draws\":%.1f,\"fog_draws\":%.1f,\"transparent_draws\":%.1f,",
		ms(second->compile_ns), second->compiles, ms(second->wait_drawable_ns) / frames,
		ms(second->wait_inflight_ns) / frames, ms(second->wait_readback_ns) / frames, ms(second->sleep_ns) / frames,
		second->draws / frames, second->passes / frames, second->copies / frames, second->shadow_draws / frames,
		second->fog_draws / frames, second->transparent_draws / frames);
	append("\"view\":[");
	for (index = 0; index < HOST_VIEWS; index++)
	{
		const struct host_view_accumulators *view = &second->views[index];

		append("%s{\"draws\":%.0f,\"passes\":%.1f,\"shadow\":%.1f,\"fog\":%.1f,\"transparent\":%.1f,\"ms\":%.3f}",
			index ? "," : "", view->draws / frames, view->passes / frames, view->shadow_draws / frames,
			view->fog_draws / frames, view->transparent_draws / frames, ms(view->ns) / frames);
	}
	append("],\"tex_uploads\":%u,\"tex_upload_mb\":%.3f,\"tex_converts\":%u,\"tex_convert_mb\":%.3f,"
		"\"tex_convert_ms\":%.3f,\"tex_deletes\":%u,\"stream_mb\":%.3f,\"stream_frame_max_mb\":%.3f,"
		"\"ring_overflows\":%u,\"file_reads\":%u,\"file_read_mb\":%.3f,\"file_writes\":%u,\"file_write_mb\":%.3f,"
		"\"file_ms\":%.3f,\"manifest_writes\":%u,\"manifest_kb\":%.2f,\"manifest_ms\":%.3f,",
		second->texture_uploads, second->texture_upload_bytes / 1048576.0, second->texture_conversions,
		second->texture_conversion_bytes / 1048576.0, ms(second->texture_conversion_ns), second->texture_deletes,
		second->stream_bytes / 1048576.0, second->stream_most / 1048576.0, second->ring_overflows, second->file_reads,
		second->file_read_bytes / 1048576.0, second->file_writes, second->file_write_bytes / 1048576.0,
		ms(second->file_ns), second->manifest_writes, second->manifest_bytes / 1024.0, ms(second->manifest_ns));
	append("\"footprint_mb\":%.1f,\"available_mb\":%.1f,\"metal_mb\":%.1f,\"thermal\":%d,\"active\":%d,"
		"\"render\":[%u,%u],\"drawable\":[%u,%u],\"scale\":%.3f,\"self_ms\":%.3f,\"writer_ms\":%.3f}\n",
		platform.footprint / 1048576.0, platform.available / 1048576.0, platform.metal_allocated / 1048576.0,
		platform.thermal, platform.active, second->render_width, second->render_height, second->drawable_width,
		second->drawable_height, second->drawable_height ? (double)second->render_height / second->drawable_height : 0.0,
		ms(second->self_ns), ms(atomic_exchange(&telemetry.writer_ns, 0)));
	for (index = 0; index < second->hitch_count; index++)
	{
		const struct hitch *hitch = &second->hitches[index];
		const struct host_frame_accumulators *frame = &hitch->frame;
		uint64_t waits = frame->wait_inflight_ns + frame->wait_drawable_ns + frame->wait_readback_ns;
		int64_t guest_ns = (int64_t)hitch->wall_ns - (int64_t)frame->host_ns - (int64_t)frame->sleep_ns;
		int64_t host_ns = (int64_t)frame->host_ns - (int64_t)frame->compile_ns - (int64_t)waits;
		uint64_t gpu_ns = 0, hoisted_ns = 0, span_ns = 0;
		uint32_t buffers = 0;
		char event[64];

		host_frame_log_gpu_of(hitch->serial, &gpu_ns, &buffers, &hoisted_ns, &span_ns);
		clean(event, sizeof(event), hitch->event);
		append("{\"k\":\"hitch\",\"t\":%.3f,\"serial\":%llu,\"map\":\"%s\",\"bsp\":%d,\"views\":%d,\"wall_ms\":%.2f,"
			"\"guest_ms\":%.2f,\"host_ms\":%.2f,\"gpu_ms\":%.2f,\"gpu_span_ms\":%.2f,\"compile_ms\":%.2f,"
			"\"compiles\":%u,\"wait_drawable_ms\":%.2f,\"wait_inflight_ms\":%.2f,\"wait_readback_ms\":%.2f,"
			"\"sleep_ms\":%.2f,\"draws\":%u,\"passes\":%u,",
			seconds_since_start(hitch->at_ns), (unsigned long long)hitch->serial, map, game->bsp, view_count,
			ms(hitch->wall_ns), guest_ns > 0 ? ms((uint64_t)guest_ns) : 0.0, host_ns > 0 ? ms((uint64_t)host_ns) : 0.0,
			ms(gpu_ns), ms(span_ns), ms(frame->compile_ns), frame->compiles, ms(frame->wait_drawable_ns),
			ms(frame->wait_inflight_ns), ms(frame->wait_readback_ns), ms(frame->sleep_ns), frame->draws, frame->passes);
		append("\"tex_uploads\":%u,\"tex_upload_kb\":%.1f,\"tex_converts\":%u,\"tex_convert_kb\":%.1f,"
			"\"tex_convert_ms\":%.2f,\"tex_deletes\":%u,\"stream_mb\":%.3f,\"ring_overflows\":%u,\"file_reads\":%u,"
			"\"file_read_kb\":%.1f,\"file_writes\":%u,\"file_ms\":%.2f,\"manifest_writes\":%u,\"manifest_ms\":%.2f,"
			"\"events\":%u,\"event\":\"%s\",\"thermal\":%d}\n",
			frame->texture_uploads, frame->texture_upload_bytes / 1024.0, frame->texture_conversions,
			frame->texture_conversion_bytes / 1024.0, ms(frame->texture_conversion_ns), frame->texture_deletes,
			frame->stream_bytes / 1048576.0, frame->ring_overflows, frame->file_reads, frame->file_read_bytes / 1024.0,
			frame->file_writes, ms(frame->file_ns), frame->manifest_writes, ms(frame->manifest_ns), hitch->events, event,
			platform.thermal);
	}
	telemetry.seconds++;
	telemetry.hitches += second->hitch_count;
}

void host_telemetry_frame(uint64_t serial, uint64_t now_ns, uint64_t wall_ns,
	const struct host_frame_accumulators *frame)
{
	struct second *second = telemetry.current;
	uint64_t started = host_frame_log_now();
	uint32_t events;
	static uint64_t hitch_ns;

	if (!host_telemetry_enabled || !second)
		return;
	if (!hitch_ns)
	{
		const char *value = getenv("HALO_TELEMETRY_HITCH_MS");

		hitch_ns = (uint64_t)((value && atof(value) > 0 ? atof(value) : 50.0) * 1e6);
	}
	/* (the first frame's wall time is from the renderer's start) */
	if (serial > 1)
	{
		second_add(second, serial, wall_ns, frame);
		telemetry.frames++;
	}
	events = atomic_exchange(&telemetry.events, 0);
	if (serial > 1 && wall_ns > hitch_ns && second->hitch_count < SECOND_HITCHES)
	{
		struct hitch *hitch = &second->hitches[second->hitch_count++];

		hitch->serial = serial;
		hitch->at_ns = now_ns;
		hitch->frame = *frame;
		hitch->wall_ns = wall_ns;
		hitch->events = events;
		pthread_mutex_lock(&event_lock);
		snprintf(hitch->event, sizeof(hitch->event), "%s", events ? telemetry.last_event : "");
		pthread_mutex_unlock(&event_lock);
	}
	second->game = telemetry.game;
	second->render_width = telemetry.render_width;
	second->render_height = telemetry.render_height;
	second->drawable_width = telemetry.drawable_width;
	second->drawable_height = telemetry.drawable_height;
	if (now_ns - second->start_ns >= 1000000000ull)
	{
		struct second *done = telemetry.waiting;

		second->end_ns = now_ns;
		/* (the second before this one: its frames' GPU times are in by now) */
		if (done->frames)
			second_write(done);
		telemetry.waiting = second;
		memset(done, 0, sizeof(*done));
		done->start_ns = now_ns;
		telemetry.current = done;
		host_telemetry_flush();
	}
	second->self_ns += host_frame_log_now() - started;
	telemetry.self_ns += host_frame_log_now() - started;
}

void host_telemetry_finish(void)
{
	uint64_t now;

	if (!host_telemetry_enabled)
		return;
	now = host_frame_log_now();
	if (telemetry.waiting && telemetry.waiting->frames)
		second_write(telemetry.waiting);
	if (telemetry.current && telemetry.current->frames)
	{
		telemetry.current->end_ns = now;
		second_write(telemetry.current);
	}
	append("{\"k\":\"end\",\"t\":%.3f,\"seconds\":%llu,\"frames\":%llu,\"hitches\":%llu,\"self_ms\":%.1f,"
		"\"written_kb\":%.1f,\"dropped_kb\":%.1f,\"footprint_first_mb\":%.1f,\"footprint_most_mb\":%.1f}\n",
		seconds_since_start(now), (unsigned long long)telemetry.seconds, (unsigned long long)telemetry.frames,
		(unsigned long long)telemetry.hitches, ms(telemetry.self_ns), atomic_load(&telemetry.writer_bytes) / 1024.0,
		atomic_load(&telemetry.dropped_bytes) / 1024.0, telemetry.footprint_first / 1048576.0,
		telemetry.footprint_most / 1048576.0);
	host_telemetry_flush();
	host_telemetry_enabled = 0;
	dispatch_sync_f(telemetry.queue, NULL, writer_close);
	host_log("telemetry: %llu seconds, %llu hitches, %.1f ms of its own on the main thread: %s",
		(unsigned long long)telemetry.seconds, (unsigned long long)telemetry.hitches, ms(telemetry.self_ns),
		telemetry.path);
}
