/*
HOST_METAL.M

The macOS build's renderer (port/macos/README.md): Metal, on behalf of the
guest's Direct3D device (port/macos/guest/d3d8_metal.c), which describes
each draw, clear and texture in the structs of
port/macos/guest/metal_protocol.h. Every Metal object lives here, behind a
small handle the guest holds.

Frames. Up to FRAMES_IN_FLIGHT frames are on the GPU at once, each with a
command buffer, committed at presentation, and a ring buffer that a draw's
vertices and indices are copied into: the guest may rewrite its memory as
soon as a call returns, as the Xbox's D3D lets the game rewrite a dynamic
buffer once a draw is queued. The uniforms go with the draw
(setVertexBytes), and only when they have changed (their serials).

Render passes. A pass is begun lazily for the targets the guest sets
(host_metal_set_targets), so that a clear of the whole of them before
anything is drawn is the pass's load action (free on Apple's tiled GPUs)
rather than a quad; a clear after draws draws one. Uploads into a texture a
queued draw reads, copies between textures and mipmaps end the pass: they
are blits, in the frame's command buffer, in order with the draws.

Visibility tests. Metal counts a draw's samples into a visibility result
buffer at an offset, written when the pass ends, not added to: a test whose
draws span passes gets a counter in each, and the frame's counters are
summed for each test when the GPU has finished it. Each of the game's test
indices keeps its latest count (host_metal_visibility_result).
*/

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <mach-o/dyld.h>
#include <mach-o/loader.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "host_dynres.h"
#include "host_frame_log.h"
#include "host_lifecycle.h"
#include "host_shader_manifest.h"
#include "host_telemetry.h"
#include "host_textures.h"
#include "metal_protocol.h"

SDL_Window *host_sdl_window(unsigned int window);

#define FRAMES_IN_FLIGHT 3
/* each frame's ring (vertices, constants, texture staging): on the Apple TV
24 MiB, where a frame streams at most ~16 MB (a10 and b30 split screen,
the play telemetry's stream_frame_max_mb) and the three rings are memory
the app keeps for good (64 MiB each was 192 MiB of its ~1.4 GB); a frame
past it takes overflow buffers of its own (ring_allocate, ring_overflows) */
#ifdef HALO_TVOS
#define RING_BYTES (24u << 20)
#else
#define RING_BYTES (64u << 20)
#endif
#define HANDLE_COUNT 65536
/* a frame's visibility tests, and its counters (a test's in each pass) */
#define VISIBILITY_TESTS 4096
#define VISIBILITY_COUNTERS 16384

/* ---------- handles */

enum
{
	_handle_free,
	_handle_texture,
	_handle_shader,
	_handle_layout,
};

struct handle
{
	int type;
	/* (retained: CFBridgingRetain) */
	void *object;
	/* a texture's: the GPU's alone (a target, a composite: blits only), a
	target nothing has drawn into yet (cleared by its first pass), and the
	frame that last drew with it */
	int private_storage;
	int fresh;
	uint64_t used_serial;
	/* the command buffer (park.generation) that last read it and wrote it:
	whether a pass can be hoisted past what it has encoded (below) */
	uint32_t commands_read, commands_written;
	/* (HALO_METAL_DEPTH_DISCARD) a depth target whose last pass did not
	store it */
	int discarded;
};

static struct handle handles[HANDLE_COUNT];
static uint32_t handle_hint = 1;

static uint32_t handle_new(int type)
{
	uint32_t probe;

	for (probe = 0; probe < HANDLE_COUNT - 1; probe++)
	{
		uint32_t index = 1 + (handle_hint + probe - 1) % (HANDLE_COUNT - 1);

		if (handles[index].type == _handle_free)
		{
			memset(&handles[index], 0, sizeof(handles[index]));
			handles[index].type = type;
			handle_hint = index + 1;
			return index;
		}
	}
	host_log("Metal: out of handles");
	return 0;
}

static void handle_set(uint32_t index, id object)
{
	if (handles[index].object)
		CFRelease(handles[index].object);
	handles[index].object = object ? (void *)CFBridgingRetain(object) : NULL;
}

static id handle_object(uint32_t index, int type)
{
	if (!index || index >= HANDLE_COUNT || handles[index].type != type || !handles[index].object)
		return nil;
	return (__bridge id)handles[index].object;
}

/* ---------- caches of Metal state objects, by the bytes of their key */

struct cache_entry
{
	struct cache_entry *next;
	uint64_t hash;
	void *object;
	uint32_t size;
	unsigned char key[];
};

#define CACHE_BUCKETS 4096

struct cache
{
	struct cache_entry *buckets[CACHE_BUCKETS];
};

static uint64_t hash_bytes(const void *data, size_t size)
{
	const unsigned char *bytes = data;
	uint64_t hash = 1469598103934665603ULL;
	size_t index;

	for (index = 0; index < size; index++)
		hash = (hash ^ bytes[index]) * 1099511628211ULL;
	return hash;
}

/* the entry of a key, or NULL; found says whether there is one (a failed
object is kept as NULL, rather than made again at every draw) */
static void *cache_find(struct cache *cache, const void *key, uint32_t size, uint64_t *hash_out, BOOL *found)
{
	uint64_t hash = hash_bytes(key, size);
	struct cache_entry *entry;

	*hash_out = hash;
	for (entry = cache->buckets[hash % CACHE_BUCKETS]; entry; entry = entry->next)
	{
		if (entry->hash == hash && entry->size == size && !memcmp(entry->key, key, size))
		{
			*found = YES;
			return entry->object;
		}
	}
	*found = NO;
	return NULL;
}

/* (how many objects the caches hold: host_metal_memory) */
static uint32_t cache_objects;

static void cache_add(struct cache *cache, const void *key, uint32_t size, uint64_t hash, id object)
{
	struct cache_entry *entry = malloc(sizeof(*entry) + size);

	if (!entry)
		return;
	cache_objects++;
	entry->hash = hash;
	entry->size = size;
	entry->object = object ? (void *)CFBridgingRetain(object) : NULL;
	memcpy(entry->key, key, size);
	entry->next = cache->buckets[hash % CACHE_BUCKETS];
	cache->buckets[hash % CACHE_BUCKETS] = entry;
}

static struct cache pipelines, depth_stencils, samplers, clear_pipelines;

/* ---------- the device */

static struct
{
	id<MTLDevice> device;
	id<MTLCommandQueue> queue;
	SDL_MetalView view;
	CAMetalLayer *layer;
	SDL_Window *window;
	int vsync;

	/* the renderer's own shaders: clears and the window's picture */
	id<MTLFunction> clear_vertex, clear_fragment, white_fragment;
	/* HALO_METAL_DEBUG: 1 draws without culling, 2 without the depth and
	stencil tests, 4 every pixel white (finding what hides a draw) */
	int debug;
	id<MTLRenderPipelineState> present_pipeline;
	id<MTLSamplerState> present_sampler;
	/* what a stage with no texture samples, of each kind */
	id<MTLTexture> dummy_2d, dummy_cube, dummy_3d;
	/* the colour attachment of a pass with a depth target alone, and the
	copy of a target a draw samples while it draws into it */
	id<MTLTexture> dummy_color;
	id<MTLTexture> feedback;

	/* the picture to show at the next present */
	uint32_t present_source;
	uint32_t present_width, present_height;

	/* dynamic resolution (host_dynres.h): the upscales' pipelines (bilinear,
	sharp; made when first used), the fraction this frame's scenes drew at
	(-1: none), the display's refresh interval, and the frame's GPU span so
	far (its hoisted buffers', to add to its own) */
	id<MTLRenderPipelineState> upscale_pipelines[2];
	id<MTLFunction> upscale_vertex, upscale_fragments[2];
	float dynres_fraction;
	uint64_t slot_ns;
	int thermal, ceiling_render;
} metal = { .dynres_fraction = -1.0f };

/* ---------- frames */

struct frame
{
	id<MTLBuffer> ring;
	uint32_t ring_used;
	/* past the ring's end: buffers of their own, for this frame */
	NSMutableArray *overflow;
	id<MTLBuffer> overflow_current;
	uint32_t overflow_used;
	/* command buffers of passes hoisted ahead of a large one (park, below) */
	NSMutableArray *hoisted;

	id<MTLBuffer> visibility;
	/* each test's index (the game's), whether it has ended, and each
	counter's test */
	uint32_t test_count;
	uint16_t test_index[VISIBILITY_TESTS];
	uint8_t test_ended[VISIBILITY_TESTS];
	uint32_t counter_count;
	uint16_t counter_test[VISIBILITY_COUNTERS];
	uint64_t sums[VISIBILITY_TESTS];
};

static struct frame frames[FRAMES_IN_FLIGHT];
static dispatch_semaphore_t frames_free;
static id<MTLCommandBuffer> commands;
/* a number of each command buffer the frame encodes into (handles' commands_read) */
static uint32_t commands_generation = 1, commands_generations = 1;
static uint64_t frame_serial = 1;
static _Atomic uint64_t completed_serial;
/* (host_lifecycle.h's, at the end) */
static id<MTLCommandBuffer> uploads_last;
/* the frame that last copied into the feedback copy: the lifecycle's
trim and host_metal_present let it go once it has not for a while */
static uint64_t feedback_serial;

/* HALO_METAL_PROFILE: command-buffer GPU time and feedback-copy bandwidth,
   sampled without forcing CPU/GPU synchronization. Off by default. */
static int profile_enabled;
static uint64_t profile_feedback_copies, profile_feedback_bytes;
static uint64_t profile_effect_frame;
static int profile_effect_type, profile_effect_passes, profile_effect_video;
static float profile_effect_radius;
static _Atomic uint64_t profile_last_completion_ns;
static _Atomic uint64_t profile_last_completion_serial;
static uint64_t profile_effect_calls, profile_convolution_frames, profile_interval_copies;
static uint64_t profile_last_convolution_frame;
static int profile_phase = -1;
static uint32_t profile_phase_draws[29];
static uint32_t profile_phase_copies[29];
static uint64_t profile_shader_ns, profile_pipeline_ns;

/* each test index's latest count, and whether it has one */
static _Atomic uint32_t index_count[METAL_VISIBILITY_SLOTS];
static _Atomic uint32_t index_known[METAL_VISIBILITY_SLOTS];

static struct frame *frame_current(void)
{
	return &frames[frame_serial % FRAMES_IN_FLIGHT];
}

static void frame_begin(void)
{
	struct frame *frame;

	if (commands)
		return;
	/* (the frame FRAMES_IN_FLIGHT before this one, which had these buffers,
	finished first) */
	if (host_frame_log_detail())
	{
		uint64_t waited = host_frame_log_now();

		dispatch_semaphore_wait(frames_free, DISPATCH_TIME_FOREVER);
		host_frame_acc.wait_inflight_ns += host_frame_log_now() - waited;
	}
	else
	{
		dispatch_semaphore_wait(frames_free, DISPATCH_TIME_FOREVER);
	}
	frame = frame_current();
	if (!frame->ring)
	{
		frame->ring = [metal.device newBufferWithLength:RING_BYTES
			options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
		frame->visibility = [metal.device newBufferWithLength:VISIBILITY_COUNTERS * 8
			options:MTLResourceStorageModeShared];
	}
	frame->ring_used = 0;
	frame->overflow = nil;
	frame->overflow_current = nil;
	frame->overflow_used = 0;
	frame->test_count = 0;
	frame->counter_count = 0;
	frame->hoisted = nil;
	commands = [metal.queue commandBuffer];
	commands_generation = ++commands_generations;
}

/* room for bytes in the frame's ring (or a buffer of its own past it): the
buffer, the offset in it and where to write */
static void *ring_allocate(uint32_t bytes, uint32_t alignment, id<MTLBuffer> *buffer, uint32_t *offset)
{
	struct frame *frame;
	uint32_t at;

	frame_begin();
	frame = frame_current();
	host_frame_acc.stream_bytes += bytes;
	at = (frame->ring_used + alignment - 1) & ~(alignment - 1);
	if ((uint64_t)at + bytes <= RING_BYTES)
	{
		frame->ring_used = at + bytes;
		*buffer = frame->ring;
		*offset = at;
		return (unsigned char *)frame->ring.contents + at;
	}
	at = (frame->overflow_used + alignment - 1) & ~(alignment - 1);
	if (!frame->overflow_current || (uint64_t)at + bytes > frame->overflow_current.length)
	{
		NSUInteger length = bytes > (16u << 20) ? bytes : (16u << 20);

		frame->overflow_current = [metal.device newBufferWithLength:length options:MTLResourceStorageModeShared];
		host_frame_acc.ring_overflows++;
		if (!frame->overflow)
			frame->overflow = [NSMutableArray array];
		[frame->overflow addObject:frame->overflow_current];
		at = 0;
	}
	frame->overflow_used = at + bytes;
	*buffer = frame->overflow_current;
	*offset = at;
	return (unsigned char *)frame->overflow_current.contents + at;
}

/* ---------- render passes */

static struct
{
	uint32_t color, depth;
	MTLLoadAction color_load, depth_load, stencil_load;
	MTLClearColor clear_color;
	double clear_depth;
	uint32_t clear_stencil;
	uint32_t width, height;
	MTLPixelFormat color_format, depth_format;
} pass;

static id<MTLRenderCommandEncoder> encoder;
static uint64_t encoder_serial, encoder_serials;

/* the queued transparents at half resolution (host_metal_halfres_begin,
below): while a run is drawn, its pass has two more colour attachments
than the guest knows of, multiply (the factor the run multiplies what is
under it by) and depths (each half-resolution pixel's own pixels' depths,
for the upsample) */
static struct
{
	BOOL active, reduced;
	uint32_t accumulate, depth;
	uint32_t screen_color, screen_depth;
	uint32_t x, y, width, height, step;
	id<MTLTexture> multiply, depths;
	/* the run's draws' stencil test, which the composite repeats at full
	resolution (set: one was seen) */
	struct metal_depth_stencil stencil;
	uint32_t stencil_reference, stencil_set;
	uint64_t runs, draws;
} halfres;

/* the pixel shaders' sources, for their variants that half-resolution runs draw with */
static NSMutableDictionary<NSNumber *, NSString *> *halfres_sources;

/* HALO_METAL_DEPTH_DISCARD=present (experimental, off by default): the
depth and stencil of the pass that ends at the present not stored (store
actions decided at the pass's end). Correct only if every frame's first use
of the depth target clears it, which the game's 3D windows do
(rasterizer_window_begin) but nothing guarantees; a load of a depth target
so discarded is counted and logged (depth_discard.misses), and turns it off */
static struct
{
	int enabled, broken;
	/* =toggle:N (diagnostic): on and off every N frames */
	uint32_t toggle;
	uint32_t depth;
	uint64_t passes, misses;
} depth_discard;

/* ---------- the pass log (HALO_PASS_LOG, host_frame_log.h): diagnostic

Each pass's record, and its GPU time: the GPU's timestamps at the start and
end of the pass's vertex and fragment stages (a counter sample buffer a
frame in flight) where the device samples at stage boundaries, else with
HALO_METAL_PASS_TIMING=buffers a command buffer a pass (which itself costs
some time). HALO_METAL_PASS_SPLIT=phase ends a pass whenever the
rasterizer's phase changes, to time the phases apart (it adds the passes'
loads and stores: diagnostic only), and HALO_METAL_SKIP_PHASES (phase
numbers, comma-separated) drops their draws, to measure what they cost.
None changes anything unless set. */

#define PASS_TIMING_PASSES 256

enum
{
	_pass_end_other,
	_pass_end_targets,
	_pass_end_blit,
	_pass_end_present,
	_pass_end_phase,
	_pass_end_flush,
	_pass_end_unpark,
};

static struct
{
	int timing; /* 0 none, 1 stage timestamps, 2 a command buffer a pass */
	int split_phases;
	uint32_t skip_phases;
	int end_reason;
	uint32_t index; /* the frame's passes so far */
	struct host_pass_record *record;
	struct host_pass_record *frame_records[FRAMES_IN_FLIGHT][PASS_TIMING_PASSES];
	uint32_t frame_samples[FRAMES_IN_FLIGHT];
	uint64_t frame_serials[FRAMES_IN_FLIGHT];
	id<MTLCounterSampleBuffer> samples[FRAMES_IN_FLIGHT];
} pass_log;

static void pass_log_initialize(void)
{
	const char *setting;

	setting = getenv("HALO_METAL_SKIP_PHASES");
	while (setting && *setting)
	{
		int phase = atoi(setting);

		if (phase >= 0 && phase < 29)
			pass_log.skip_phases |= 1u << phase;
		setting = strchr(setting, ',');
		if (setting)
			setting++;
	}
	if (pass_log.skip_phases)
		host_log("Metal: HALO_METAL_SKIP_PHASES (diagnostic): the draws of phases %08x are not drawn",
			pass_log.skip_phases);
	if (!host_pass_log_enabled)
		return;
	pass_log.split_phases = getenv("HALO_METAL_PASS_SPLIT") && !strcmp(getenv("HALO_METAL_PASS_SPLIT"), "phase");
	setting = getenv("HALO_METAL_PASS_TIMING");
	if (setting && !strcmp(setting, "buffers"))
	{
		pass_log.timing = 2;
	}
	else if (!setting || strcmp(setting, "none"))
	{
		id<MTLCounterSet> timestamps = nil;

		if (@available(macOS 11.0, iOS 14.0, tvOS 14.0, *))
		{
			for (id<MTLCounterSet> set in metal.device.counterSets)
			{
				if ([set.name isEqualToString:MTLCommonCounterSetTimestamp])
					timestamps = set;
			}
			host_log("Metal: counter sampling: stage %d draw %d blit %d; timestamp counters %s",
				[metal.device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary],
				[metal.device supportsCounterSampling:MTLCounterSamplingPointAtDrawBoundary],
				[metal.device supportsCounterSampling:MTLCounterSamplingPointAtBlitBoundary], timestamps ? "yes" : "no");
			if (timestamps && [metal.device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary])
			{
				MTLCounterSampleBufferDescriptor *descriptor = [MTLCounterSampleBufferDescriptor new];
				NSError *error = nil;
				int frame;

				descriptor.counterSet = timestamps;
				descriptor.storageMode = MTLStorageModeShared;
				descriptor.sampleCount = PASS_TIMING_PASSES * 4;
				pass_log.timing = 1;
				for (frame = 0; frame < FRAMES_IN_FLIGHT; frame++)
				{
					pass_log.samples[frame] = [metal.device newCounterSampleBufferWithDescriptor:descriptor error:&error];
					if (!pass_log.samples[frame])
					{
						host_log("Metal: no counter sample buffer: %s", error.localizedDescription.UTF8String);
						pass_log.timing = 0;
					}
				}
			}
		}
	}
	host_log("Metal: pass log on; GPU time a pass: %s%s", pass_log.timing == 1 ? "stage timestamps" :
		pass_log.timing == 2 ? "a command buffer a pass" : "none (no stage counters: HALO_METAL_PASS_TIMING=buffers)",
		pass_log.split_phases ? "; passes split at phases (diagnostic)" : "");
}

/* the GPU's clock against the CPU's, for converting counter ticks */
static void pass_log_clock(void)
{
	if (pass_log.timing != 1)
		return;
	if (@available(macOS 10.15, iOS 14.0, tvOS 14.0, *))
	{
		MTLTimestamp cpu = 0, gpu = 0;
		uint64_t before = host_frame_log_now(), after;

		[metal.device sampleTimestamps:&cpu gpuTimestamp:&gpu];
		after = host_frame_log_now();
		host_frame_log_gpu_clock(before + (after - before) / 2, gpu);
	}
}

/* a new pass's record, and its counter samples in the descriptor */
static void pass_log_begin(MTLRenderPassDescriptor *descriptor)
{
	struct host_pass_record *record;
	uint32_t slot = (uint32_t)(frame_serial % FRAMES_IN_FLIGHT);

	pass_log.record = NULL;
	if (!host_pass_log_enabled || !host_frame_log_main_thread())
		return;
	/* (a slot's samples are the frame's that uses it now: the one before
	in it has completed, frame_begin waited for that) */
	if (pass_log.frame_serials[slot] != frame_serial)
	{
		pass_log.frame_serials[slot] = frame_serial;
		pass_log.frame_samples[slot] = 0;
		pass_log.index = 0;
	}
	record = host_frame_log_pass_new(frame_serial, pass_log.index++);
	if (!record)
		return;
	record->color = pass.color;
	record->depth = pass.depth;
	record->width = pass.width;
	record->height = pass.height;
	record->color_format = (uint32_t)descriptor.colorAttachments[0].texture.pixelFormat;
	record->depth_format = (uint32_t)descriptor.depthAttachment.texture.pixelFormat;
	record->color_load = (uint8_t)descriptor.colorAttachments[0].loadAction;
	record->color_store = (uint8_t)descriptor.colorAttachments[0].storeAction;
	if (descriptor.depthAttachment.texture)
	{
		record->depth_load = (uint8_t)descriptor.depthAttachment.loadAction;
		record->depth_store = (uint8_t)descriptor.depthAttachment.storeAction;
		record->stencil_load = (uint8_t)descriptor.stencilAttachment.loadAction;
		record->stencil_store = (uint8_t)descriptor.stencilAttachment.storeAction;
	}
	pass_log.record = record;
	if (pass_log.timing == 1 && pass_log.frame_samples[slot] < PASS_TIMING_PASSES)
	{
		if (@available(macOS 11.0, iOS 14.0, tvOS 14.0, *))
		{
			uint32_t at = pass_log.frame_samples[slot]++;
			MTLRenderPassSampleBufferAttachmentDescriptor *attachment = descriptor.sampleBufferAttachments[0];

			attachment.sampleBuffer = pass_log.samples[slot];
			attachment.startOfVertexSampleIndex = at * 4;
			attachment.endOfVertexSampleIndex = at * 4 + 1;
			attachment.startOfFragmentSampleIndex = at * 4 + 2;
			attachment.endOfFragmentSampleIndex = at * 4 + 3;
			pass_log.frame_records[slot][at] = record;
		}
	}
}

/* the pass's end (before endEncoding): why, and its command buffer with
HALO_METAL_PASS_TIMING=buffers */
static void pass_log_end(void)
{
	struct host_pass_record *record = pass_log.record;

	pass_log.record = NULL;
	if (!record)
		return;
	record->reason = (uint8_t)pass_log.end_reason;
	if (pass_log.timing == 2)
	{
		uint64_t serial = atomic_load(&record->serial);

		/* (the frame goes on in a command buffer of its own: its ring and
		counters stay, as host_metal_texture_read's) */
		[commands addCompletedHandler:^(id<MTLCommandBuffer> buffer)
		{
			if (atomic_load(&record->serial) == serial && buffer.GPUEndTime > buffer.GPUStartTime)
			{
				atomic_store(&record->gpu_start_ns, (uint64_t)(buffer.GPUStartTime * 1e9));
				atomic_store(&record->gpu_end_ns, (uint64_t)(buffer.GPUEndTime * 1e9));
			}
		}];
		pass_log.end_reason = _pass_end_other;
		return;
	}
	pass_log.end_reason = _pass_end_other;
}

/* the frame's counter samples, into its passes' records (its command
buffer's completion; the next frame in this slot has not begun) */
static void pass_log_resolve(uint32_t slot, uint64_t serial)
{
	uint32_t count = pass_log.frame_samples[slot], index;

	if (pass_log.timing != 1 || !count)
		return;
	if (@available(macOS 10.15, iOS 14.0, tvOS 14.0, *))
	{
		NSData *data = [pass_log.samples[slot] resolveCounterRange:NSMakeRange(0, count * 4)];
		const MTLCounterResultTimestamp *times = data.bytes;

		for (index = 0; times && index < count && (index + 1) * 4 * sizeof(*times) <= data.length; index++)
		{
			struct host_pass_record *record = pass_log.frame_records[slot][index];

			if (!record || atomic_load(&record->serial) != serial)
				continue;
			atomic_store(&record->vertex_start, times[index * 4].timestamp);
			atomic_store(&record->vertex_end, times[index * 4 + 1].timestamp);
			atomic_store(&record->fragment_start, times[index * 4 + 2].timestamp);
			atomic_store(&record->fragment_end, times[index * 4 + 3].timestamp);
		}
	}
}

/* what the encoder has, to set only what changes */
static struct
{
	void *pipeline, *depth_stencil;
	uint32_t vertex_uniforms_serial, vertex_constants_serial, pixel_uniforms_serial, constant_attributes_serial;
	uint32_t stencil_reference;
	uint32_t cull, winding, wireframe;
	float depth_bias, depth_slope;
	float blend_color[4];
	float viewport[6];
	uint32_t scissor[4];
	void *textures[METAL_STAGES];
	void *samplers[METAL_STAGES];
	/* the visibility counter counting, or ~0 */
	uint32_t counter;
} bound;

/* ---------- hoisting small passes out of a large one

Halo draws its 3D view into the screen-sized target and, between its
phases, into small ones: the shadow maps (128x128 and the like), the water's
bump map and its mipmaps, the HUD's meter textures, mipmap composites. Each
of those ended the screen's pass, and the next draw into the screen began a
new one, loading its colour, depth and stencil from memory and storing them
again at its end: at 3840x2160 about 75 MB each way, three to six times a
frame, which on a tile-based GPU is the bandwidth that matters.

So the screen's pass is parked instead (its encoder left open, in the
frame's command buffer), and the small passes and blits between are encoded
in a command buffer of their own, committed when the screen's targets come
back: before the frame's, so the GPU runs them first, and the screen's pass
goes on as one. Moving work earlier is right only when it does not depend
on what the frame's command buffer has encoded so far, and nothing encoded
there depends on its not having happened yet:
- a hoisted pass or blit may read a texture only if the frame's command
  buffer has not written it (else the pass is unparked: ended, as before);
- it may write a texture only if the frame's command buffer has neither
  written it (unparked) nor read it; one that was read (a shadow map drawn
  again for the next light) is renamed: its handle gets another texture,
  its pixels copied in first, and the draws already encoded keep the old.
Readbacks, presents, feedback copies and the parked targets being redefined
unpark first. HALO_METAL_HOIST=0 turns it off (for comparison). */

#define PARK_LARGE_PIXELS (1u << 20)
#define PARK_RENAME_PIXELS (1024u * 1024u)
#define PARK_POOL 32

static struct
{
	int enabled;
	/* HALO_METAL_HOIST=toggle:N (diagnostic): on and off every N frames,
	for comparing the two in one run */
	uint32_t toggle;
	BOOL active;
	id<MTLRenderCommandEncoder> encoder;
	id<MTLCommandBuffer> commands;
	uint32_t generation;
	uint64_t encoder_serial;
	struct host_pass_record *record;
	uint32_t discard_depth;
	__typeof__(pass) main_pass;
	__typeof__(bound) main_bound;
	/* renamed textures, free again once their frame is done */
	id<MTLTexture> pool[PARK_POOL];
	uint64_t pool_serial[PARK_POOL];
} park;

/* each test's counter in the current pass (frame_current's tests) */
static uint32_t test_counter[VISIBILITY_TESTS];
static uint64_t test_counter_encoder[VISIBILITY_TESTS];

static void bound_forget(void)
{
	memset(&bound, 0xff, sizeof(bound));
	memset(bound.textures, 0, sizeof(bound.textures));
	memset(bound.samplers, 0, sizeof(bound.samplers));
	bound.pipeline = bound.depth_stencil = NULL;
	/* (serials are never 0: the guest's draws always send theirs once) */
	bound.vertex_uniforms_serial = bound.vertex_constants_serial = 0;
	bound.pixel_uniforms_serial = bound.constant_attributes_serial = 0;
	bound.counter = ~0u;
}

static void encoder_end(void)
{
	if (encoder)
	{
		if (depth_discard.enabled && depth_discard.depth)
		{
			MTLStoreAction store = pass_log.end_reason == _pass_end_present ? MTLStoreActionDontCare :
				MTLStoreActionStore;

			[encoder setDepthStoreAction:store];
			[encoder setStencilStoreAction:store];
			handles[depth_discard.depth].discarded = store == MTLStoreActionDontCare;
			depth_discard.passes += store == MTLStoreActionDontCare;
			if (pass_log.record)
				pass_log.record->depth_store = pass_log.record->stencil_store = (uint8_t)store;
		}
		pass_log_end();
		[encoder endEncoding];
		encoder = nil;
		/* (HALO_METAL_PASS_TIMING=buffers: the pass's command buffer
		committed, the frame going on in another, as a readback does) */
		if (pass_log.timing == 2 && commands)
		{
			host_lifecycle_gate();
			[commands commit];
			commands = [metal.queue commandBuffer];
		}
	}
}

static id<MTLTexture> texture_object(uint32_t handle)
{
	return handle_object(handle, _handle_texture);
}

/* a colour attachment for a pass with a depth target alone (the pixel
shaders write a colour), as large as the depth target at least */
static id<MTLTexture> dummy_color(uint32_t width, uint32_t height)
{
	if (!metal.dummy_color || metal.dummy_color.width < width || metal.dummy_color.height < height)
	{
		MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
			texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
			width:width > metal.dummy_color.width ? width : metal.dummy_color.width
			height:height > metal.dummy_color.height ? height : metal.dummy_color.height mipmapped:NO];

		descriptor.usage = MTLTextureUsageRenderTarget;
		descriptor.storageMode = MTLStorageModePrivate;
		metal.dummy_color = [metal.device newTextureWithDescriptor:descriptor];
	}
	return metal.dummy_color;
}

/* ---------- parking (hoisting, above) */

static void park_initialize(void)
{
	const char *setting = getenv("HALO_METAL_HOIST");

	setting = getenv("HALO_METAL_DEPTH_DISCARD");
	depth_discard.enabled = setting && !strcmp(setting, "present");
	if (setting && !strncmp(setting, "toggle:", 7) && atoi(setting + 7) > 0)
		depth_discard.toggle = (uint32_t)atoi(setting + 7);
	setting = getenv("HALO_METAL_HOIST");
	if (depth_discard.enabled || depth_discard.toggle)
		host_log("Metal: HALO_METAL_DEPTH_DISCARD=present (experimental): the depth and stencil of the frame's last "
			"pass are not stored");

	park.enabled = !(setting && !strcmp(setting, "0")) && pass_log.timing != 2;
	if (setting && !strncmp(setting, "toggle:", 7) && atoi(setting + 7) > 0)
	{
		park.toggle = (uint32_t)atoi(setting + 7);
		host_log("Metal: HALO_METAL_HOIST=%s (diagnostic): hoisting on in even blocks of %u frames, off in odd",
			setting, park.toggle);
	}
	if (!park.enabled)
		host_log("Metal: small passes are not hoisted out of the screen's (HALO_METAL_HOIST=0 or pass timing by buffers)");
}

static BOOL texture_small(uint32_t handle)
{
	id<MTLTexture> texture = texture_object(handle);

	return !texture || texture.width * texture.height < PARK_LARGE_PIXELS;
}

/* the open pass, large, left open; what comes next goes into a command
buffer of its own */
static void park_begin(void)
{
	park.encoder = encoder;
	park.discard_depth = depth_discard.depth;
	depth_discard.depth = 0;
	park.main_pass = pass;
	park.main_bound = bound;
	park.record = pass_log.record;
	park.encoder_serial = encoder_serial;
	park.commands = commands;
	park.generation = commands_generation;
	encoder = nil;
	pass_log.record = NULL;
	commands = [metal.queue commandBuffer];
	commands_generation = ++commands_generations;
	park.active = YES;
}

/* the hoisted command buffer, committed: ahead of the frame's */
static void park_commit_hoisted(void)
{
	struct frame *frame = frame_current();

	host_lifecycle_gate();
	[commands commit];
	if (!frame->hoisted)
		frame->hoisted = [NSMutableArray array];
	[frame->hoisted addObject:commands];
	commands = park.commands;
	commands_generation = park.generation;
	park.commands = nil;
	park.active = NO;
}

/* the screen's targets back: its pass goes on */
static void park_resume(void)
{
	pass_log.end_reason = _pass_end_targets;
	encoder_end();
	park_commit_hoisted();
	encoder = park.encoder;
	park.encoder = nil;
	depth_discard.depth = park.discard_depth;
	pass = park.main_pass;
	bound = park.main_bound;
	pass_log.record = park.record;
	encoder_serial = park.encoder_serial;
}

/* something that must come after the parked pass: the hoisted work
committed and the parked pass ended (the current targets, the hoisted
ones, go on in the frame's command buffer, loaded) */
static void park_end(void)
{
	if (!park.active)
		return;
	pass_log.end_reason = _pass_end_unpark;
	encoder_end();
	park_commit_hoisted();
	encoder = park.encoder;
	park.encoder = nil;
	depth_discard.depth = park.discard_depth;
	pass_log.record = park.record;
	encoder_serial = park.encoder_serial;
	pass_log.end_reason = _pass_end_unpark;
	encoder_end();
	bound_forget();
}

/* a target the frame's command buffer has read, given another texture for
the hoisted pass to draw into (its pixels copied in when the pass keeps
them); NO if it cannot be */
static BOOL park_rename(uint32_t handle, BOOL keep)
{
	id<MTLTexture> old = texture_object(handle), fresh = nil;
	uint64_t done = atomic_load(&completed_serial);
	int index, slot = -1;

	if (!old || old.textureType != MTLTextureType2D || old.mipmapLevelCount != 1 || old.sampleCount != 1 ||
		old.width * old.height > PARK_RENAME_PIXELS || !(old.usage & MTLTextureUsageRenderTarget))
	{
		return NO;
	}
	for (index = 0; index < PARK_POOL && !fresh; index++)
	{
		id<MTLTexture> pooled = park.pool[index];

		if (pooled && park.pool_serial[index] <= done && pooled.width == old.width && pooled.height == old.height &&
			pooled.pixelFormat == old.pixelFormat && pooled.usage == old.usage)
		{
			fresh = pooled;
			park.pool[index] = nil;
		}
	}
	if (!fresh)
	{
		MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:old.pixelFormat
			width:old.width height:old.height mipmapped:NO];

		descriptor.usage = old.usage;
		descriptor.storageMode = MTLStorageModePrivate;
		fresh = [metal.device newTextureWithDescriptor:descriptor];
		if (!fresh)
			return NO;
	}
	if (keep)
	{
		id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];

		[blit copyFromTexture:old toTexture:fresh];
		[blit endEncoding];
	}
	/* (the old one free again once this frame is done) */
	for (index = 0; index < PARK_POOL; index++)
	{
		if (!park.pool[index] || park.pool_serial[index] <= done)
		{
			slot = index;
			break;
		}
	}
	if (slot >= 0)
	{
		park.pool[slot] = old;
		park.pool_serial[slot] = frame_serial;
	}
	handle_set(handle, fresh);
	handles[handle].commands_read = 0;
	return YES;
}

/* a pass about to begin while the screen's is parked: hoisted, unless it
depends on what the frame's command buffer has drawn (or is large) */
static void park_check_targets(void)
{
	uint32_t targets[2] = { pass.color, pass.depth };
	int index;

	if (!park.active)
		return;
	for (index = 0; index < 2; index++)
	{
		if (targets[index] && (handles[targets[index]].commands_written == park.generation ||
			!texture_small(targets[index])))
		{
			park_end();
			return;
		}
	}
	for (index = 0; index < 2; index++)
	{
		BOOL keep = index == 0 ? pass.color_load == MTLLoadActionLoad :
			pass.depth_load == MTLLoadActionLoad || pass.stencil_load == MTLLoadActionLoad;

		if (targets[index] && handles[targets[index]].commands_read == park.generation &&
			!park_rename(targets[index], keep))
		{
			park_end();
			return;
		}
	}
}

/* a blit that reads one texture and writes another (0: none): hoisted
when it can be, the screen's pass parked for it */
static void park_blit(uint32_t read, uint32_t write)
{
	if (park.active)
	{
		if ((read && handles[read].commands_written == park.generation) ||
			(write && (handles[write].commands_written == park.generation ||
			handles[write].commands_read == park.generation)))
		{
			park_end();
		}
		return;
	}
	if (park.enabled && encoder && pass.width * pass.height >= PARK_LARGE_PIXELS &&
		(!read || handles[read].commands_written != commands_generation) &&
		(!write || (handles[write].commands_written != commands_generation &&
		handles[write].commands_read != commands_generation)))
	{
		park_begin();
	}
}

/* after a blit: what it read and wrote, in this command buffer */
static void blit_mark(uint32_t read, uint32_t write)
{
	if (read && read < HANDLE_COUNT)
		handles[read].commands_read = commands_generation;
	if (write && write < HANDLE_COUNT)
		handles[write].commands_written = commands_generation;
}

static BOOL encoder_begin(void)
{
	MTLRenderPassDescriptor *descriptor;
	id<MTLTexture> color, depth;

	if (encoder)
		return YES;
	/* (the screen's targets, parked by a blit: its pass goes on, unless a
	clear waits in the load actions) */
	if (park.active && pass.color == park.main_pass.color && pass.depth == park.main_pass.depth)
	{
		if (pass.color_load == MTLLoadActionLoad && pass.depth_load == MTLLoadActionLoad &&
			pass.stencil_load == MTLLoadActionLoad)
		{
			park_resume();
			return encoder != nil;
		}
		park_end();
	}
	park_check_targets();
	color = texture_object(pass.color);
	depth = texture_object(pass.depth);
	if (!color && !depth)
		return NO;
	frame_begin();
	descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.width = (uint32_t)(color ? color.width : depth.width);
	pass.height = (uint32_t)(color ? color.height : depth.height);
	if (color && depth)
	{
		if (depth.width < pass.width)
			pass.width = (uint32_t)depth.width;
		if (depth.height < pass.height)
			pass.height = (uint32_t)depth.height;
	}
	/* a target's first pass clears it (a new texture's pixels are
	whatever was in its memory) */
	if (color && handles[pass.color].fresh)
	{
		handles[pass.color].fresh = 0;
		if (pass.color_load == MTLLoadActionLoad)
		{
			pass.color_load = MTLLoadActionClear;
			pass.clear_color = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
		}
	}
	if (depth && handles[pass.depth].fresh)
	{
		handles[pass.depth].fresh = 0;
		if (pass.depth_load == MTLLoadActionLoad)
		{
			pass.depth_load = MTLLoadActionClear;
			pass.clear_depth = 1.0;
		}
		if (pass.stencil_load == MTLLoadActionLoad)
		{
			pass.stencil_load = MTLLoadActionClear;
			pass.clear_stencil = 0;
		}
	}
	descriptor.colorAttachments[0].texture = color ? color : dummy_color(pass.width, pass.height);
	descriptor.colorAttachments[0].loadAction = color ? pass.color_load : MTLLoadActionDontCare;
	descriptor.colorAttachments[0].storeAction = color ? MTLStoreActionStore : MTLStoreActionDontCare;
	descriptor.colorAttachments[0].clearColor = pass.clear_color;
	/* (a half-resolution run's: its multiply and depths beside it) */
	if (halfres.active && pass.color == halfres.accumulate)
	{
		/* (the run's first pass: the reduction writes every pixel) */
		MTLLoadAction load = halfres.reduced ? MTLLoadActionLoad : MTLLoadActionDontCare;

		descriptor.colorAttachments[1].texture = halfres.multiply;
		descriptor.colorAttachments[1].loadAction = load;
		descriptor.colorAttachments[1].storeAction = MTLStoreActionStore;
		descriptor.colorAttachments[2].texture = halfres.depths;
		descriptor.colorAttachments[2].loadAction = load;
		descriptor.colorAttachments[2].storeAction = MTLStoreActionStore;
		halfres.reduced = YES;
	}
	if (depth)
	{
		/* (HALO_METAL_DEPTH_DISCARD: a load of what was not stored) */
		if (handles[pass.depth].discarded)
		{
			handles[pass.depth].discarded = 0;
			if (pass.depth_load == MTLLoadActionLoad || pass.stencil_load == MTLLoadActionLoad)
			{
				if (depth_discard.misses++ < 10)
					host_log("Metal: HALO_METAL_DEPTH_DISCARD: depth target %u loaded after a discard (frame %llu); "
						"turned off", pass.depth, (unsigned long long)frame_serial);
				depth_discard.enabled = 0;
				depth_discard.broken = 1;
			}
		}
		descriptor.depthAttachment.texture = depth;
		descriptor.depthAttachment.loadAction = pass.depth_load;
		descriptor.depthAttachment.storeAction = depth_discard.enabled ? MTLStoreActionUnknown : MTLStoreActionStore;
		descriptor.depthAttachment.clearDepth = pass.clear_depth;
		/* (a depth target without stencil: the half-resolution runs') */
		descriptor.stencilAttachment.texture = depth.pixelFormat == MTLPixelFormatDepth32Float ? nil : depth;
		descriptor.stencilAttachment.loadAction = pass.stencil_load;
		descriptor.stencilAttachment.storeAction = depth_discard.enabled ? MTLStoreActionUnknown : MTLStoreActionStore;
		descriptor.stencilAttachment.clearStencil = pass.clear_stencil;
	}
	depth_discard.depth = depth && depth_discard.enabled ? pass.depth : 0;
	descriptor.renderTargetWidth = pass.width;
	descriptor.renderTargetHeight = pass.height;
	descriptor.visibilityResultBuffer = frame_current()->visibility;
	pass.color_format = color ? color.pixelFormat : MTLPixelFormatBGRA8Unorm;
	pass.depth_format = depth ? depth.pixelFormat : MTLPixelFormatInvalid;
	if (host_pass_log_enabled)
		pass_log_begin(descriptor);
	encoder = [commands renderCommandEncoderWithDescriptor:descriptor];
	encoder_serial = ++encoder_serials;
	host_frame_log_count_pass();
	if (pass.color)
		handles[pass.color].commands_written = commands_generation;
	if (pass.depth)
		handles[pass.depth].commands_written = commands_generation;
	/* (the next pass of these targets keeps what this one drew) */
	pass.color_load = pass.depth_load = pass.stencil_load = MTLLoadActionLoad;
	bound_forget();
	return encoder != nil;
}

/* a clear still waiting in the load actions, done (by an empty pass) */
static void pass_flush(void)
{
	if (encoder)
		return;
	if (pass.color_load == MTLLoadActionClear || pass.depth_load == MTLLoadActionClear ||
		pass.stencil_load == MTLLoadActionClear)
	{
		if (encoder_begin())
		{
			pass_log.end_reason = _pass_end_flush;
			encoder_end();
		}
	}
}

void host_metal_set_targets(unsigned int color, unsigned int depth)
{
	HOST_FRAME_LOG_SCOPE;
	if (color == pass.color && depth == pass.depth)
		return;
	@autoreleasepool
	{
		pass_flush();
		/* (the screen's targets again: its parked pass goes on) */
		if (park.active && color == park.main_pass.color && depth == park.main_pass.depth)
		{
			park_resume();
			return;
		}
		if (park.enabled && !park.active && encoder && pass.width * pass.height >= PARK_LARGE_PIXELS &&
			texture_small(color) && texture_small(depth))
		{
			park_begin();
		}
		else
		{
			pass_log.end_reason = _pass_end_targets;
			encoder_end();
		}
	}
	pass.color = color;
	pass.depth = depth;
	pass.color_load = pass.depth_load = pass.stencil_load = MTLLoadActionLoad;
}

/* a blit encoder in the frame, after its draws: the render pass ended first
(it is begun again, its targets' pixels loaded, by the next draw), or, the
screen's pass parked, hoisted (park_blit first) */
static id<MTLBlitCommandEncoder> blit_begin(void)
{
	pass_flush();
	pass_log.end_reason = _pass_end_blit;
	encoder_end();
	frame_begin();
	return [commands blitCommandEncoder];
}

/* ---------- the window */

void host_metal_window_created(SDL_Window *window)
{
	(void)window;
}

void host_metal_window_destroyed(SDL_Window *window)
{
	if (window != metal.window)
		return;
	if (metal.view)
		SDL_Metal_DestroyView(metal.view);
	metal.view = NULL;
	metal.layer = nil;
	metal.window = NULL;
}

static NSString *const renderer_shaders =
	@"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"struct clear_values { float4 color; float depth; };\n"
	"struct plain_out { float4 position [[position]]; float2 uv; };\n"
	/* a triangle that covers the viewport */
	"vertex plain_out clear_vertex(uint id [[vertex_id]], constant clear_values &v [[buffer(0)]])\n"
	"{\n"
	"\tfloat2 p = float2((id << 1) & 2, id & 2);\n"
	"\tplain_out o;\n"
	"\to.position = float4(p * 2.0 - 1.0, v.depth, 1.0);\n"
	"\to.uv = p;\n"
	"\treturn o;\n"
	"}\n"
	"fragment float4 clear_fragment(constant clear_values &v [[buffer(0)]]) { return v.color; }\n"
	"fragment float4 white_fragment() { return float4(1.0); }\n"
	/* the window's picture: row 0 of the target is the top of it */
	"vertex plain_out present_vertex(uint id [[vertex_id]])\n"
	"{\n"
	"\tfloat2 p = float2((id << 1) & 2, id & 2);\n"
	"\tplain_out o;\n"
	"\to.position = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);\n"
	"\to.uv = p;\n"
	"\treturn o;\n"
	"}\n"
	"fragment float4 present_fragment(plain_out in [[stage_in]], texture2d<float> picture [[texture(0)]],\n"
	"\tsampler s [[sampler(0)]])\n"
	"{\n"
	"\treturn float4(picture.sample(s, in.uv).rgb, 1.0);\n"
	"}\n"
	/* dynamic resolution (host_metal_scene_upscale): a view's scene, the
	source rectangle of the scene target, scaled up over the destination
	rectangle (the viewport). The coordinates are clamped to the source's
	pixels' centres, so that no filter reads past its edge into what the
	scene target holds outside it (another view's, or an older frame's) */
	"struct upscale_values { float4 source; float2 size; float sharpness; float padding; };\n"
	"vertex plain_out upscale_vertex(uint id [[vertex_id]], constant upscale_values &v [[buffer(0)]])\n"
	"{\n"
	"\tfloat2 p = float2((id << 1) & 2, id & 2);\n"
	"\tplain_out o;\n"
	"\to.position = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);\n"
	"\to.uv = v.source.xy + p * v.source.zw;\n"
	"\treturn o;\n"
	"}\n"
	"static float2 upscale_clamp(float2 at, constant upscale_values &v)\n"
	"{\n"
	"\treturn clamp(at, v.source.xy + 0.5, v.source.xy + v.source.zw - 0.5);\n"
	"}\n"
	/* bilinear: one sample */
	"fragment float4 upscale_bilinear(plain_out in [[stage_in]], texture2d<float> scene [[texture(0)]],\n"
	"\tsampler s [[sampler(0)]], constant upscale_values &v [[buffer(0)]])\n"
	"{\n"
	"\treturn scene.sample(s, upscale_clamp(in.uv, v) / v.size);\n"
	"}\n"
	/* (alpha too: the game keeps scratch in the destination's alpha, which
	what is drawn after the scene may blend with) */
	/* sharp: the bilinear sample, then a contrast-adaptive sharpening of it
	from its four neighbours a source pixel away (AMD's CAS, simplified:
	the weight shrinks where the neighbourhood already has contrast, so
	edges do not ring) */
	"fragment float4 upscale_sharp(plain_out in [[stage_in]], texture2d<float> scene [[texture(0)]],\n"
	"\tsampler s [[sampler(0)]], constant upscale_values &v [[buffer(0)]])\n"
	"{\n"
	"\tfloat2 at = upscale_clamp(in.uv, v);\n"
	"\tfloat4 center = scene.sample(s, at / v.size);\n"
	"\tfloat3 c = center.rgb;\n"
	"\tfloat3 n = scene.sample(s, upscale_clamp(at - float2(0.0, 1.0), v) / v.size).rgb;\n"
	"\tfloat3 w = scene.sample(s, upscale_clamp(at - float2(1.0, 0.0), v) / v.size).rgb;\n"
	"\tfloat3 e = scene.sample(s, upscale_clamp(at + float2(1.0, 0.0), v) / v.size).rgb;\n"
	"\tfloat3 so = scene.sample(s, upscale_clamp(at + float2(0.0, 1.0), v) / v.size).rgb;\n"
	"\tfloat3 lo = min(c, min(min(n, w), min(e, so)));\n"
	"\tfloat3 hi = max(c, max(max(n, w), max(e, so)));\n"
	"\tfloat3 amount = sqrt(saturate(min(lo, 1.0 - hi) / max(hi, 1.0 / 256.0)));\n"
	"\tfloat3 weight = -amount * mix(0.125, 0.2, v.sharpness);\n"
	"\tfloat3 sharp = (c + (n + w + e + so) * weight) / (1.0 + 4.0 * weight);\n"
	"\treturn float4(saturate(sharp), center.a);\n"
	"}\n";

static id<MTLTexture> dummy_make(MTLTextureType type)
{
	MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
	uint32_t black[6] = { 0 };
	id<MTLTexture> texture;
	int face;

	descriptor.textureType = type;
	descriptor.pixelFormat = MTLPixelFormatBGRA8Unorm;
	descriptor.width = descriptor.height = descriptor.depth = 1;
	descriptor.storageMode = MTLStorageModeShared;
	descriptor.usage = MTLTextureUsageShaderRead;
	texture = [metal.device newTextureWithDescriptor:descriptor];
	for (face = 0; face < (type == MTLTextureTypeCube ? 6 : 1); face++)
	{
		[texture replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:(NSUInteger)face
			withBytes:black bytesPerRow:4 bytesPerImage:4];
	}
	return texture;
}

/* the options every shader is compiled with. The language version is
MSL 2.2, not the SDK's default (the newest, 4.x): the shaders need nothing
past 2.1 (invariant positions), and most of a compile is the newest
metal_stdlib's headers, not the shader's code. Measured on an M4, the
translated pixel shaders of a10 and b30 took 8.8 ms each at 2.2 and 47 ms at
the default, and rendered bit-identically at both
(port/tvos/PERFORMANCE.md, "Shader compile stalls").
HALO_METAL_LANGUAGE_VERSION ("2.4", "3.0", ..., or "default") compiles
them as another version, for comparing */
static MTLCompileOptions *compile_options(void)
{
	static MTLCompileOptions *options;

	if (!options)
	{
		const char *version = getenv("HALO_METAL_LANGUAGE_VERSION");
		unsigned int major = 2, minor = 2;
		MTLLanguageVersion sdk_default;

		options = [MTLCompileOptions new];
		sdk_default = options.languageVersion;
		/* (every pass that draws a surface again lands on the same pixels:
		GLSL's invariant gl_Position) */
		options.preserveInvariance = YES;
		if (version && *version && strcmp(version, "default") && sscanf(version, "%u.%u", &major, &minor) != 2)
		{
			host_log("Metal: HALO_METAL_LANGUAGE_VERSION %s is not a version (major.minor): using 2.2", version);
			major = 2;
			minor = 2;
		}
		if (!version || strcmp(version, "default"))
			options.languageVersion = (MTLLanguageVersion)((major << 16) + minor);
		host_log("Metal: shaders compiled as MSL %u.%u (the SDK's default %u.%u)",
			(unsigned int)(options.languageVersion >> 16), (unsigned int)(options.languageVersion & 0xffff),
			(unsigned int)(sdk_default >> 16), (unsigned int)(sdk_default & 0xffff));
	}
	return options;
}

/* (the shader cache: below) */
static void shader_cache_start(void);
void host_metal_map(const char *scenario);

static BOOL renderer_initialize(void)
{
	NSError *error = nil;
	id<MTLLibrary> library = [metal.device newLibraryWithSource:renderer_shaders options:compile_options()
		error:&error];
	MTLRenderPipelineDescriptor *descriptor;
	MTLSamplerDescriptor *sampler;
	int index;

	if (!library)
	{
		host_log("Metal: the renderer's shaders do not build: %s", error.localizedDescription.UTF8String);
		return NO;
	}
	metal.clear_vertex = [library newFunctionWithName:@"clear_vertex"];
	metal.clear_fragment = [library newFunctionWithName:@"clear_fragment"];
	metal.white_fragment = [library newFunctionWithName:@"white_fragment"];
	metal.upscale_vertex = [library newFunctionWithName:@"upscale_vertex"];
	metal.upscale_fragments[METAL_UPSCALE_BILINEAR] = [library newFunctionWithName:@"upscale_bilinear"];
	metal.upscale_fragments[METAL_UPSCALE_SHARP] = [library newFunctionWithName:@"upscale_sharp"];
	metal.debug = getenv("HALO_METAL_DEBUG") ? atoi(getenv("HALO_METAL_DEBUG")) : 0;
	descriptor = [MTLRenderPipelineDescriptor new];
	descriptor.vertexFunction = [library newFunctionWithName:@"present_vertex"];
	descriptor.fragmentFunction = [library newFunctionWithName:@"present_fragment"];
	descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	metal.present_pipeline = [metal.device newRenderPipelineStateWithDescriptor:descriptor error:&error];
	if (!metal.present_pipeline)
	{
		host_log("Metal: no presentation pipeline: %s", error.localizedDescription.UTF8String);
		return NO;
	}
	sampler = [MTLSamplerDescriptor new];
	sampler.minFilter = sampler.magFilter = MTLSamplerMinMagFilterLinear;
	metal.present_sampler = [metal.device newSamplerStateWithDescriptor:sampler];
	metal.dummy_2d = dummy_make(MTLTextureType2D);
	metal.dummy_cube = dummy_make(MTLTextureTypeCube);
	metal.dummy_3d = dummy_make(MTLTextureType3D);
	/* (the pixel shaders' sources, for display.half_res_transparents' variants) */
	halfres_sources = [NSMutableDictionary dictionary];
	frames_free = dispatch_semaphore_create(FRAMES_IN_FLIGHT);
	for (index = 0; index < METAL_VISIBILITY_SLOTS; index++)
		atomic_store(&index_known[index], 0);
	return YES;
}

int host_metal_attach(unsigned int window)
{
	@autoreleasepool
	{
		SDL_Window *object = host_sdl_window(window);

		if (!object)
			return 0;
		if (!metal.device)
		{
			profile_enabled = getenv("HALO_METAL_PROFILE") && atoi(getenv("HALO_METAL_PROFILE")) != 0;
			host_frame_log_start();
			metal.device = MTLCreateSystemDefaultDevice();
			if (!metal.device)
			{
				SDL_SetError("no Metal device");
				return 0;
			}
			metal.queue = [metal.device newCommandQueue];
			host_log("Metal: %s", metal.device.name.UTF8String);
			if (!renderer_initialize())
			{
				metal.device = nil;
				SDL_SetError("the Metal renderer cannot start");
				return 0;
			}
			shader_cache_start();
			pass_log_initialize();
			park_initialize();
			host_lifecycle_start();
		}
		metal.view = SDL_Metal_CreateView(object);
		if (!metal.view)
			return 0;
		metal.layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(metal.view);
		metal.layer.device = metal.device;
		metal.layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
		metal.layer.framebufferOnly = YES;
		metal.layer.maximumDrawableCount = 3;
		/* (the display's refresh interval: the dynamic resolution's budget) */
		metal.slot_ns = (uint64_t)(1e9 / host_telemetry_platform_refresh_hz());
		metal.thermal = (int)[NSProcessInfo processInfo].thermalState;
#ifndef HALO_TVOS
		metal.layer.displaySyncEnabled = metal.vsync ? YES : NO;
#endif
		metal.window = object;
		return 1;
	}
}

void host_metal_set_vsync(int interval)
{
	metal.vsync = interval != 0;
#ifndef HALO_TVOS
	if (metal.layer)
		metal.layer.displaySyncEnabled = metal.vsync ? YES : NO;
#endif
}

void host_metal_profile_phase(int profile, int begin)
{
	if (!profile_enabled && !host_frame_log_level && !pass_log.skip_phases)
		return;
	if (begin)
		profile_phase = profile >= 0 && profile < 29 ? profile : -1;
	else if (profile_phase == profile)
		profile_phase = -1;
}

void host_metal_screen_effect(int type, int passes, float radius, int video)
{
	if (!profile_enabled)
		return;
	profile_effect_calls++;
	if (type != 0 && profile_last_convolution_frame != frame_serial)
	{
		profile_convolution_frames++;
		profile_last_convolution_frame = frame_serial;
	}
	profile_effect_frame = frame_serial;
	profile_effect_type = type;
	profile_effect_passes = passes;
	profile_effect_radius = radius;
	profile_effect_video = video;
}

/* ---------- textures */

/* (HALO_METAL_NO_BC=1: as a GPU without BC formats, the Apple TV's, would:
the conversions on a Mac, to compare with its own BC) */
uint64_t host_metal_allocated_bytes(void)
{
	return metal.device ? (uint64_t)metal.device.currentAllocatedSize : 0;
}

int host_metal_supports_bc(void)
{
	static int no_bc = -1;

	if (no_bc < 0)
		no_bc = getenv("HALO_METAL_NO_BC") && atoi(getenv("HALO_METAL_NO_BC")) != 0;
	return metal.device && metal.device.supportsBCTextureCompression && !no_bc ? 1 : 0;
}

/* ---------- textures: their memory, and DXT on a GPU without BC formats */

/* each sampled texture's bytes (allocatedSize), and whether it was of DXT
blocks converted here; the sums, and their most */
static uint32_t texture_bytes[HANDLE_COUNT];
static uint8_t texture_converted[HANDLE_COUNT];
static uint64_t textures_total, textures_converted_total, textures_most;
static uint32_t textures_count;
/* the targets' (colour and depth, mip composites) the same way */
static uint32_t target_bytes[HANDLE_COUNT];
static uint64_t targets_total;
static uint32_t targets_count;
/* the conversions: how many, their blocks' bytes and the time */
static uint64_t conversions, conversion_source_bytes, conversion_ns;
/* a texture defined next is the GPU's alone (host_metal_texture_bc) */
static int define_private;

static void textures_report(void);
static void target_account(uint32_t handle, id<MTLTexture> texture);

/* a texture's bytes: what Metal allocated, or (where it says 0, as the
simulator does) its levels' texels in its format */
static uint32_t texture_size(id<MTLTexture> texture)
{
	uint64_t bytes = texture.allocatedSize;
	NSUInteger level;

	if (bytes)
		return (uint32_t)bytes;
	for (level = 0; level < texture.mipmapLevelCount; level++)
	{
		uint64_t w = texture.width >> level ? texture.width >> level : 1;
		uint64_t h = texture.height >> level ? texture.height >> level : 1;
		uint64_t d = texture.depth >> level ? texture.depth >> level : 1;

		switch ((unsigned int)texture.pixelFormat)
		{
		case METAL_FORMAT_BC1: bytes += ((w + 3) / 4) * ((h + 3) / 4) * 8 * d; break;
		case METAL_FORMAT_BC2:
		case METAL_FORMAT_BC3:
		case METAL_FORMAT_ASTC_4X4: bytes += ((w + 3) / 4) * ((h + 3) / 4) * 16 * d; break;
		default: bytes += w * h * d * 4; break;
		}
	}
	return (uint32_t)(bytes * texture.arrayLength * (texture.textureType == MTLTextureTypeCube ? 6 : 1));
}

static void texture_account(uint32_t handle, id<MTLTexture> texture, int converted)
{
	static int reported;

	/* (the totals in the log at exit, with the frame log: HALO_FRAME_LOG) */
	if (!reported && host_frame_log_level)
	{
		reported = 1;
		atexit(textures_report);
	}
	textures_total -= texture_bytes[handle];
	textures_count -= texture_bytes[handle] != 0;
	if (texture_converted[handle])
		textures_converted_total -= texture_bytes[handle];
	texture_bytes[handle] = texture ? texture_size(texture) : 0;
	textures_count += texture_bytes[handle] != 0;
	texture_converted[handle] = (uint8_t)(texture && converted);
	textures_total += texture_bytes[handle];
	if (texture_converted[handle])
		textures_converted_total += texture_bytes[handle];
	if (textures_total > textures_most)
		textures_most = textures_total;
	if (host_frame_log_level && texture && converted && conversions % 256 == 255)
		textures_report();
}

long long host_metal_sampled_bytes(void)
{
	return (long long)textures_total;
}

/* a target's bytes (nil: none any more) */
static void target_account(uint32_t handle, id<MTLTexture> texture)
{
	targets_total -= target_bytes[handle];
	targets_count -= target_bytes[handle] != 0;
	target_bytes[handle] = texture ? texture_size(texture) : 0;
	targets_total += target_bytes[handle];
	targets_count += target_bytes[handle] != 0;
}

/* what owns Metal's allocations (host_telemetry.h), for finding what grows
over a long session: the sampled textures, the targets, the frames' rings
and their overflow buffers, the renderer's own textures (feedback copy,
dummy colour attachment, the hoisting's pool of renamed targets), and how
many state objects (pipelines and the rest) the caches hold */
void host_metal_memory(struct host_metal_memory *memory)
{
	uint32_t index;

	memset(memory, 0, sizeof(*memory));
	memory->textures_bytes = textures_total;
	memory->textures = textures_count;
	memory->converted_bytes = textures_converted_total;
	memory->targets_bytes = targets_total;
	memory->targets = targets_count;
	for (index = 0; index < FRAMES_IN_FLIGHT; index++)
	{
		if (frames[index].ring)
			memory->buffers_bytes += frames[index].ring.allocatedSize;
		for (id<MTLBuffer> buffer in frames[index].overflow)
			memory->buffers_bytes += buffer.allocatedSize;
	}
	if (metal.feedback)
		memory->own_bytes += metal.feedback.allocatedSize;
	if (metal.dummy_color)
		memory->own_bytes += metal.dummy_color.allocatedSize;
	for (index = 0; index < PARK_POOL; index++)
	{
		if (park.pool[index])
			memory->own_bytes += park.pool[index].allocatedSize;
	}
	memory->state_objects = cache_objects;
}

static void textures_report(void)
{
	host_log("textures: %.1f MiB sampled (%.1f MiB of them DXT converted), at most %.1f MiB; %llu DXT textures "
		"converted (%.1f MiB of blocks) in %.1f ms; the device's allocations %.1f MiB",
		textures_total / 1048576.0, textures_converted_total / 1048576.0, textures_most / 1048576.0,
		(unsigned long long)conversions, conversion_source_bytes / 1048576.0, conversion_ns / 1e6,
		metal.device ? metal.device.currentAllocatedSize / 1048576.0 : 0.0);
}

/* (rows of blocks a conversion splits between threads, at most) */
#define CONVERSION_ROWS 16

void host_metal_texture_define(unsigned int handle, int type, unsigned int format, unsigned int width,
	unsigned int height, unsigned int depth, unsigned int levels, int usage, unsigned int swizzle);

void host_metal_texture_bc(unsigned int handle, int type, int kind, int into, unsigned int width,
	unsigned int height, unsigned int depth, unsigned int levels, unsigned int swizzle, const void *data,
	unsigned int face_bytes)
{
	HOST_FRAME_LOG_SCOPE;
	static int shared = -1;
	uint64_t started = host_frame_log_now();
	unsigned int faces = type == _metal_texture_cube ? 6 : 1, block_bytes = host_textures_bc_block_bytes(kind);
	unsigned int face, level, level_count;
	id<MTLTexture> texture;

	if (!metal.device || !data || !width || !height)
		return;
	if (shared < 0)
	{
		/* (HALO_METAL_TEXTURE_SHARED=1: the converted textures in memory the
		CPU writes, as the guest's are, rather than the GPU's own, which it
		may compress) */
		shared = getenv("HALO_METAL_TEXTURE_SHARED") && atoi(getenv("HALO_METAL_TEXTURE_SHARED")) != 0;
	}
	/* (ASTC for 2D and cube textures, where the GPU has it) */
	if (into == _metal_bc_astc && (type == _metal_texture_3d || ![metal.device supportsFamily:MTLGPUFamilyApple2]))
		into = _metal_bc_exact;
	if (type != _metal_texture_3d)
		depth = 1;
	define_private = !shared;
	host_metal_texture_define(handle, type, into == _metal_bc_astc ? METAL_FORMAT_ASTC_4X4 : METAL_FORMAT_BGRA8,
		width, height, depth, levels, _metal_usage_sampled, swizzle);
	define_private = 0;
	texture = texture_object(handle);
	if (!texture)
		return;
	handles[handle].private_storage = !shared;
	level_count = (unsigned int)texture.mipmapLevelCount;
	@autoreleasepool
	{
		/* each face's and level's converted texels, one after another
		(256-byte aligned) in a buffer the GPU copies from */
		NSUInteger total = 0;
		id<MTLBuffer> staging;
		unsigned char *out;
		id<MTLCommandBuffer> uploads = nil;
		id<MTLBlitCommandEncoder> blit = nil;

		for (face = 0; face < faces; face++)
		{
			for (level = 0; level < level_count; level++)
			{
				unsigned int w = width >> level ? width >> level : 1, h = height >> level ? height >> level : 1;
				unsigned int d = depth >> level ? depth >> level : 1;

				total += (into == _metal_bc_astc ? (NSUInteger)((w + 3) / 4) * ((h + 3) / 4) * 16 :
					(NSUInteger)w * h * 4) * d;
				total = (total + 255) & ~(NSUInteger)255;
			}
		}
		staging = [metal.device newBufferWithLength:total options:MTLResourceStorageModeShared];
		if (!staging)
		{
			host_log("Metal: no memory to convert a %ux%u DXT texture", width, height);
			return;
		}
		out = staging.contents;
		if (!shared)
		{
			uploads = [metal.queue commandBuffer];
			uploads.label = @"texture uploads";
			blit = [uploads blitCommandEncoder];
		}
		total = 0;
		for (face = 0; face < faces; face++)
		{
			const unsigned char *source = (const unsigned char *)data + (NSUInteger)face * face_bytes;

			for (level = 0; level < level_count; level++)
			{
				unsigned int w = width >> level ? width >> level : 1, h = height >> level ? height >> level : 1;
				unsigned int d = depth >> level ? depth >> level : 1;
				unsigned int blocks_x = (w + 3) / 4, blocks_y = (h + 3) / 4;
				unsigned int source_row = blocks_x * block_bytes, source_image = source_row * blocks_y;
				unsigned int row = into == _metal_bc_astc ? blocks_x * 16 : w * 4;
				unsigned int image = into == _metal_bc_astc ? row * blocks_y : row * h;
				unsigned char *level_out = out + total;
				/* (large levels split by rows of blocks between threads) */
				size_t chunks = d == 1 && blocks_x * blocks_y >= 4096 ? (blocks_y + CONVERSION_ROWS - 1) / CONVERSION_ROWS : 1;

				dispatch_apply(chunks, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t chunk)
				{
					unsigned int first = chunks > 1 ? (unsigned int)chunk * CONVERSION_ROWS : 0;
					unsigned int rows = chunks > 1 ? (first + CONVERSION_ROWS > blocks_y ? blocks_y - first : CONVERSION_ROWS) :
						blocks_y;

					if (into == _metal_bc_astc)
					{
						host_textures_bc_to_astc(kind, source + (NSUInteger)first * source_row, source_row, source_image,
							blocks_x, rows, d, level_out + (NSUInteger)first * row, NULL);
					}
					else
					{
						unsigned int texel_rows = chunks > 1 ? (first * 4 + rows * 4 > h ? h - first * 4 : rows * 4) : h;

						host_textures_bc_to_bgra(kind, source + (NSUInteger)first * source_row, source_row, source_image,
							w, texel_rows, d, (uint32_t *)(level_out + (NSUInteger)first * 4 * row));
					}
				});
				if (blit)
				{
					[blit copyFromBuffer:staging sourceOffset:total sourceBytesPerRow:row
						sourceBytesPerImage:type == _metal_texture_3d ? image : 0 sourceSize:MTLSizeMake(w, h, d)
						toTexture:texture destinationSlice:face destinationLevel:level destinationOrigin:MTLOriginMake(0, 0, 0)];
				}
				else
				{
					/* (written now, as host_metal_texture_upload writes: the
					texture is new, no frame reads it) */
					[texture replaceRegion:MTLRegionMake3D(0, 0, 0, w, h, d) mipmapLevel:level slice:face
						withBytes:level_out bytesPerRow:row bytesPerImage:type == _metal_texture_3d ? image : 0];
				}
				source += (NSUInteger)source_image * d;
				total = (total + (NSUInteger)image * d + 255) & ~(NSUInteger)255;
			}
		}
		if (blit)
		{
			/* (a command buffer of its own, committed now: before the frame's,
			which draws with the texture, is) */
			[blit endEncoding];
			host_lifecycle_gate();
			[uploads commit];
			uploads_last = uploads;
		}
	}
	texture_account(handle, texture, 1);
	conversions++;
	conversion_source_bytes += (uint64_t)face_bytes * faces;
	conversion_ns += host_frame_log_now() - started;
	if (host_frame_log_detail())
	{
		host_frame_acc.texture_conversions++;
		host_frame_acc.texture_conversion_bytes += (uint64_t)face_bytes * faces;
		host_frame_acc.texture_conversion_ns += host_frame_log_now() - started;
	}
}

unsigned int host_metal_texture_new(void)
{
	return handle_new(_handle_texture);
}

/* (a new texture each time: a frame on the GPU that drew with the old one
keeps it until it is done, and the new one can be written at once) */
void host_metal_texture_define(unsigned int handle, int type, unsigned int format, unsigned int width,
	unsigned int height, unsigned int depth, unsigned int levels, int usage, unsigned int swizzle)
{
	HOST_FRAME_LOG_SCOPE;
	if (!metal.device || !handle || handle >= HANDLE_COUNT || handles[handle].type != _handle_texture || !width ||
		!height)
	{
		return;
	}
	@autoreleasepool
	{
		MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
		unsigned int largest, most = 1;
		id<MTLTexture> texture;

		if (!depth || type != _metal_texture_3d)
			depth = 1;
		if (type == _metal_texture_cube)
			height = width;
		largest = width > height ? width : height;
		if (depth > largest)
			largest = depth;
		while (largest > 1)
		{
			largest >>= 1;
			most++;
		}
		descriptor.textureType = type == _metal_texture_cube ? MTLTextureTypeCube :
			type == _metal_texture_3d ? MTLTextureType3D : MTLTextureType2D;
		descriptor.pixelFormat = (MTLPixelFormat)format;
		descriptor.width = width;
		descriptor.height = height;
		descriptor.depth = depth;
		descriptor.mipmapLevelCount = levels < 1 ? 1 : levels > most ? most : levels;
		if (usage == _metal_usage_sampled)
		{
			descriptor.usage = MTLTextureUsageShaderRead;
			descriptor.storageMode = define_private ? MTLStorageModePrivate : MTLStorageModeShared;
			descriptor.swizzle = MTLTextureSwizzleChannelsMake((MTLTextureSwizzle)(swizzle & 0xff),
				(MTLTextureSwizzle)((swizzle >> 8) & 0xff), (MTLTextureSwizzle)((swizzle >> 16) & 0xff),
				(MTLTextureSwizzle)((swizzle >> 24) & 0xff));
		}
		else
		{
			descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
			descriptor.storageMode = MTLStorageModePrivate;
		}
		texture = [metal.device newTextureWithDescriptor:descriptor];
		if (!texture)
			host_log("Metal: cannot make a %ux%ux%u texture (format %u)", width, height, depth, format);
		if (park.active && (park.main_pass.color == handle || park.main_pass.depth == handle))
			park_end();
		if (pass.color == handle || pass.depth == handle)
		{
			/* (a target redefined: its pass is of the old texture) */
			pass_flush();
			encoder_end();
		}
		handle_set(handle, texture);
		handles[handle].commands_read = handles[handle].commands_written = 0;
		handles[handle].private_storage = usage != _metal_usage_sampled;
		handles[handle].fresh = usage != _metal_usage_sampled;
		handles[handle].used_serial = 0;
		texture_account(handle, usage == _metal_usage_sampled ? texture : nil, 0);
		target_account(handle, usage != _metal_usage_sampled ? texture : nil);
	}
}

void host_metal_texture_upload(unsigned int handle, unsigned int face, unsigned int level, unsigned int x,
	unsigned int y, unsigned int z, unsigned int width, unsigned int height, unsigned int depth, const void *data,
	unsigned int bytes_per_row, unsigned int bytes_per_image)
{
	HOST_FRAME_LOG_SCOPE;
	id<MTLTexture> texture = texture_object(handle);
	BOOL volume;

	if (!texture || !data || level >= texture.mipmapLevelCount || handles[handle].private_storage)
		return;
	if (!depth)
		depth = 1;
	volume = texture.textureType == MTLTextureType3D;
	host_frame_acc.texture_uploads++;
	host_frame_acc.texture_upload_bytes += (uint64_t)bytes_per_image * depth;
	@autoreleasepool
	{
		/* a texture no queued frame reads: written now */
		if (handles[handle].used_serial <= atomic_load(&completed_serial))
		{
			[texture replaceRegion:MTLRegionMake3D(x, y, z, width, height, depth) mipmapLevel:level slice:face
				withBytes:data bytesPerRow:bytes_per_row bytesPerImage:volume ? bytes_per_image : 0];
			return;
		}
		/* else a blit, in order with the draws, from the pixels staged in
		the frame's ring */
		{
			uint32_t bytes = bytes_per_image * depth, offset;
			id<MTLBuffer> buffer;
			void *staged = ring_allocate(bytes, 256, &buffer, &offset);
			id<MTLBlitCommandEncoder> blit;

			memcpy(staged, data, bytes);
			park_blit(0, handle);
			blit = blit_begin();
			blit_mark(0, handle);
			[blit copyFromBuffer:buffer sourceOffset:offset sourceBytesPerRow:bytes_per_row
				sourceBytesPerImage:volume ? bytes_per_image : 0 sourceSize:MTLSizeMake(width, height, depth)
				toTexture:texture destinationSlice:face destinationLevel:level destinationOrigin:MTLOriginMake(x, y, z)];
			[blit endEncoding];
		}
	}
}

void host_metal_texture_delete(unsigned int handle)
{
	if (!handle || handle >= HANDLE_COUNT || handles[handle].type != _handle_texture)
		return;
	@autoreleasepool
	{
		if (metal.present_source == handle)
			metal.present_source = 0;
		if (park.active && (park.main_pass.color == handle || park.main_pass.depth == handle))
			park_end();
		if (pass.color == handle || pass.depth == handle)
		{
			encoder_end();
			if (pass.color == handle)
				pass.color = 0;
			if (pass.depth == handle)
				pass.depth = 0;
			pass.color_load = pass.depth_load = pass.stencil_load = MTLLoadActionLoad;
		}
		texture_account(handle, nil, 0);
		target_account(handle, nil);
		handle_set(handle, nil);
		handles[handle].type = _handle_free;
		host_frame_acc.texture_deletes++;
	}
}

/* level 0 of source (width by height of it) into a level of destination */
void host_metal_texture_copy_level(unsigned int source, unsigned int destination, unsigned int level,
	unsigned int width, unsigned int height)
{
	id<MTLTexture> from = texture_object(source), to = texture_object(destination);

	if (!from || !to || level >= to.mipmapLevelCount || from.pixelFormat != to.pixelFormat)
		return;
	@autoreleasepool
	{
		id<MTLBlitCommandEncoder> blit;
		NSUInteger level_width = to.width >> level ? to.width >> level : 1;
		NSUInteger level_height = to.height >> level ? to.height >> level : 1;

		if (width > from.width)
			width = (uint32_t)from.width;
		if (height > from.height)
			height = (uint32_t)from.height;
		if (width > level_width)
			width = (uint32_t)level_width;
		if (height > level_height)
			height = (uint32_t)level_height;
		park_blit(source, destination);
		blit = blit_begin();
		blit_mark(source, destination);
		[blit copyFromTexture:from sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
			sourceSize:MTLSizeMake(width, height, 1) toTexture:to destinationSlice:0 destinationLevel:level
			destinationOrigin:MTLOriginMake(0, 0, 0)];
		[blit endEncoding];
	}
}

/* the levels past first made from it */
void host_metal_texture_generate_mipmaps(unsigned int handle, unsigned int first)
{
	id<MTLTexture> texture = texture_object(handle);

	if (!texture || first + 1 >= texture.mipmapLevelCount)
		return;
	@autoreleasepool
	{
		id<MTLBlitCommandEncoder> blit;

		park_blit(handle, handle);
		blit = blit_begin();
		blit_mark(handle, handle);
		if (first)
		{
			id<MTLTexture> view = [texture newTextureViewWithPixelFormat:texture.pixelFormat
				textureType:texture.textureType levels:NSMakeRange(first, texture.mipmapLevelCount - first)
				slices:NSMakeRange(0, texture.textureType == MTLTextureTypeCube ? 6 : 1)];

			[blit generateMipmapsForTexture:view];
		}
		else
		{
			[blit generateMipmapsForTexture:texture];
		}
		[blit endEncoding];
	}
}

/* ---------- shaders and vertex layouts */

/* the diagnostics that change a shader's text, read once, on the main
thread (shader_text also runs on the shader cache's) */
static struct
{
	BOOL read;
	/* HALO_METAL_SHADER_SALT, HALO_METAL_PROBE, HALO_METAL_PROBE_EARLY, or
	NULL */
	const char *salt, *probe, *probe_early;
	/* the names of the shaders' functions: the translator's, or with the
	salt's letters and digits after them */
	NSString *entry_points[2];
} shader_settings;

static void shader_settings_read(void)
{
	NSMutableString *suffix = [NSMutableString string];
	const char *at;

	if (shader_settings.read)
		return;
	shader_settings.read = YES;
	shader_settings.salt = getenv("HALO_METAL_SHADER_SALT") && *getenv("HALO_METAL_SHADER_SALT") ?
		getenv("HALO_METAL_SHADER_SALT") : NULL;
	shader_settings.probe = getenv("HALO_METAL_PROBE");
	shader_settings.probe_early = getenv("HALO_METAL_PROBE_EARLY");
	for (at = shader_settings.salt; at && *at; at++)
	{
		if ((*at >= '0' && *at <= '9') || (*at >= 'a' && *at <= 'z') || (*at >= 'A' && *at <= 'Z'))
			[suffix appendFormat:@"%c", *at];
	}
	if (suffix.length)
		[suffix insertString:@"_salt_" atIndex:0];
	shader_settings.entry_points[0] = [@"vertex_main" stringByAppendingString:suffix];
	shader_settings.entry_points[1] = [@"fragment_main" stringByAppendingString:suffix];
}

static const char *shader_salt(void)
{
	return shader_settings.salt;
}

static NSString *shader_entry_point(int fragment)
{
	return shader_settings.entry_points[fragment ? 1 : 0];
}

/* the shader cache's records (below) */
struct pipeline_key;
static void shader_cache_record_shader(uint32_t handle, const char *source, int fragment);
static void shader_cache_record_layout(uint32_t handle, const struct metal_vertex_layout *layout);
static void shader_cache_record_pipeline(const struct pipeline_key *key);
static void shader_cache_note(uint32_t handle);
/* (the shader cache keeps a record: shader_cache.enabled) */
static BOOL shader_cache_noting;
@class PreparedShader;
static id<MTLFunction> prepared_take(NSString *text, int fragment, PreparedShader **claimed);
static void prepared_give(PreparedShader *claimed, id<MTLFunction> function);

/* the text compiled for a shader the game gave: its source, with the
diagnostics' changes. The same for a draw's compile and a prepared one
(shader_prepare), so that what is prepared is what the draw asks for */
static NSString *shader_text(const char *source, int fragment)
{
	NSString *text = [NSString stringWithUTF8String:source];

	if (!text)
		return nil;
	/* HALO_METAL_PROBE: an expression of the pixel shaders' (a
	register, a combiner input) drawn in place of their result, with
	no alpha test (finding where a color goes wrong) */
	if (fragment && shader_settings.probe)
	{
		NSString *probe = [NSString stringWithFormat:@"\treturn vec4(clamp(vec4(%s).rgb, 0.0, 1.0), 1.0);\n}\n",
			shader_settings.probe];
		NSRange end = [text rangeOfString:@"\treturn clamp(result, 0.0, 1.0)" options:NSBackwardsSearch];

		/* (HALO_METAL_PROBE_EARLY: before the combiners, the texture
		stages' samples as they are) */
		if (shader_settings.probe_early)
		{
			const char *at = shader_settings.probe_early;
			NSRange early = strlen(at) > 1 ? [text rangeOfString:[NSString stringWithUTF8String:at]] :
				[text rangeOfString:@"\tvec4 r0 = vec4(0.0, 0.0, 0.0, t0.a);"];

			if (early.location != NSNotFound)
				text = [[text substringToIndex:early.location] stringByAppendingString:probe];
			end.location = NSNotFound;
		}
		if (end.location != NSNotFound)
		{
			NSRange test = [text rangeOfString:@"\tif (!(floor(clamp(result.a" options:NSBackwardsSearch];

			if (test.location != NSNotFound && test.location < end.location)
				end.location = test.location;
			text = [[text substringToIndex:end.location] stringByAppendingString:probe];
		}
	}
	/* HALO_METAL_SHADER_SALT (diagnostic, for measuring cold compiles):
	a comment of its text at the end of every shader, so that none is
	found in the system's compiled-shader cache of an earlier launch,
	and its letters and digits in the name of the shader's function. A
	comment alone changes only the source: its compiled code (AIR) is
	the same as before, and the pipelines made of it are found in the
	system's cache of them. The code compiled is the same */
	{
		const char *salt = shader_salt();

		if (salt)
		{
			NSRange name = [text rangeOfString:fragment ? @"fragment_main(" : @"vertex_main("];

			if (name.location != NSNotFound)
				text = [text stringByReplacingCharactersInRange:NSMakeRange(name.location, name.length - 1)
					withString:shader_entry_point(fragment)];
			text = [text stringByAppendingFormat:@"\n// salt %s\n", salt];
		}
	}
	return text;
}

unsigned int host_metal_shader(const char *source, int fragment)
{
	HOST_FRAME_LOG_SCOPE;
	if (!metal.device || !source)
		return 0;
	shader_settings_read();
	@autoreleasepool
	{
		NSError *error = nil;
		MTLCompileOptions *options = compile_options();
		id<MTLLibrary> library;
		id<MTLFunction> function;
		PreparedShader *claimed;
		uint32_t handle;

		NSString *text = shader_text(source, fragment);

		if (!text)
			return 0;
		uint64_t compile_started = profile_enabled ? SDL_GetTicksNS() : 0;
		uint64_t frame_log_started = host_frame_log_detail() ? host_frame_log_now() : 0;
		/* (the shader cache's, if a thread compiled this text: its wait for
		one compiling it is counted as compiling) */
		function = prepared_take(text, fragment, &claimed);
		library = function ? nil : [metal.device newLibraryWithSource:text options:options error:&error];
		if (profile_enabled)
			profile_shader_ns += SDL_GetTicksNS() - compile_started;
		if (frame_log_started)
		{
			host_frame_acc.compile_ns += host_frame_log_now() - frame_log_started;
			host_frame_acc.compiles++;
		}
		if (!function && !library)
		{
			host_log("Metal: a %s shader does not build:\n%s\n%s", fragment ? "pixel" : "vertex",
				error.localizedDescription.UTF8String, source);
			return 0;
		}
		if (!function)
		{
			function = [library newFunctionWithName:shader_entry_point(fragment)];
			prepared_give(claimed, function);
		}
		if (!function)
			return 0;
		handle = handle_new(_handle_shader);
		if (handle)
		{
			handle_set(handle, function);
			shader_cache_record_shader(handle, source, fragment);
			/* (its text, for its half-resolution variant: halfres_fragment) */
			if (fragment && halfres_sources)
				halfres_sources[@(handle)] = [NSString stringWithUTF8String:source];
		}
		return handle;
	}
}

static MTLVertexDescriptor *vertex_descriptor_make(const struct metal_vertex_layout *layout)
{
	MTLVertexDescriptor *descriptor = [MTLVertexDescriptor vertexDescriptor];
	uint32_t index;

	for (index = 0; index < METAL_VERTEX_BUFFERS; index++)
	{
		const struct metal_vertex_buffer_layout *buffer = &layout->buffers[index];

		if (!buffer->stride)
			continue;
		/* (the stride before the step function: Metal checks it then) */
		descriptor.layouts[index].stride = buffer->stride;
		if (buffer->constant)
		{
			descriptor.layouts[index].stepFunction = MTLVertexStepFunctionConstant;
			descriptor.layouts[index].stepRate = 0;
		}
	}
	for (index = 0; index < METAL_VERTEX_ATTRIBUTES; index++)
	{
		const struct metal_vertex_attribute *attribute = &layout->attributes[index];

		if (attribute->format == METAL_VERTEX_FORMAT_INVALID)
			continue;
		descriptor.attributes[index].format = (MTLVertexFormat)attribute->format;
		descriptor.attributes[index].bufferIndex = attribute->buffer;
		descriptor.attributes[index].offset = attribute->offset;
	}
	return descriptor;
}

unsigned int host_metal_vertex_layout(const struct metal_vertex_layout *layout)
{
	@autoreleasepool
	{
		uint32_t handle = handle_new(_handle_layout);

		if (handle)
		{
			handle_set(handle, vertex_descriptor_make(layout));
			shader_cache_record_layout(handle, layout);
		}
		return handle;
	}
}

/* ---------- state objects */

struct pipeline_key
{
	uint32_t vertex_shader, fragment_shader, vertex_layout;
	uint32_t color_format, depth_format;
	struct metal_blend blend;
};

/* (a half-resolution run's draw's pipeline: below) */
static id<MTLRenderPipelineState> halfres_pipeline_get(const struct metal_draw *draw);

/* a draw pipeline's descriptor: its shaders, vertex layout, targets' formats
and blend (a pipeline_key's, as pipeline_get makes it) */
static MTLRenderPipelineDescriptor *pipeline_descriptor_make(id<MTLFunction> vertex, id<MTLFunction> fragment,
	MTLVertexDescriptor *layout, uint32_t color_format, uint32_t depth_format, const struct metal_blend *blend)
{
	MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];

	descriptor.vertexFunction = vertex;
	descriptor.fragmentFunction = fragment;
	descriptor.vertexDescriptor = layout;
	descriptor.colorAttachments[0].pixelFormat = (MTLPixelFormat)color_format;
	descriptor.colorAttachments[0].writeMask = blend->write_mask;
	descriptor.colorAttachments[0].blendingEnabled = blend->enabled ? YES : NO;
	descriptor.colorAttachments[0].sourceRGBBlendFactor = (MTLBlendFactor)blend->source;
	descriptor.colorAttachments[0].sourceAlphaBlendFactor = (MTLBlendFactor)blend->source;
	descriptor.colorAttachments[0].destinationRGBBlendFactor = (MTLBlendFactor)blend->destination;
	descriptor.colorAttachments[0].destinationAlphaBlendFactor = (MTLBlendFactor)blend->destination;
	descriptor.colorAttachments[0].rgbBlendOperation = (MTLBlendOperation)blend->operation;
	descriptor.colorAttachments[0].alphaBlendOperation = (MTLBlendOperation)blend->operation;
	descriptor.depthAttachmentPixelFormat = (MTLPixelFormat)depth_format;
	descriptor.stencilAttachmentPixelFormat = (MTLPixelFormat)depth_format;
	return descriptor;
}

static id<MTLRenderPipelineState> pipeline_get(const struct metal_draw *draw)
{
	struct pipeline_key key;
	uint64_t hash;
	BOOL found;
	void *object;
	MTLRenderPipelineDescriptor *descriptor;
	id<MTLRenderPipelineState> state;
	NSError *error = nil;

	/* (a map's first draw of a shader: the shader cache's per-map record) */
	if (shader_cache_noting)
	{
		shader_cache_note(draw->vertex_shader);
		shader_cache_note(draw->fragment_shader);
	}
	if (halfres.active && pass.color == halfres.accumulate)
		return halfres_pipeline_get(draw);
	memset(&key, 0, sizeof(key));
	key.vertex_shader = draw->vertex_shader;
	key.fragment_shader = draw->fragment_shader;
	key.vertex_layout = draw->vertex_layout;
	key.color_format = (uint32_t)pass.color_format;
	key.depth_format = (uint32_t)pass.depth_format;
	key.blend = draw->blend;
	if (!key.blend.enabled)
		key.blend.source = key.blend.destination = key.blend.operation = 0;
	object = cache_find(&pipelines, &key, sizeof(key), &hash, &found);
	if (found)
		return (__bridge id<MTLRenderPipelineState>)object;
	descriptor = pipeline_descriptor_make(handle_object(draw->vertex_shader, _handle_shader),
		metal.debug & 4 ? metal.white_fragment : handle_object(draw->fragment_shader, _handle_shader),
		handle_object(draw->vertex_layout, _handle_layout), key.color_format, key.depth_format, &key.blend);
	uint64_t compile_started = profile_enabled ? SDL_GetTicksNS() : 0;
	uint64_t frame_log_started = host_frame_log_detail() ? host_frame_log_now() : 0;
	state = descriptor.vertexFunction && descriptor.fragmentFunction && descriptor.vertexDescriptor ?
		[metal.device newRenderPipelineStateWithDescriptor:descriptor error:&error] : nil;
	if (profile_enabled)
		profile_pipeline_ns += SDL_GetTicksNS() - compile_started;
	if (frame_log_started)
	{
		host_frame_acc.compile_ns += host_frame_log_now() - frame_log_started;
		host_frame_acc.compiles++;
	}
	if (!state)
	{
		host_log("Metal: no pipeline for shaders %u/%u: %s", draw->vertex_shader, draw->fragment_shader,
			error ? error.localizedDescription.UTF8String : "a shader or the vertex layout is missing");
	}
	else if (!(metal.debug & 4))
	{
		shader_cache_record_pipeline(&key);
	}
	cache_add(&pipelines, &key, sizeof(key), hash, state);
	return state;
}

/* ---------- the shader cache

Two records of the shaders and pipelines the game has made
(host_shader_manifest.h), each grouped by the map that made them:
- the app's own, in its caches (HALO_METAL_SHADER_CACHE: a file name there,
  a path, or 0 for none; HALO_METAL_SHADER_CACHE_RESET=1 empties it first,
  for measuring a first launch), written as the game plays: a shader's or a
  pipeline's record when it is made, and when a map draws one an earlier map
  made, a use record of it;
- the bundled pack (metal_shaders.pack in the app's bundle, made by
  tools/shader_pack.py from scripted visits of every campaign map on a Mac;
  HALO_METAL_SHADER_PACK: another file, or 0 for none), read only. Its
  header names the hash of the shader translators' sources it was made
  from; tools/tvos_build.py bundles it only if that is this build's (and a
  stale one could only cost time: what is prepared is used only for its
  exact text).
Nothing is compiled at launch. When a map begins to load (host_metal_map,
the guest's game_load) its shaders, of both, are compiled on other threads,
HALO_METAL_SHADER_PREPARE_JOBS (default 4) at a time, and then its
pipelines; HALO_METAL_SHADER_PREPARE_REST seconds after that (default 5, a
negative number never) every other shader the records have (the other maps'
sets: a map's draws sometimes use one only another map's visit recorded) is
compiled at a low priority, two at a time, and kept for the draws, which
for the whole campaign is about 260 shaders; HALO_METAL_SHADER_PREPARE_NEXT
seconds into the map (default 120, 0 never) the next map's pipelines, in
the pack's order, are made at a low priority and dropped, which leaves them
in the system's caches for when it loads. (The records of a build before the maps were recorded, no map's,
are compiled when the cache starts, as before.)

A draw's shader whose text is exactly a prepared one's (the same source,
compiled the same way: shader_text) takes the function compiled for it
(prepared_take), waiting for it if a thread is compiling it; every other is
compiled on the main thread as before, and finds the system's caches of
compiled shaders and pipelines filled by the threads (on a Mac, about 0.01
ms a shader instead of 9). The prepared pipelines are dropped: the draws
make their own, from the system's cache. So a record that is stale, of
another build or damaged can cost time but not change a picture. The main
thread alone writes the record and its tables; the threads have the
prepared shaders (the library), the main thread only what it takes from
them, under their lock. At each map's end and at exit the log says how many
of the shaders the map's draws asked for were prepared. */

/* a recorded shader (the library's): its source, its text (shader_text's),
made when it is to be compiled, and once compiled, its function. A draw's
compile of exactly this text and kind, on the main thread, takes the
function (the same compile's result) or waits for the thread compiling it;
one queued but not started yet is the main thread's to compile, and the
threads skip it */
enum
{
	/* not to be compiled (no map loading has it), or compiled and dropped */
	_prepared_idle,
	_prepared_queued,
	_prepared_compiling,
	_prepared_done,
	/* the main thread compiles it, or has taken it, or it does not build */
	_prepared_taken,
};

@interface PreparedShader : NSObject
@property(nonatomic, strong) NSData *source;
@property(atomic, strong) NSString *text;
@property(nonatomic) int fragment, state;
@property(nonatomic, strong) id<MTLFunction> function;
@end

@implementation PreparedShader
@end

/* a map's shaders (the library's indices, in the order they were first
made) and pipelines (shader_manifest_pipelines of those indices) */
@interface PreparedMap : NSObject
@property(nonatomic, strong) NSMutableOrderedSet<NSNumber *> *shaders;
@property(nonatomic, strong) NSMutableOrderedSet<NSData *> *pipelines;
@end

@implementation PreparedMap
- (instancetype)init
{
	if ((self = [super init]))
	{
		_shaders = [NSMutableOrderedSet orderedSet];
		_pipelines = [NSMutableOrderedSet orderedSet];
	}
	return self;
}
@end

/* a record's file as read: its shaders (the file's indices), and its maps'
sets of them ("" the records before any map's) */
@interface PreparedRead : NSObject
@property(nonatomic, strong) NSMutableArray<NSData *> *sources;
@property(nonatomic, strong) NSMutableData *fragments;
@property(nonatomic, strong) NSMutableDictionary<NSString *, PreparedMap *> *maps;
@property(nonatomic, strong) NSMutableArray<NSString *> *order;
@property(nonatomic, strong) NSString *map;
@end

@implementation PreparedRead
- (instancetype)init
{
	if ((self = [super init]))
	{
		_sources = [NSMutableArray array];
		_fragments = [NSMutableData data];
		_maps = [NSMutableDictionary dictionary];
		_order = [NSMutableArray array];
		_map = @"";
	}
	return self;
}

- (PreparedMap *)current
{
	PreparedMap *set = self.maps[self.map];

	if (!set)
	{
		set = [PreparedMap new];
		self.maps[self.map] = set;
		[self.order addObject:self.map];
	}
	return set;
}
@end

static struct
{
	BOOL enabled;
	struct shader_manifest manifest;
	/* each shader handle's record, plus 1 (0: none), and the map (epoch) it
	was last drawn in */
	uint32_t *shader_records, *shader_marks;
	/* the current map's: its number (from 1), its name (nil before the
	first), and the records' count when its map record was written (a
	shader recorded since is its own) */
	uint32_t epoch, segment_first;
	NSString *map;
	/* the records' sources (a hash of the kind and the source) to their
	indices; each map's shaders and pipelines the record has */
	NSMutableDictionary<NSNumber *, NSNumber *> *sources;
	NSMutableDictionary<NSString *, NSMutableSet<NSNumber *> *> *known_shaders;
	NSMutableDictionary<NSString *, NSMutableSet<NSData *> *> *known_pipelines;
	/* each vertex layout handle's layout */
	NSMutableDictionary<NSNumber *, NSData *> *layouts;
	/* the current map's draws' shaders: prepared, the draw's own compile of
	one queued, in no record, and a text compiled before (a second handle of
	the same source: the system's cache has it) (for the log) */
	uint32_t found, claimed, missed, again;
} shader_cache;

static struct
{
	/* the library (the prepare queue's, but each shader's state and
	function, which are under the lock): every recorded shader, the
	index of each source (by hash), each map's set, and the maps' order
	(the pack's, then the app record's) */
	NSMutableArray<PreparedShader *> *shaders;
	NSMutableDictionary<NSNumber *, NSNumber *> *by_source;
	NSMutableDictionary<NSString *, PreparedMap *> *maps;
	NSMutableArray<NSString *> *order;
	/* the queued and compiled ones by text (fragment's '1' or '0' first),
	and the texts the main thread compiled itself (not to be prepared) */
	NSMutableDictionary<NSString *, PreparedShader *> *by_text;
	NSMutableSet<NSString *> *compiled;
	/* guards the shaders' state and functions, by_text and compiled */
	NSCondition *condition;
	/* the maps' preparation, in order, and the next map's, at a low
	priority; a map's loading makes the number new, which ends both */
	dispatch_queue_t queue, next_queue;
	_Atomic uint32_t generation;
	int jobs;
	double next_seconds, rest_seconds;
	/* the map host_metal_map gave before the cache started */
	NSString *pending_map;
} prepared;

static uint64_t shader_cache_source_hash(const void *source, size_t length, int fragment)
{
	return hash_bytes(source, length) * 31 + (fragment ? 1 : 0);
}

static NSString *prepared_key(NSString *text, int fragment)
{
	return [(fragment ? @"1" : @"0") stringByAppendingString:text];
}

/* the app record's PreparedRead while it is read (its indices are the
record's: shader_cache.sources) */
static PreparedRead *shader_cache_read_target;

/* (the reader's, into a PreparedRead) */
static void shader_cache_read_shader(void *context, uint32_t index, int fragment, const char *source, uint32_t length)
{
	PreparedRead *read = (__bridge PreparedRead *)context;
	unsigned char kind = fragment ? 1 : 0;

	(void)index;
	[read.sources addObject:[NSData dataWithBytes:source length:length]];
	[read.fragments appendBytes:&kind length:1];
	[read.current.shaders addObject:@(read.sources.count - 1)];
	if (shader_cache.manifest.kept && shader_cache.sources && context == (__bridge void *)shader_cache_read_target)
		shader_cache.sources[@(shader_cache_source_hash(source, length, fragment))] = @(read.sources.count - 1);
}

static void shader_cache_read_pipeline(void *context, const struct shader_manifest_pipeline *pipeline)
{
	PreparedRead *read = (__bridge PreparedRead *)context;

	[read.current.pipelines addObject:[NSData dataWithBytes:pipeline length:sizeof(*pipeline)]];
}

static void shader_cache_read_map(void *context, const char *name)
{
	PreparedRead *read = (__bridge PreparedRead *)context;

	read.map = [NSString stringWithUTF8String:name];
}

static void shader_cache_read_shader_used(void *context, uint32_t index)
{
	PreparedRead *read = (__bridge PreparedRead *)context;

	[read.current.shaders addObject:@(index)];
}

static const struct shader_manifest_reader shader_cache_reader =
{
	NULL, shader_cache_read_shader, shader_cache_read_pipeline, shader_cache_read_map, shader_cache_read_shader_used,
};

/* a file's records into the library (the prepare queue's) */
static void prepared_merge(PreparedRead *read)
{
	NSMutableArray<NSNumber *> *global = [NSMutableArray arrayWithCapacity:read.sources.count];
	const unsigned char *fragments = read.fragments.bytes;
	NSUInteger index;

	for (index = 0; index < read.sources.count; index++)
	{
		NSData *source = read.sources[index];
		NSNumber *hash = @(shader_cache_source_hash(source.bytes, source.length, fragments[index]));
		NSNumber *found = prepared.by_source[hash];

		if (!found)
		{
			PreparedShader *shader = [PreparedShader new];

			shader.source = source;
			shader.fragment = fragments[index];
			shader.state = _prepared_idle;
			found = @(prepared.shaders.count);
			[prepared.shaders addObject:shader];
			prepared.by_source[hash] = found;
		}
		[global addObject:found];
	}
	for (NSString *name in read.order)
	{
		PreparedMap *from = read.maps[name], *into = prepared.maps[name];

		if (!into)
		{
			into = [PreparedMap new];
			prepared.maps[name] = into;
			[prepared.order addObject:name];
		}
		for (NSNumber *shader in from.shaders)
			[into.shaders addObject:global[shader.unsignedIntValue]];
		for (NSData *record in from.pipelines)
		{
			struct shader_manifest_pipeline pipeline;

			memcpy(&pipeline, record.bytes, sizeof(pipeline));
			if (pipeline.vertex_shader >= global.count || pipeline.fragment_shader >= global.count)
				continue;
			pipeline.vertex_shader = global[pipeline.vertex_shader].unsignedIntValue;
			pipeline.fragment_shader = global[pipeline.fragment_shader].unsignedIntValue;
			[into.pipelines addObject:[NSData dataWithBytes:&pipeline length:sizeof(pipeline)]];
		}
	}
}

/* a library shader's text (shader_text of its source), made once */
static NSString *prepared_text(PreparedShader *shader)
{
	NSString *text = shader.text;

	if (!text)
	{
		NSData *source = shader.source;
		char *copy = malloc(source.length + 1);

		if (!copy)
			return nil;
		memcpy(copy, source.bytes, source.length);
		copy[source.length] = 0;
		text = strlen(copy) == source.length ? shader_text(copy, shader.fragment) : nil;
		free(copy);
		shader.text = text;
	}
	return text;
}

/* the main thread's compile of this text: the prepared function, after
waiting for a thread compiling it; nil if it is the main thread's to compile
(*claimed then the library's entry, if it was queued, for prepared_give) */
static id<MTLFunction> prepared_take(NSString *text, int fragment, PreparedShader **claimed)
{
	PreparedShader *shader;
	id<MTLFunction> function = nil;
	NSString *key;
	BOOL seen;

	*claimed = nil;
	if (!prepared.condition)
		return nil;
	key = prepared_key(text, fragment);
	[prepared.condition lock];
	shader = prepared.by_text[key];
	if (shader)
	{
		while (shader.state == _prepared_compiling)
			[prepared.condition wait];
		if (shader.state == _prepared_done)
			function = shader.function;
		else if (shader.state == _prepared_queued)
		{
			shader.state = _prepared_taken;
			*claimed = shader;
		}
		/* (one entry for one handle: a second shader of the same source
		compiles its own, as before) */
		[prepared.by_text removeObjectForKey:key];
	}
	seen = !shader && [prepared.compiled containsObject:key];
	[prepared.compiled addObject:key];
	[prepared.condition unlock];
	if (function)
		shader_cache.found++;
	else if (*claimed)
		shader_cache.claimed++;
	else if (seen)
		shader_cache.again++;
	else
		shader_cache.missed++;
	return function;
}

/* the function the main thread compiled for a claimed entry, for the
pipelines the threads prepare */
static void prepared_give(PreparedShader *claimed, id<MTLFunction> function)
{
	if (!claimed || !function)
		return;
	[prepared.condition lock];
	claimed.function = function;
	claimed.state = _prepared_done;
	[prepared.condition unlock];
}

/* a recorded pipeline's state within what Metal's enumerations take (a
record is checked, but may be another build's) */
static BOOL shader_cache_pipeline_sane(const struct shader_manifest_pipeline *pipeline)
{
	uint32_t index;

	if (pipeline->color_format > 1000 || pipeline->depth_format > 1000 || pipeline->blend.source > 18 ||
		pipeline->blend.destination > 18 || pipeline->blend.operation > 4 || pipeline->blend.write_mask > 15)
	{
		return NO;
	}
	for (index = 0; index < METAL_VERTEX_ATTRIBUTES; index++)
	{
		const struct metal_vertex_attribute *attribute = &pipeline->layout.attributes[index];

		if (attribute->format != METAL_VERTEX_FORMAT_INVALID && (attribute->format > 64 ||
			attribute->buffer >= METAL_VERTEX_BUFFERS || !pipeline->layout.buffers[attribute->buffer].stride ||
			attribute->offset > 4096))
		{
			return NO;
		}
	}
	for (index = 0; index < METAL_VERTEX_BUFFERS; index++)
	{
		if (pipeline->layout.buffers[index].stride > 4096)
			return NO;
	}
	return YES;
}

static MTLRenderPipelineDescriptor *prepared_pipeline_descriptor(const struct shader_manifest_pipeline *pipeline,
	id<MTLFunction> vertex, id<MTLFunction> fragment)
{
	if (!vertex || !fragment || !shader_cache_pipeline_sane(pipeline))
		return nil;
	return pipeline_descriptor_make(vertex, fragment, vertex_descriptor_make(&pipeline->layout), pipeline->color_format,
		pipeline->depth_format, &pipeline->blend);
}

static BOOL prepared_current(uint32_t generation)
{
	return atomic_load(&prepared.generation) == generation;
}

static void prepare_next_schedule(NSString *name, uint32_t generation);
static void prepare_rest_schedule(uint32_t generation);

/* a map's set compiled (the prepare queue's): its shaders, kept for the
draws, then its pipelines, dropped; the previous map's that no draw took
dropped first. Ends early if another map begins to load */
static void prepare_map(NSString *name, uint32_t generation)
{
	PreparedMap *set = prepared.maps[name];
	id<MTLDevice> device = metal.device;
	MTLCompileOptions *options = compile_options();
	NSMutableArray<PreparedShader *> *todo = [NSMutableArray array];
	NSMutableSet<NSNumber *> *members = [NSMutableSet set];
	dispatch_semaphore_t slots = dispatch_semaphore_create(prepared.jobs);
	dispatch_group_t group = dispatch_group_create();
	__block _Atomic uint32_t compiled = 0, failed_shaders = 0, made_pipelines = 0, failed_pipelines = 0;
	uint64_t started = SDL_GetTicksNS(), shaders_done;
	NSUInteger skipped = 0, dropped = 0, pipelines_tried = 0;

	if (!set)
	{
		if (name.length)
			host_log("Metal: shader cache: no shaders recorded for map %s", name.UTF8String);
		return;
	}
	for (NSNumber *index in set.shaders)
	{
		[members addObject:index];
		prepared_text(prepared.shaders[index.unsignedIntValue]);
	}
	[prepared.condition lock];
	/* (what the last map's draws did not take, and this map has not, is
	dropped: its memory, and the dictionary's) */
	for (NSString *key in prepared.by_text.allKeys)
	{
		PreparedShader *shader = prepared.by_text[key];
		NSNumber *index = prepared.by_source[@(shader_cache_source_hash(shader.source.bytes, shader.source.length,
			shader.fragment))];

		if ((shader.state == _prepared_done || shader.state == _prepared_queued) && ![members containsObject:index])
		{
			shader.function = nil;
			shader.state = _prepared_idle;
			[prepared.by_text removeObjectForKey:key];
			dropped++;
		}
	}
	for (NSNumber *index in set.shaders)
	{
		PreparedShader *shader = prepared.shaders[index.unsignedIntValue];
		NSString *key;

		if (shader.state != _prepared_idle || !shader.text)
			continue;
		key = prepared_key(shader.text, shader.fragment);
		if ([prepared.compiled containsObject:key] || prepared.by_text[key])
		{
			skipped++;
			continue;
		}
		prepared.by_text[key] = shader;
		shader.state = _prepared_queued;
		[todo addObject:shader];
	}
	[prepared.condition unlock];

	for (PreparedShader *shader in todo)
	{
		dispatch_semaphore_wait(slots, DISPATCH_TIME_FOREVER);
		[prepared.condition lock];
		if (!prepared_current(generation) || shader.state != _prepared_queued)
		{
			/* (another map's loading: the rest are not compiled) */
			if (shader.state == _prepared_queued && !prepared_current(generation))
			{
				shader.state = _prepared_idle;
				[prepared.by_text removeObjectForKey:prepared_key(shader.text, shader.fragment)];
			}
			[prepared.condition unlock];
			dispatch_semaphore_signal(slots);
			continue;
		}
		shader.state = _prepared_compiling;
		[prepared.condition unlock];
		dispatch_group_enter(group);
		[device newLibraryWithSource:shader.text options:options
			completionHandler:^(id<MTLLibrary> library, NSError *error)
		{
			id<MTLFunction> function = [library newFunctionWithName:shader_entry_point(shader.fragment)];

			(void)error;
			[prepared.condition lock];
			shader.function = function;
			shader.state = function ? _prepared_done : _prepared_taken;
			[prepared.condition broadcast];
			[prepared.condition unlock];
			atomic_fetch_add(function ? &compiled : &failed_shaders, 1);
			dispatch_semaphore_signal(slots);
			dispatch_group_leave(group);
		}];
	}
	dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
	shaders_done = SDL_GetTicksNS();
	for (NSData *record in set.pipelines)
	{
		struct shader_manifest_pipeline pipeline;
		id<MTLFunction> vertex, fragment;
		MTLRenderPipelineDescriptor *descriptor;

		if (!prepared_current(generation))
			break;
		memcpy(&pipeline, record.bytes, sizeof(pipeline));
		[prepared.condition lock];
		vertex = prepared.shaders[pipeline.vertex_shader].function;
		fragment = prepared.shaders[pipeline.fragment_shader].function;
		[prepared.condition unlock];
		/* (one whose shader failed, or that a draw compiled itself before
		it was prepared, is the draw's to make) */
		if (!(descriptor = prepared_pipeline_descriptor(&pipeline, vertex, fragment)))
			continue;
		pipelines_tried++;
		dispatch_semaphore_wait(slots, DISPATCH_TIME_FOREVER);
		dispatch_group_enter(group);
		[device newRenderPipelineStateWithDescriptor:descriptor
			completionHandler:^(id<MTLRenderPipelineState> state, NSError *error)
		{
			(void)error;
			atomic_fetch_add(state ? &made_pipelines : &failed_pipelines, 1);
			dispatch_semaphore_signal(slots);
			dispatch_group_leave(group);
		}];
	}
	dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
	host_log("Metal: shader cache: map %s: %u of %lu shaders prepared in %.0f ms (%u failed, %lu the draws had "
		"already, %lu dropped), %u of %lu pipelines in %.0f ms (%u failed), %d at a time, on other threads%s",
		name.length ? name.UTF8String : "(none)", atomic_load(&compiled), (unsigned long)set.shaders.count,
		(shaders_done - started) / 1e6, atomic_load(&failed_shaders), (unsigned long)skipped, (unsigned long)dropped,
		atomic_load(&made_pipelines), (unsigned long)set.pipelines.count, (SDL_GetTicksNS() - shaders_done) / 1e6,
		atomic_load(&failed_pipelines), prepared.jobs, prepared_current(generation) ? "" : " (cut short: another map)");
	(void)pipelines_tried;
	if (prepared_current(generation) && name.length)
	{
		prepare_rest_schedule(generation);
		prepare_next_schedule(name, generation);
	}
}

/* every library shader no map loading has queued, compiled at a low
priority, a few at a time, and kept for the draws as a map's are (the next
queue's). Ends as soon as a map begins to load */
static void prepare_rest(uint32_t generation)
{
	__block NSArray<PreparedShader *> *shaders = nil;
	NSMutableArray<PreparedShader *> *todo = [NSMutableArray array];
	id<MTLDevice> device = metal.device;
	MTLCompileOptions *options = compile_options();
	dispatch_semaphore_t slots = dispatch_semaphore_create(2);
	dispatch_group_t group = dispatch_group_create();
	__block _Atomic uint32_t compiled = 0;
	uint64_t started = SDL_GetTicksNS();

	dispatch_sync(prepared.queue, ^
	{
		shaders = [prepared.shaders copy];
	});
	for (PreparedShader *shader in shaders)
	{
		NSString *text, *key;

		if (!prepared_current(generation))
			return;
		if (!(text = prepared_text(shader)))
			continue;
		key = prepared_key(text, shader.fragment);
		[prepared.condition lock];
		if (shader.state == _prepared_idle && ![prepared.compiled containsObject:key] && !prepared.by_text[key])
		{
			prepared.by_text[key] = shader;
			shader.state = _prepared_queued;
			[todo addObject:shader];
		}
		[prepared.condition unlock];
	}
	for (PreparedShader *shader in todo)
	{
		dispatch_semaphore_wait(slots, DISPATCH_TIME_FOREVER);
		[prepared.condition lock];
		if (shader.state != _prepared_queued || !prepared_current(generation))
		{
			[prepared.condition unlock];
			dispatch_semaphore_signal(slots);
			continue;
		}
		shader.state = _prepared_compiling;
		[prepared.condition unlock];
		dispatch_group_enter(group);
		[device newLibraryWithSource:shader.text options:options
			completionHandler:^(id<MTLLibrary> library, NSError *error)
		{
			id<MTLFunction> function = [library newFunctionWithName:shader_entry_point(shader.fragment)];

			(void)error;
			[prepared.condition lock];
			shader.function = function;
			shader.state = function ? _prepared_done : _prepared_taken;
			[prepared.condition broadcast];
			[prepared.condition unlock];
			if (function)
				atomic_fetch_add(&compiled, 1);
			dispatch_semaphore_signal(slots);
			dispatch_group_leave(group);
		}];
	}
	dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
	if (todo.count)
		host_log("Metal: shader cache: the other maps' %u of %lu shaders compiled in %.0f ms at a low priority%s",
			atomic_load(&compiled), (unsigned long)todo.count, (SDL_GetTicksNS() - started) / 1e6,
			prepared_current(generation) ? "" : " (cut short: another map)");
}

static void prepare_rest_schedule(uint32_t generation)
{
	if (prepared.rest_seconds < 0)
		return;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(prepared.rest_seconds * NSEC_PER_SEC)),
		prepared.next_queue, ^
	{
		if (prepared_current(generation))
			prepare_rest(generation);
	});
}

/* the next map's set (the pack's order) compiled at a low priority, one at
a time, and dropped: the system's caches have them when it loads (the next
queue's). Ends as soon as a map begins to load */
static void prepare_next(NSString *name, uint32_t generation)
{
	__block PreparedMap *set = nil;
	__block NSArray<PreparedShader *> *shaders = nil;
	id<MTLDevice> device = metal.device;
	MTLCompileOptions *options = compile_options();
	NSMutableDictionary<NSNumber *, id<MTLFunction>> *functions = [NSMutableDictionary dictionary];
	uint64_t started = SDL_GetTicksNS();
	uint32_t compiled = 0, pipelines = 0;

	/* (the library is the prepare queue's: its set is taken there) */
	dispatch_sync(prepared.queue, ^
	{
		set = prepared.maps[name];
		shaders = [prepared.shaders copy];
	});
	if (!set)
		return;
	for (NSNumber *index in set.shaders)
	{
		@autoreleasepool
		{
			PreparedShader *shader = shaders[index.unsignedIntValue];
			NSString *text = prepared_text(shader);
			BOOL skip;
			id<MTLLibrary> library;
			NSError *error = nil;

			if (!prepared_current(generation))
				return;
			if (!text)
				continue;
			[prepared.condition lock];
			skip = shader.state != _prepared_idle ||
				[prepared.compiled containsObject:prepared_key(text, shader.fragment)];
			if (shader.function)
				functions[index] = shader.function;
			[prepared.condition unlock];
			if (skip)
				continue;
			library = [device newLibraryWithSource:text options:options error:&error];
			if ((functions[index] = [library newFunctionWithName:shader_entry_point(shader.fragment)]))
				compiled++;
		}
	}
	for (NSData *record in set.pipelines)
	{
		@autoreleasepool
		{
			struct shader_manifest_pipeline pipeline;
			MTLRenderPipelineDescriptor *descriptor;
			NSError *error = nil;

			if (!prepared_current(generation))
				return;
			memcpy(&pipeline, record.bytes, sizeof(pipeline));
			descriptor = prepared_pipeline_descriptor(&pipeline, functions[@(pipeline.vertex_shader)],
				functions[@(pipeline.fragment_shader)]);
			if (descriptor && [device newRenderPipelineStateWithDescriptor:descriptor error:&error])
				pipelines++;
		}
	}
	host_log("Metal: shader cache: the next map's (%s) %u shaders and %u pipelines compiled ahead in %.0f ms, "
		"at a low priority", name.UTF8String, compiled, pipelines, (SDL_GetTicksNS() - started) / 1e6);
}

/* (the prepare queue's, after a map's set) the next map's, a while into
this one, if this one is still loaded then */
static void prepare_next_schedule(NSString *name, uint32_t generation)
{
	NSUInteger at = [prepared.order indexOfObject:name];
	NSString *next;

	if (prepared.next_seconds <= 0 || at == NSNotFound || at + 1 >= prepared.order.count)
		return;
	next = prepared.order[at + 1];
	if (!next.length || [next isEqualToString:@"ui"])
		return;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(prepared.next_seconds * NSEC_PER_SEC)),
		prepared.next_queue, ^
	{
		if (prepared_current(generation))
			prepare_next(next, generation);
	});
}

/* the app's build (its executable's UUID) */
static NSString *build_identifier(void)
{
	const struct mach_header_64 *header = (const struct mach_header_64 *)_dyld_get_image_header(0);
	const struct load_command *command;
	uint32_t index;

	if (!header || header->magic != MH_MAGIC_64)
		return @"unknown";
	command = (const struct load_command *)(header + 1);
	for (index = 0; index < header->ncmds; index++)
	{
		if (command->cmd == LC_UUID)
		{
			const uint8_t *uuid = ((const struct uuid_command *)command)->uuid;
			NSMutableString *text = [NSMutableString string];
			int byte;

			for (byte = 0; byte < 16; byte++)
				[text appendFormat:@"%02x", uuid[byte]];
			return text;
		}
		command = (const struct load_command *)((const uint8_t *)command + command->cmdsize);
	}
	return @"unknown";
}

/* how many of the map's draws' new shaders were prepared, for the log */
static void shader_cache_coverage_log(void)
{
	uint32_t total = shader_cache.found + shader_cache.claimed + shader_cache.missed;

	if (!total || !prepared.condition)
		return;
	host_log("Metal: shader cache: map %s: of the %u new shader texts its draws compiled, %u were prepared, %u "
		"queued but compiled by the draw first, %u in no record (and %u texts again, for another handle)",
		shader_cache.map ? shader_cache.map.UTF8String : "(none)", total, shader_cache.found, shader_cache.claimed,
		shader_cache.missed, shader_cache.again);
	shader_cache.found = shader_cache.claimed = shader_cache.missed = shader_cache.again = 0;
}

/* the pack: the bundle's, or HALO_METAL_SHADER_PACK's; NULL for none */
static NSString *shader_pack_path(BOOL *any_build)
{
	const char *setting = getenv("HALO_METAL_SHADER_PACK");

	*any_build = NO;
	if (setting && *setting)
	{
		if (!strcmp(setting, "0") || !strcmp(setting, "off"))
			return nil;
		*any_build = YES;
		return [NSString stringWithUTF8String:setting];
	}
	return [[NSBundle mainBundle] pathForResource:@"metal_shaders" ofType:@"pack"];
}

/* the record opened, the pack read, and the records of no map prepared */
static void shader_cache_start(void)
{
	const char *setting = getenv("HALO_METAL_SHADER_CACHE");
	const char *jobs = getenv("HALO_METAL_SHADER_PREPARE_JOBS"), *next = getenv("HALO_METAL_SHADER_PREPARE_NEXT");
	NSString *path, *key, *pack_key, *build, *pack;
	struct shader_manifest_reader reader = shader_cache_reader;
	MTLCompileOptions *options = compile_options();
	uint64_t started = SDL_GetTicksNS();
	PreparedRead *read = [PreparedRead new];
	BOOL recorded, any_build;


	shader_settings_read();
	prepared.shaders = [NSMutableArray array];
	prepared.by_source = [NSMutableDictionary dictionary];
	prepared.maps = [NSMutableDictionary dictionary];
	prepared.order = [NSMutableArray array];
	prepared.by_text = [NSMutableDictionary dictionary];
	prepared.compiled = [NSMutableSet set];
	prepared.jobs = jobs && atoi(jobs) > 0 ? MIN(atoi(jobs), 16) : 4;
	prepared.next_seconds = next && *next ? atof(next) : 120.0;
	prepared.rest_seconds = getenv("HALO_METAL_SHADER_PREPARE_REST") && *getenv("HALO_METAL_SHADER_PREPARE_REST") ?
		atof(getenv("HALO_METAL_SHADER_PREPARE_REST")) : 5.0;
	prepared.queue = dispatch_queue_create("opence.shader-prepare",
		dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0));
	prepared.next_queue = dispatch_queue_create("opence.shader-prepare-next",
		dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
	/* (what must match for the records to be read: what the shaders are
	compiled as, and for the app's own the GPU; what must match for the
	app's to be kept: the build that wrote them, and the system; for the
	pack's to be read: the shader translators it was made by) */
	pack_key = [NSString stringWithFormat:@"msl %lx|invariance %d|layout %zu", (unsigned long)options.languageVersion,
		options.preserveInvariance ? 1 : 0, sizeof(struct shader_manifest_pipeline)];
	key = [NSString stringWithFormat:@"gpu %@|%@", metal.device.name, pack_key];
	build = [NSString stringWithFormat:@"%@|%@", build_identifier(),
		[NSProcessInfo processInfo].operatingSystemVersionString];

	recorded = !(setting && (!strcmp(setting, "0") || !strcmp(setting, "off")));
	if (!recorded)
		host_log("Metal: shader cache: no record kept (HALO_METAL_SHADER_CACHE)");
	if (recorded)
	{
		if (setting && *setting == '/')
		{
			path = [NSString stringWithUTF8String:setting];
		}
		else
		{
			NSURL *caches = [[NSFileManager defaultManager] URLsForDirectory:NSCachesDirectory
				inDomains:NSUserDomainMask].firstObject;
			NSString *folder = [caches.path stringByAppendingPathComponent:
				[NSBundle mainBundle].bundleIdentifier ? [NSBundle mainBundle].bundleIdentifier : @"OpenCE"];

			[[NSFileManager defaultManager] createDirectoryAtPath:folder withIntermediateDirectories:YES
				attributes:nil error:nil];
			path = [folder stringByAppendingPathComponent:setting && *setting ?
				[[NSString stringWithUTF8String:setting] lastPathComponent] : @"metal_shaders.manifest"];
		}
		if (getenv("HALO_METAL_SHADER_CACHE_RESET") && atoi(getenv("HALO_METAL_SHADER_CACHE_RESET")))
			unlink(path.fileSystemRepresentation);
		shader_cache.sources = [NSMutableDictionary dictionary];
		shader_cache.known_shaders = [NSMutableDictionary dictionary];
		shader_cache.known_pipelines = [NSMutableDictionary dictionary];
		shader_cache.layouts = [NSMutableDictionary dictionary];
		reader.context = (__bridge void *)read;
		shader_cache_read_target = read;
		if (!shader_manifest_open(&shader_cache.manifest, path.fileSystemRepresentation, key.UTF8String,
			build.UTF8String, &reader))
		{
			host_log("Metal: shader cache: %s cannot be opened (another copy of the game has it?): none kept",
				path.fileSystemRepresentation);
			shader_cache.sources = nil;
			shader_cache.layouts = nil;
		}
		else
		{
			shader_cache.shader_records = calloc(HANDLE_COUNT, sizeof(*shader_cache.shader_records));
			shader_cache.shader_marks = calloc(HANDLE_COUNT, sizeof(*shader_cache.shader_marks));
			shader_cache.enabled = shader_cache.shader_records && shader_cache.shader_marks;
			shader_cache_noting = shader_cache.enabled;
			shader_cache.epoch = 1;
			/* (what each map has in the record, so that none is recorded
			twice) */
			if (shader_cache.manifest.kept)
			{
				for (NSString *name in read.order)
				{
					NSMutableSet<NSNumber *> *shaders = [NSMutableSet setWithArray:read.maps[name].shaders.array];
					NSMutableSet<NSData *> *pipelines = [NSMutableSet setWithArray:read.maps[name].pipelines.array];

					shader_cache.known_shaders[name] = shaders;
					shader_cache.known_pipelines[name] = pipelines;
				}
			}
		}
		shader_cache_read_target = nil;
		host_log("Metal: shader cache: %s: %lu shaders of %lu maps read in %.1f ms%s%s%s",
			path.fileSystemRepresentation, (unsigned long)read.sources.count, (unsigned long)read.order.count,
			(SDL_GetTicksNS() - started) / 1e6,
			shader_cache.manifest.kept || !read.sources.count ? "" : " (another build's: started again)",
			shader_cache.manifest.discarded[0] ? "; not read past " : "", shader_cache.manifest.discarded);
	}
	prepared.condition = [NSCondition new];
	atexit(shader_cache_coverage_log);

	pack = shader_pack_path(&any_build);
	dispatch_async(prepared.queue, ^
	{
		uint64_t pack_started = SDL_GetTicksNS();

		if (pack)
		{
			PreparedRead *pack_read = [PreparedRead new];
			struct shader_manifest_reader pack_reader = shader_cache_reader;
			struct shader_manifest info;

			pack_reader.context = (__bridge void *)pack_read;
			if (shader_manifest_read_pack(pack.fileSystemRepresentation, pack_key.UTF8String, NULL,
				&pack_reader, &info))
			{
				prepared_merge(pack_read);
				host_log("Metal: shader cache: the pack %s (%s): %lu shaders of %lu maps read in %.1f ms%s%s",
					pack.lastPathComponent.UTF8String, info.build, (unsigned long)pack_read.sources.count,
					(unsigned long)pack_read.order.count, (SDL_GetTicksNS() - pack_started) / 1e6,
					info.discarded[0] ? "; not read past " : "", info.discarded);
			}
			else
			{
				/* (silent but for the log: none, or another build's) */
				host_log("Metal: shader cache: the pack %s is not read: %s", pack.fileSystemRepresentation,
					info.discarded);
			}
		}
		prepared_merge(read);
		/* (the records of no map: an earlier build's, prepared now) */
		prepare_map(@"", atomic_load(&prepared.generation));
	});
	if (prepared.pending_map)
	{
		NSString *map = prepared.pending_map;

		prepared.pending_map = nil;
		host_metal_map(map.UTF8String);
	}
}

/* a map begins to load (the guest's game_load: "levels\\a10\\a10"): the
record's records are of it from now on, and its set is prepared */
void host_metal_map(const char *scenario)
{
	NSMutableString *name = [NSMutableString string];
	const char *at, *start = scenario;
	uint32_t generation;

	if (!scenario)
		return;
	for (at = scenario; *at; at++)
	{
		if (*at == '\\' || *at == '/')
			start = at + 1;
	}
	for (at = start; *at && name.length < SHADER_MANIFEST_MAP_NAME_LIMIT; at++)
	{
		char letter = *at >= 'A' && *at <= 'Z' ? (char)(*at - 'A' + 'a') : *at;

		if ((letter >= 'a' && letter <= 'z') || (letter >= '0' && letter <= '9') || letter == '_' || letter == '-')
			[name appendFormat:@"%c", letter];
	}
	if (!name.length)
		return;
	host_frame_log_event("shader-map", name.UTF8String);
	if (!prepared.queue)
	{
		prepared.pending_map = name;
		return;
	}
	shader_cache_coverage_log();
	if (shader_cache.enabled)
	{
		shader_cache.epoch++;
		shader_cache.map = name;
		shader_manifest_add_map(&shader_cache.manifest, name.UTF8String);
		shader_cache.segment_first = shader_cache.manifest.shaders;
		if (!shader_cache.known_shaders[name])
			shader_cache.known_shaders[name] = [NSMutableSet set];
		if (!shader_cache.known_pipelines[name])
			shader_cache.known_pipelines[name] = [NSMutableSet set];
	}
	else
	{
		shader_cache.map = name;
	}
	generation = atomic_fetch_add(&prepared.generation, 1) + 1;
	dispatch_async(prepared.queue, ^
	{
		prepare_map(name, generation);
	});
}

static void shader_cache_record_shader(uint32_t handle, const char *source, int fragment)
{
	NSNumber *key, *index;
	size_t length;

	if (!shader_cache.enabled || handle >= HANDLE_COUNT)
		return;
	length = strlen(source);
	key = @(shader_cache_source_hash(source, length, fragment));
	index = shader_cache.sources[key];
	if (!index)
	{
		uint64_t started = host_frame_log_detail() ? host_frame_log_now() : 0;
		uint32_t added = shader_manifest_add_shader(&shader_cache.manifest, fragment, source, (uint32_t)length);

		/* (a record appended: one write, no sync; the telemetry's) */
		if (started)
		{
			host_frame_acc.manifest_writes++;
			host_frame_acc.manifest_bytes += length;
			host_frame_acc.manifest_ns += host_frame_log_now() - started;
		}

		if (added == ~0u)
			return;
		index = @(added);
		shader_cache.sources[key] = index;
	}
	shader_cache.shader_records[handle] = index.unsignedIntValue + 1;
}

/* a draw's shader: if it is the first of this map's draws to use it, and
an earlier map's record has it, recorded as this map's too */
static void shader_cache_note(uint32_t handle)
{
	NSMutableSet<NSNumber *> *known;
	uint32_t index;

	if (handle >= HANDLE_COUNT || shader_cache.shader_marks[handle] == shader_cache.epoch)
		return;
	shader_cache.shader_marks[handle] = shader_cache.epoch;
	if (!shader_cache.shader_records[handle] || !shader_cache.map)
		return;
	index = shader_cache.shader_records[handle] - 1;
	/* (recorded since the map's record: its own) */
	if (index >= shader_cache.segment_first)
		return;
	known = shader_cache.known_shaders[shader_cache.map];
	if ([known containsObject:@(index)])
		return;
	if (shader_manifest_add_shader_use(&shader_cache.manifest, index))
		[known addObject:@(index)];
}

static void shader_cache_record_layout(uint32_t handle, const struct metal_vertex_layout *layout)
{
	if (shader_cache.enabled)
		shader_cache.layouts[@(handle)] = [NSData dataWithBytes:layout length:sizeof(*layout)];
}

static void shader_cache_record_pipeline(const struct pipeline_key *key)
{
	struct shader_manifest_pipeline pipeline;
	NSData *layout, *record;
	NSMutableSet<NSData *> *known;

	if (!shader_cache.enabled || key->vertex_shader >= HANDLE_COUNT || key->fragment_shader >= HANDLE_COUNT ||
		!shader_cache.shader_records[key->vertex_shader] || !shader_cache.shader_records[key->fragment_shader] ||
		!(layout = shader_cache.layouts[@(key->vertex_layout)]))
	{
		return;
	}
	memset(&pipeline, 0, sizeof(pipeline));
	pipeline.vertex_shader = shader_cache.shader_records[key->vertex_shader] - 1;
	pipeline.fragment_shader = shader_cache.shader_records[key->fragment_shader] - 1;
	pipeline.color_format = key->color_format;
	pipeline.depth_format = key->depth_format;
	pipeline.blend = key->blend;
	memcpy(&pipeline.layout, layout.bytes, sizeof(pipeline.layout));
	record = [NSData dataWithBytes:&pipeline length:sizeof(pipeline)];
	known = shader_cache.known_pipelines[shader_cache.map ? shader_cache.map : @""];
	if (!known)
	{
		known = [NSMutableSet set];
		shader_cache.known_pipelines[shader_cache.map ? shader_cache.map : @""] = known;
	}
	if ([known containsObject:record])
		return;
	{
		uint64_t started = host_frame_log_detail() ? host_frame_log_now() : 0;

		if (shader_manifest_add_pipeline(&shader_cache.manifest, &pipeline))
			[known addObject:record];
		if (started)
		{
			host_frame_acc.manifest_writes++;
			host_frame_acc.manifest_bytes += sizeof(pipeline);
			host_frame_acc.manifest_ns += host_frame_log_now() - started;
		}
	}
}

static id<MTLDepthStencilState> depth_stencil_get(const struct metal_depth_stencil *state_key)
{
	uint64_t hash;
	BOOL found;
	void *object = cache_find(&depth_stencils, state_key, sizeof(*state_key), &hash, &found);
	MTLDepthStencilDescriptor *descriptor;
	id<MTLDepthStencilState> state;

	if (found)
		return (__bridge id<MTLDepthStencilState>)object;
	descriptor = [MTLDepthStencilDescriptor new];
	descriptor.depthCompareFunction = state_key->depth_test ? (MTLCompareFunction)state_key->depth_function :
		MTLCompareFunctionAlways;
	descriptor.depthWriteEnabled = state_key->depth_test && state_key->depth_write ? YES : NO;
	if (state_key->stencil_test)
	{
		MTLStencilDescriptor *stencil = [MTLStencilDescriptor new];

		stencil.stencilCompareFunction = (MTLCompareFunction)state_key->stencil_function;
		stencil.stencilFailureOperation = (MTLStencilOperation)state_key->stencil_fail;
		stencil.depthFailureOperation = (MTLStencilOperation)state_key->stencil_depth_fail;
		stencil.depthStencilPassOperation = (MTLStencilOperation)state_key->stencil_pass;
		stencil.readMask = state_key->stencil_read_mask;
		stencil.writeMask = state_key->stencil_write_mask;
		descriptor.frontFaceStencil = stencil;
		descriptor.backFaceStencil = stencil;
	}
	state = [metal.device newDepthStencilStateWithDescriptor:descriptor];
	cache_add(&depth_stencils, state_key, sizeof(*state_key), hash, state);
	return state;
}

static id<MTLSamplerState> sampler_get(const struct metal_sampler *sampler_key)
{
	uint64_t hash;
	BOOL found;
	void *object = cache_find(&samplers, sampler_key, sizeof(*sampler_key), &hash, &found);
	MTLSamplerDescriptor *descriptor;
	id<MTLSamplerState> state;

	if (found)
		return (__bridge id<MTLSamplerState>)object;
	descriptor = [MTLSamplerDescriptor new];
	descriptor.minFilter = (MTLSamplerMinMagFilter)sampler_key->min_filter;
	descriptor.magFilter = (MTLSamplerMinMagFilter)sampler_key->mag_filter;
	descriptor.mipFilter = (MTLSamplerMipFilter)sampler_key->mip_filter;
	descriptor.sAddressMode = (MTLSamplerAddressMode)sampler_key->address_u;
	descriptor.tAddressMode = (MTLSamplerAddressMode)sampler_key->address_v;
	descriptor.rAddressMode = (MTLSamplerAddressMode)sampler_key->address_w;
	descriptor.borderColor = (MTLSamplerBorderColor)sampler_key->border;
	descriptor.maxAnisotropy = sampler_key->anisotropy < 1 ? 1 : sampler_key->anisotropy > 16 ? 16 :
		sampler_key->anisotropy;
	descriptor.lodMinClamp = sampler_key->min_lod;
	state = [metal.device newSamplerStateWithDescriptor:descriptor];
	cache_add(&samplers, sampler_key, sizeof(*sampler_key), hash, state);
	return state;
}

/* ---------- clears */

struct clear_pipeline_key
{
	uint32_t color_format, depth_format, color_mask;
};

static id<MTLRenderPipelineState> clear_pipeline_get(uint32_t color_mask)
{
	struct clear_pipeline_key key = { (uint32_t)pass.color_format, (uint32_t)pass.depth_format, color_mask };
	uint64_t hash;
	BOOL found;
	void *object = cache_find(&clear_pipelines, &key, sizeof(key), &hash, &found);
	MTLRenderPipelineDescriptor *descriptor;
	id<MTLRenderPipelineState> state;
	NSError *error = nil;

	if (found)
		return (__bridge id<MTLRenderPipelineState>)object;
	descriptor = [MTLRenderPipelineDescriptor new];
	descriptor.vertexFunction = metal.clear_vertex;
	descriptor.fragmentFunction = metal.clear_fragment;
	descriptor.colorAttachments[0].pixelFormat = pass.color_format;
	descriptor.colorAttachments[0].writeMask = color_mask;
	descriptor.depthAttachmentPixelFormat = pass.depth_format;
	descriptor.stencilAttachmentPixelFormat = pass.depth_format;
	uint64_t frame_log_started = host_frame_log_detail() ? host_frame_log_now() : 0;
	state = [metal.device newRenderPipelineStateWithDescriptor:descriptor error:&error];
	if (frame_log_started)
	{
		host_frame_acc.compile_ns += host_frame_log_now() - frame_log_started;
		host_frame_acc.compiles++;
	}
	if (!state)
		host_log("Metal: no clear pipeline: %s", error.localizedDescription.UTF8String);
	cache_add(&clear_pipelines, &key, sizeof(key), hash, state);
	return state;
}

/* the scissor of a rectangle, inside the pass's targets; NO if it has no
pixels in them */
static BOOL scissor_inside(const metal_u32 rectangle[4], MTLScissorRect *scissor)
{
	uint64_t right = (uint64_t)rectangle[0] + rectangle[2], bottom = (uint64_t)rectangle[1] + rectangle[3];

	if (rectangle[0] >= pass.width || rectangle[1] >= pass.height || !rectangle[2] || !rectangle[3])
		return NO;
	scissor->x = rectangle[0];
	scissor->y = rectangle[1];
	scissor->width = (right > pass.width ? pass.width : right) - rectangle[0];
	scissor->height = (bottom > pass.height ? pass.height : bottom) - rectangle[1];
	return YES;
}

void host_metal_clear(const struct metal_clear *clear)
{
	HOST_FRAME_LOG_SCOPE;
	id<MTLTexture> color = texture_object(pass.color), depth = texture_object(pass.depth);
	uint32_t width, height, color_mask;
	BOOL whole;

	if (!color && !depth)
		return;
	color_mask = color ? clear->color_mask : 0;
	if (!color_mask && !(depth && (clear->depth || clear->stencil)))
		return;
	width = (uint32_t)(color ? color.width : depth.width);
	height = (uint32_t)(color ? color.height : depth.height);
	if (color && depth)
	{
		if (depth.width < width)
			width = (uint32_t)depth.width;
		if (depth.height < height)
			height = (uint32_t)depth.height;
	}
	whole = clear->rectangle[0] == 0 && clear->rectangle[1] == 0 && clear->rectangle[2] >= width &&
		clear->rectangle[3] >= height;
	/* nothing drawn in the pass yet, and the whole of the targets: their
	load actions (each attachment cleared whole, or kept) */
	if (!encoder && whole && (!color_mask ||
		color_mask == (METAL_WRITE_RED | METAL_WRITE_GREEN | METAL_WRITE_BLUE | METAL_WRITE_ALPHA)))
	{
		if (color_mask)
		{
			pass.color_load = MTLLoadActionClear;
			pass.clear_color = MTLClearColorMake(clear->color[0], clear->color[1], clear->color[2], clear->color[3]);
		}
		if (clear->depth && depth)
		{
			pass.depth_load = MTLLoadActionClear;
			pass.clear_depth = clear->depth_value;
		}
		if (clear->stencil && depth)
		{
			pass.stencil_load = MTLLoadActionClear;
			pass.clear_stencil = clear->stencil_value & 0xff;
		}
		return;
	}
	@autoreleasepool
	{
		struct { float color[4]; float depth; float padding[3]; } values;
		struct metal_depth_stencil state_key;
		id<MTLRenderPipelineState> pipeline;
		MTLScissorRect scissor;
		MTLViewport viewport;

		if (!encoder_begin())
			return;
		if (pass_log.record)
			pass_log.record->clears++;
		pipeline = clear_pipeline_get(color_mask);
		if (!pipeline || !scissor_inside(clear->rectangle, &scissor))
			return;
		viewport = (MTLViewport){ 0.0, 0.0, (double)pass.width, (double)pass.height, 0.0, 1.0 };
		memcpy(values.color, clear->color, sizeof(values.color));
		values.depth = clear->depth_value;
		memset(&state_key, 0, sizeof(state_key));
		state_key.depth_test = clear->depth && depth;
		state_key.depth_function = MTLCompareFunctionAlways;
		state_key.depth_write = clear->depth && depth;
		state_key.stencil_test = clear->stencil && depth;
		if (state_key.stencil_test)
		{
			state_key.stencil_function = MTLCompareFunctionAlways;
			state_key.stencil_fail = state_key.stencil_depth_fail = state_key.stencil_pass = MTLStencilOperationReplace;
			state_key.stencil_read_mask = state_key.stencil_write_mask = 0xff;
		}
		[encoder setRenderPipelineState:pipeline];
		[encoder setDepthStencilState:depth_stencil_get(&state_key)];
		[encoder setStencilReferenceValue:clear->stencil_value & 0xff];
		[encoder setCullMode:MTLCullModeNone];
		[encoder setTriangleFillMode:MTLTriangleFillModeFill];
		[encoder setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
		[encoder setViewport:viewport];
		[encoder setScissorRect:scissor];
		[encoder setVertexBytes:&values length:sizeof(values) atIndex:0];
		[encoder setFragmentBytes:&values length:sizeof(values) atIndex:0];
		if (bound.counter != ~0u)
			[encoder setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
		[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		/* (what the draws set, set again by the next: the clear's buffers
		took the uniforms' places) */
		bound_forget();
	}
}

/* ---------- draws */

#define FEEDBACK_IDLE_FRAMES 600

/* a copy of the colour target, for a draw that samples it as it draws into
it (Metal's attachments cannot be read so) */
static id<MTLTexture> feedback_copy(id<MTLTexture> target)
{
	id<MTLBlitCommandEncoder> blit;

	if (!metal.feedback || metal.feedback.width != target.width || metal.feedback.height != target.height ||
		metal.feedback.pixelFormat != target.pixelFormat)
	{
		MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:target.pixelFormat
			width:target.width height:target.height mipmapped:NO];

		descriptor.usage = MTLTextureUsageShaderRead;
		descriptor.storageMode = MTLStorageModePrivate;
		metal.feedback = [metal.device newTextureWithDescriptor:descriptor];
	}
	host_frame_acc.copies++;
	feedback_serial = frame_serial;
	if (profile_enabled)
	{
		profile_feedback_copies++;
		if (profile_phase >= 0 && profile_phase < 29)
			profile_phase_copies[profile_phase]++;
		profile_feedback_bytes += (uint64_t)target.width * target.height * 4;
	}
	/* (metal.feedback is one texture for the frame: not hoisted) */
	park_end();
	blit = blit_begin();
	[blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
		sourceSize:MTLSizeMake(target.width, target.height, 1) toTexture:metal.feedback destinationSlice:0
		destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
	[blit endEncoding];
	return metal.feedback;
}

static id<MTLTexture> dummy_texture(uint32_t type)
{
	return type == _metal_texture_cube ? metal.dummy_cube : type == _metal_texture_3d ? metal.dummy_3d :
		metal.dummy_2d;
}

/* ---------- the queued transparents at half resolution

display.half_res_transparents (the guest decides which draws:
d3d8_metal.c's half-resolution runs). A run of the queued transparents'
draws is drawn into targets at half the scene's resolution each way, and
composited into the screen once, so that what each pixel ends as is what
the draws, one after another at full resolution, would have made of it:

Every draw a run may have blends affinely in what is under it: dst' =
src * a + dst * b, where b is a factor of the source (or 1, 0) and a a
factor of the source or the destination's colour (multiply's). Then a run
of them makes dst_end = S + M * dst_start for each pixel, whatever was
under it, with S and M accumulated as the draws go, from S = 0 and M = 1:
- accumulate (S, the guest's target) blends as the draw does;
- multiply (M) blends the same source with the same destination factor
  b, and a source factor of 0, or of M itself where the draw's is the
  destination's colour (S' = src * S + S * b, M' = src * M + M * b).
The composite, at full resolution, sets dst = S + M * dst (dual-source
blending), so that a pixel no draw touched (S 0, M 1) is kept exactly.
Both are half-precision floats: the 8-bit target's rounding and clamping
between draws is not there, which differs from full resolution only where
draws saturate.

Depth: the half-resolution depth is each 2x2's nearest, so a transparent
behind any of them is hidden there; each of the 2x2's own depths is kept
(depths) for the composite's upsample, which takes the bilinear blend of
the four nearest half-resolution pixels where their depths agree with the
pixel's own (within a tenth), and else the one whose depth is nearest the
pixel's. The run's stencil test (the first-person weapon's mask) is not
drawn at half resolution but repeated by the composite at full: the weapon
is near, so its half-resolution pixels hide what is behind them already.
*/

static NSString *const halfres_shaders =
	@"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"struct halfres_vertex_out { float4 position [[position]]; };\n"
	"struct halfres_frame { int4 rect; int step; float tolerance; int2 pad; };\n"
	"vertex halfres_vertex_out halfres_vertex(uint id [[vertex_id]])\n"
	"{\n"
	"\tfloat2 p = float2((id << 1) & 2, id & 2);\n"
	"\thalfres_vertex_out o;\n"
	"\to.position = float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
	"\treturn o;\n"
	"}\n"
	/* the reduction: each half-resolution pixel's nearest depth (as
	depth, for the draws' test) and its own pixels' (1 - z, so that the
	far ones keep their precision in half floats; -1 outside the screen) */
	"struct halfres_reduce_out { float4 accumulate [[color(0)]]; float4 multiply [[color(1)]];\n"
	"\tfloat4 depths [[color(2)]]; float depth [[depth(any)]]; };\n"
	"fragment halfres_reduce_out halfres_reduce(halfres_vertex_out in [[stage_in]],\n"
	"\tdepth2d<float> screen [[texture(0)]], constant halfres_frame &f [[buffer(0)]])\n"
	"{\n"
	"\tint2 h = int2(in.position.xy);\n"
	"\tint2 size = int2(screen.get_width(), screen.get_height());\n"
	"\tfloat4 d = float4(-1.0);\n"
	"\tfloat nearest = 1.0;\n"
	"\tfor (int c = 0; c < f.step * f.step && c < 4; c++)\n"
	"\t{\n"
	"\t\tint2 q = h * f.step + int2(c % f.step, c / f.step);\n"
	"\t\tif (q.x < size.x && q.y < size.y)\n"
	"\t\t{\n"
	"\t\t\tfloat z = screen.read(uint2(q));\n"
	"\t\t\td[c] = 1.0 - z;\n"
	"\t\t\tnearest = min(nearest, z);\n"
	"\t\t}\n"
	"\t}\n"
	"\thalfres_reduce_out o;\n"
	"\to.accumulate = float4(0.0);\n"
	"\to.multiply = float4(1.0);\n"
	"\to.depths = d;\n"
	"\to.depth = nearest;\n"
	"\treturn o;\n"
	"}\n"
	/* the composite: dst = S + M * dst (source 1 is M) */
	"struct halfres_composite_out { float4 add [[color(0), index(0)]]; float4 multiply [[color(0), index(1)]]; };\n"
	"fragment halfres_composite_out halfres_composite(halfres_vertex_out in [[stage_in]],\n"
	"\ttexture2d<float> accumulate [[texture(0)]], texture2d<float> multiply [[texture(1)]],\n"
	"\ttexture2d<float> depths [[texture(2)]], constant halfres_frame &f [[buffer(0)]])\n"
	"{\n"
	"\tint2 q = int2(in.position.xy);\n"
	"\tint2 size = int2(accumulate.get_width(), accumulate.get_height());\n"
	"\thalfres_composite_out o;\n"
	"\tif (f.step == 1)\n"
	"\t{\n"
	"\t\to.add = accumulate.read(uint2(q));\n"
	"\t\to.multiply = multiply.read(uint2(q));\n"
	"\t\treturn o;\n"
	"\t}\n"
	"\tint2 parent = q / 2;\n"
	"\tint child = (q.y & 1) * 2 + (q.x & 1);\n"
	"\tfloat own = depths.read(uint2(parent))[child];\n"
	/* the four nearest half-resolution pixels and their bilinear weights
	(a pixel's centre is a quarter of a half-resolution pixel from its
	parent's: 0.25 and 0.75) */
	"\tfloat2 at = (float2(q) + 0.5) * 0.5 - 0.5;\n"
	"\tint2 base = int2(floor(at));\n"
	"\tfloat2 t = at - float2(base);\n"
	"\tfloat4 s = float4(0.0), m = float4(0.0);\n"
	"\tfloat weights[4] = { (1.0 - t.x) * (1.0 - t.y), t.x * (1.0 - t.y), (1.0 - t.x) * t.y, t.x * t.y };\n"
	"\tint2 at4[4];\n"
	"\tfloat near4[4];\n"
	"\tbool agree = true;\n"
	"\tint best = 0;\n"
	"\tfloat best_difference = 1.0e9;\n"
	"\tfor (int i = 0; i < 4; i++)\n"
	"\t{\n"
	"\t\tat4[i] = clamp(base + int2(i & 1, i >> 1), int2(0), size - 1);\n"
	"\t\tfloat4 d = depths.read(uint2(at4[i]));\n"
	"\t\tnear4[i] = max(max(d.x, d.y), max(d.z, d.w));\n"
	"\t\tfloat difference = abs(near4[i] - own);\n"
	"\t\tif (difference > f.tolerance * max(own, 1.0e-5))\n"
	"\t\t\tagree = false;\n"
	"\t\tif (difference < best_difference)\n"
	"\t\t{\n"
	"\t\t\tbest_difference = difference;\n"
	"\t\t\tbest = i;\n"
	"\t\t}\n"
	"\t}\n"
	"\tif (agree)\n"
	"\t{\n"
	"\t\tfor (int i = 0; i < 4; i++)\n"
	"\t\t{\n"
	"\t\t\ts += weights[i] * accumulate.read(uint2(at4[i]));\n"
	"\t\t\tm += weights[i] * multiply.read(uint2(at4[i]));\n"
	"\t\t}\n"
	"\t}\n"
	"\telse\n"
	"\t{\n"
	"\t\ts = accumulate.read(uint2(at4[best]));\n"
	"\t\tm = multiply.read(uint2(at4[best]));\n"
	"\t}\n"
	"\to.add = float4(s.rgb, 0.0);\n"
	"\to.multiply = float4(m.rgb, 1.0);\n"
	"\treturn o;\n"
	"}\n";

static struct
{
	BOOL tried;
	id<MTLFunction> vertex, reduce, composite;
	id<MTLRenderPipelineState> reduce_pipeline;
	/* the composite's, by the screen's colour and depth formats */
	id<MTLRenderPipelineState> composite_pipeline;
	uint32_t composite_formats[2];
	/* each pixel shader's variant writing both accumulators, by handle */
	NSMutableDictionary<NSNumber *, id<MTLFunction>> *fragments;
	struct cache pipelines;
} halfres_state;

static BOOL halfres_prepare(void)
{
	NSError *error = nil;
	id<MTLLibrary> library;

	if (halfres_state.tried)
		return halfres_state.composite != nil;
	halfres_state.tried = YES;
	library = [metal.device newLibraryWithSource:halfres_shaders options:compile_options() error:&error];
	if (!library)
	{
		host_log("Metal: half resolution: its shaders do not build: %s", error.localizedDescription.UTF8String);
		return NO;
	}
	halfres_state.vertex = [library newFunctionWithName:@"halfres_vertex"];
	halfres_state.reduce = [library newFunctionWithName:@"halfres_reduce"];
	halfres_state.composite = [library newFunctionWithName:@"halfres_composite"];
	halfres_state.fragments = [NSMutableDictionary dictionary];
	return halfres_state.composite != nil;
}

/* a pixel shader's text, with its result written to the run's two
accumulators (and nothing to the depths): the entry point returns a struct
of both, each return in it giving the same colour to both */
static NSString *halfres_fragment_text(NSString *source)
{
	NSRange entry = [source rangeOfString:@"fragment vec4 fragment_main("];
	NSMutableString *text;
	NSUInteger at;

	if (entry.location == NSNotFound)
		return nil;
	text = [NSMutableString stringWithString:[source substringToIndex:entry.location]];
	[text appendString:@"struct halfres_out { vec4 accumulate [[color(0)]]; vec4 multiply [[color(1)]]; };\n"
		"static inline halfres_out halfres_both(vec4 c) { halfres_out o; o.accumulate = c; o.multiply = c; "
		"return o; }\n"
		"fragment halfres_out fragment_main("];
	at = entry.location + entry.length;
	while (at < source.length)
	{
		NSRange found = [source rangeOfString:@"return " options:0 range:NSMakeRange(at, source.length - at)];
		NSRange end;

		if (found.location == NSNotFound)
			break;
		end = [source rangeOfString:@";" options:0
			range:NSMakeRange(found.location, source.length - found.location)];
		if (end.location == NSNotFound)
			return nil;
		[text appendString:[source substringWithRange:NSMakeRange(at, found.location - at)]];
		[text appendString:@"return halfres_both("];
		[text appendString:[source substringWithRange:NSMakeRange(found.location + found.length,
			end.location - found.location - found.length)]];
		[text appendString:@")"];
		at = end.location;
	}
	[text appendString:[source substringFromIndex:at]];
	return text;
}

static id<MTLFunction> halfres_fragment(uint32_t handle)
{
	id<MTLFunction> function = halfres_state.fragments[@(handle)];
	NSString *source, *text;
	NSError *error = nil;
	id<MTLLibrary> library;
	uint64_t frame_log_started;

	if (function)
		return function;
	source = halfres_sources[@(handle)];
	text = source ? halfres_fragment_text(source) : nil;
	if (!text)
		return nil;
	text = shader_text(text.UTF8String, 1);
	frame_log_started = host_frame_log_detail() ? host_frame_log_now() : 0;
	library = [metal.device newLibraryWithSource:text options:compile_options() error:&error];
	if (frame_log_started)
	{
		host_frame_acc.compile_ns += host_frame_log_now() - frame_log_started;
		host_frame_acc.compiles++;
	}
	function = library ? [library newFunctionWithName:shader_entry_point(1)] : nil;
	if (!function)
	{
		host_log("Metal: half resolution: pixel shader %u's variant does not build: %s", handle,
			error ? error.localizedDescription.UTF8String : "no entry point");
		return nil;
	}
	halfres_state.fragments[@(handle)] = function;
	return function;
}

/* the multiply's source factor for a draw's (half resolution's comment) */
static metal_u32 halfres_multiply_source(metal_u32 source)
{
	return source == METAL_BLEND_DESTINATION_COLOR ? METAL_BLEND_DESTINATION_COLOR : METAL_BLEND_ZERO;
}

static id<MTLRenderPipelineState> halfres_pipeline_get(const struct metal_draw *draw)
{
	struct pipeline_key key;
	uint64_t hash;
	BOOL found;
	void *object;
	MTLRenderPipelineDescriptor *descriptor;
	id<MTLRenderPipelineState> state = nil;
	id<MTLFunction> fragment;
	NSError *error = nil;

	memset(&key, 0, sizeof(key));
	key.vertex_shader = draw->vertex_shader;
	key.fragment_shader = draw->fragment_shader;
	key.vertex_layout = draw->vertex_layout;
	key.blend = draw->blend;
	object = cache_find(&halfres_state.pipelines, &key, sizeof(key), &hash, &found);
	if (found)
		return (__bridge id<MTLRenderPipelineState>)object;
	fragment = halfres_fragment(draw->fragment_shader);
	if (fragment)
	{
		descriptor = pipeline_descriptor_make(handle_object(draw->vertex_shader, _handle_shader), fragment,
			handle_object(draw->vertex_layout, _handle_layout), METAL_FORMAT_RGBA16F, MTLPixelFormatDepth32Float,
			&draw->blend);
		descriptor.stencilAttachmentPixelFormat = MTLPixelFormatInvalid;
		descriptor.colorAttachments[0].writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
		descriptor.colorAttachments[1].pixelFormat = MTLPixelFormatRGBA16Float;
		descriptor.colorAttachments[1].writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
		descriptor.colorAttachments[1].blendingEnabled = YES;
		descriptor.colorAttachments[1].sourceRGBBlendFactor = (MTLBlendFactor)halfres_multiply_source(draw->blend.source);
		descriptor.colorAttachments[1].sourceAlphaBlendFactor = descriptor.colorAttachments[1].sourceRGBBlendFactor;
		descriptor.colorAttachments[1].destinationRGBBlendFactor = (MTLBlendFactor)draw->blend.destination;
		descriptor.colorAttachments[1].destinationAlphaBlendFactor = (MTLBlendFactor)draw->blend.destination;
		descriptor.colorAttachments[1].rgbBlendOperation = MTLBlendOperationAdd;
		descriptor.colorAttachments[1].alphaBlendOperation = MTLBlendOperationAdd;
		descriptor.colorAttachments[2].pixelFormat = MTLPixelFormatRGBA16Float;
		descriptor.colorAttachments[2].writeMask = MTLColorWriteMaskNone;
		state = [metal.device newRenderPipelineStateWithDescriptor:descriptor error:&error];
		if (!state)
			host_log("Metal: half resolution: no pipeline for shaders %u/%u: %s", draw->vertex_shader,
				draw->fragment_shader, error.localizedDescription.UTF8String);
	}
	cache_add(&halfres_state.pipelines, &key, sizeof(key), hash, state);
	return state;
}

static void halfres_frame_values(uint32_t values[8])
{
	float tolerance = 0.1f;

	values[0] = halfres.x;
	values[1] = halfres.y;
	values[2] = halfres.width;
	values[3] = halfres.height;
	values[4] = halfres.step;
	memcpy(&values[5], &tolerance, sizeof(tolerance));
	values[6] = values[7] = 0;
}

void host_metal_halfres_begin(unsigned int screen_color, unsigned int screen_depth, unsigned int accumulate,
	unsigned int depth, unsigned int x, unsigned int y, unsigned int width, unsigned int height, unsigned int step)
{
	HOST_FRAME_LOG_SCOPE;
	id<MTLTexture> target = texture_object(accumulate), screen = texture_object(screen_depth);
	MTLRenderPassDescriptor *descriptor;

	if (!metal.device || halfres.active || !target || !screen || !halfres_prepare())
		return;
	@autoreleasepool
	{
		/* the screen's pass ended (its depth stored, for the reduction) */
		pass_flush();
		park_end();
		pass_log.end_reason = _pass_end_targets;
		encoder_end();
		frame_begin();
		if (!halfres.multiply || halfres.multiply.width != target.width || halfres.multiply.height != target.height)
		{
			MTLTextureDescriptor *made = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
				MTLPixelFormatRGBA16Float width:target.width height:target.height mipmapped:NO];

			made.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
			made.storageMode = MTLStorageModePrivate;
			halfres.multiply = [metal.device newTextureWithDescriptor:made];
			halfres.depths = [metal.device newTextureWithDescriptor:made];
		}
		if (!halfres_state.reduce_pipeline)
		{
			MTLRenderPipelineDescriptor *pipeline = [MTLRenderPipelineDescriptor new];
			NSError *error = nil;

			pipeline.vertexFunction = halfres_state.vertex;
			pipeline.fragmentFunction = halfres_state.reduce;
			pipeline.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
			pipeline.colorAttachments[1].pixelFormat = MTLPixelFormatRGBA16Float;
			pipeline.colorAttachments[2].pixelFormat = MTLPixelFormatRGBA16Float;
			pipeline.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
			halfres_state.reduce_pipeline = [metal.device newRenderPipelineStateWithDescriptor:pipeline error:&error];
			if (!halfres_state.reduce_pipeline)
			{
				host_log("Metal: half resolution: no reduction pipeline: %s", error.localizedDescription.UTF8String);
				return;
			}
		}
		halfres.active = YES;
		halfres.accumulate = accumulate;
		halfres.depth = depth;
		halfres.screen_color = screen_color;
		halfres.screen_depth = screen_depth;
		halfres.x = x;
		halfres.y = y;
		halfres.width = width;
		halfres.height = height;
		halfres.step = step ? step : 2;
		halfres.stencil_set = 0;
		halfres.reduced = NO;
		halfres.runs++;
		/* the targets the guest's draws now name: theirs, cleared by the
		reduction (whole: the window's rectangle is the composite's) */
		pass.color = accumulate;
		pass.depth = depth;
		pass.color_load = pass.depth_load = pass.stencil_load = MTLLoadActionDontCare;
		handles[accumulate].fresh = handles[depth].fresh = 0;
		if (!encoder_begin())
		{
			halfres.active = NO;
			return;
		}
		{
			uint32_t values[8];
			struct metal_depth_stencil state_key;

			memset(&state_key, 0, sizeof(state_key));
			state_key.depth_test = 1;
			state_key.depth_function = MTLCompareFunctionAlways;
			state_key.depth_write = 1;
			halfres_frame_values(values);
			[encoder setRenderPipelineState:halfres_state.reduce_pipeline];
			[encoder setDepthStencilState:depth_stencil_get(&state_key)];
			[encoder setCullMode:MTLCullModeNone];
			[encoder setTriangleFillMode:MTLTriangleFillModeFill];
			[encoder setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
			[encoder setViewport:(MTLViewport){ 0.0, 0.0, (double)pass.width, (double)pass.height, 0.0, 1.0 }];
			[encoder setScissorRect:(MTLScissorRect){ 0, 0, pass.width, pass.height }];
			[encoder setFragmentTexture:screen atIndex:0];
			[encoder setFragmentBytes:values length:sizeof(values) atIndex:0];
			if (bound.counter != ~0u)
				[encoder setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
			[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			bound_forget();
			handles[screen_depth].commands_read = commands_generation;
		}
		(void)descriptor;
	}
}

/* a draw of a run: its depth's and stencil's tests as the half-resolution
targets have them (the stencil's repeated by the composite) */
static const struct metal_draw *halfres_draw(const struct metal_draw *draw, struct metal_draw *adjusted)
{
	if (!halfres.active || pass.color != halfres.accumulate)
		return draw;
	*adjusted = *draw;
	if (draw->depth_stencil.stencil_test && !halfres.stencil_set)
	{
		halfres.stencil = draw->depth_stencil;
		halfres.stencil_reference = draw->stencil_reference;
		halfres.stencil_set = 1;
	}
	adjusted->depth_stencil.stencil_test = 0;
	adjusted->depth_stencil.stencil_function = adjusted->depth_stencil.stencil_fail = 0;
	adjusted->depth_stencil.stencil_depth_fail = adjusted->depth_stencil.stencil_pass = 0;
	adjusted->depth_stencil.stencil_read_mask = adjusted->depth_stencil.stencil_write_mask = 0;
	adjusted->stencil_reference = 0;
	adjusted->depth_stencil.depth_write = 0;
	adjusted->blend.write_mask = METAL_WRITE_RED | METAL_WRITE_GREEN | METAL_WRITE_BLUE;
	halfres.draws++;
	return adjusted;
}

void host_metal_halfres_end(void)
{
	HOST_FRAME_LOG_SCOPE;
	id<MTLTexture> screen_color, screen_depth;

	if (!halfres.active)
		return;
	@autoreleasepool
	{
		/* (the run's pass, parked by a small one between its draws, ended
		too) */
		park_end();
		pass_log.end_reason = _pass_end_targets;
		encoder_end();
		halfres.active = NO;
		pass.color = halfres.screen_color;
		pass.depth = halfres.screen_depth;
		pass.color_load = pass.depth_load = pass.stencil_load = MTLLoadActionLoad;
		screen_color = texture_object(pass.color);
		screen_depth = texture_object(pass.depth);
		if (!screen_color || !encoder_begin())
			return;
		if (!halfres_state.composite_pipeline || halfres_state.composite_formats[0] != (uint32_t)pass.color_format ||
			halfres_state.composite_formats[1] != (uint32_t)pass.depth_format)
		{
			MTLRenderPipelineDescriptor *pipeline = [MTLRenderPipelineDescriptor new];
			NSError *error = nil;

			pipeline.vertexFunction = halfres_state.vertex;
			pipeline.fragmentFunction = halfres_state.composite;
			pipeline.colorAttachments[0].pixelFormat = pass.color_format;
			pipeline.colorAttachments[0].writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskGreen |
				MTLColorWriteMaskBlue;
			pipeline.colorAttachments[0].blendingEnabled = YES;
			pipeline.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
			pipeline.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorSource1Color;
			pipeline.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorZero;
			pipeline.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
			pipeline.depthAttachmentPixelFormat = pass.depth_format;
			pipeline.stencilAttachmentPixelFormat = pass.depth_format;
			halfres_state.composite_pipeline = [metal.device newRenderPipelineStateWithDescriptor:pipeline error:&error];
			halfres_state.composite_formats[0] = (uint32_t)pass.color_format;
			halfres_state.composite_formats[1] = (uint32_t)pass.depth_format;
			if (!halfres_state.composite_pipeline)
			{
				host_log("Metal: half resolution: no composite pipeline: %s", error.localizedDescription.UTF8String);
				return;
			}
		}
		{
			uint32_t values[8];
			struct metal_depth_stencil state_key;
			MTLScissorRect scissor;
			uint32_t x = halfres.x * halfres.step, y = halfres.y * halfres.step;
			uint32_t right = (halfres.x + halfres.width) * halfres.step, bottom = (halfres.y + halfres.height) * halfres.step;

			if (right > pass.width)
				right = pass.width;
			if (bottom > pass.height)
				bottom = pass.height;
			if (x >= right || y >= bottom)
				return;
			scissor = (MTLScissorRect){ x, y, right - x, bottom - y };
			memset(&state_key, 0, sizeof(state_key));
			if (halfres.stencil_set && screen_depth)
			{
				state_key.stencil_test = 1;
				state_key.stencil_function = halfres.stencil.stencil_function;
				state_key.stencil_read_mask = halfres.stencil.stencil_read_mask;
			}
			halfres_frame_values(values);
			[encoder setRenderPipelineState:halfres_state.composite_pipeline];
			[encoder setDepthStencilState:depth_stencil_get(&state_key)];
			[encoder setStencilReferenceValue:halfres.stencil_reference & 0xff];
			[encoder setCullMode:MTLCullModeNone];
			[encoder setTriangleFillMode:MTLTriangleFillModeFill];
			[encoder setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
			[encoder setViewport:(MTLViewport){ 0.0, 0.0, (double)pass.width, (double)pass.height, 0.0, 1.0 }];
			[encoder setScissorRect:scissor];
			[encoder setFragmentTexture:texture_object(halfres.accumulate) atIndex:0];
			[encoder setFragmentTexture:halfres.multiply atIndex:1];
			[encoder setFragmentTexture:halfres.depths atIndex:2];
			[encoder setFragmentBytes:values length:sizeof(values) atIndex:0];
			if (bound.counter != ~0u)
				[encoder setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
			[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			bound_forget();
			handles[halfres.accumulate].commands_read = commands_generation;
		}
	}
}

/* ---------- the queued transparents' kinds (diagnostic)

HALO_METAL_TRANSPARENT_SURVEY=1: the queued transparents' draws (phase 22)
counted by what drawing them at half resolution needs to know: their blend
(the destination factor's dependence on what is under them, the source's),
whether they write depth, test stencil, sample a screen-sized render target
(the screen, its copy, the secondary target), and the vertex count; written
at exit. HALO_METAL_SKIP_TRANSPARENT_CLASSES=a,b,... (diagnostic, an upper
bound on what each costs): the draws of those classes not drawn. A class:
0 affine in the destination (alpha, add, multiply, double multiply,
premultiplied: drawable at half resolution into a pair of accumulators),
1 subtract / min / max, 2 a constant (meter) or destination-alpha factor,
3 writes depth or colour alpha, 4 tests stencil, 5 samples a screen-sized
target, 6 blending off. */

enum
{
	_transparent_affine,
	_transparent_not_affine,
	_transparent_constant_or_destination_alpha,
	_transparent_writes_depth_or_alpha,
	_transparent_stencil,
	_transparent_samples_screen,
	_transparent_opaque,
	NUMBER_OF_TRANSPARENT_CLASSES
};

static struct
{
	int enabled;
	uint32_t skip;
	uint64_t draws[NUMBER_OF_TRANSPARENT_CLASSES], vertices[NUMBER_OF_TRANSPARENT_CLASSES];
	uint64_t frames_first, frames_last;
	/* the stencil tests (function, reference, read mask, depth test and
	function) and blends (source, destination, operation) seen, and their draws */
	uint32_t stencil_keys[8], blend_keys[16];
	uint64_t stencil_draws[8], blend_draws[16];
} transparent_survey;

static const char *const transparent_class_names[NUMBER_OF_TRANSPARENT_CLASSES] =
{
	"affine", "subtract/min/max", "constant/destination-alpha", "writes depth/alpha", "stencil", "samples screen",
	"blend off",
};

static void transparent_survey_report(void)
{
	int index;
	uint64_t frames = transparent_survey.frames_last - transparent_survey.frames_first + 1;

	for (index = 0; index < NUMBER_OF_TRANSPARENT_CLASSES; index++)
	{
		host_log("transparent survey: %s: %.1f draws %.0f vertices a frame (%llu frames)",
			transparent_class_names[index], (double)transparent_survey.draws[index] / (double)frames,
			(double)transparent_survey.vertices[index] / (double)frames, (unsigned long long)frames);
	}
	for (index = 0; index < 8 && transparent_survey.stencil_keys[index]; index++)
	{
		uint32_t key = transparent_survey.stencil_keys[index];

		host_log("transparent survey: stencil function %u reference %u read mask %02x depth test %u function %u: "
			"%.1f draws a frame", key & 15, (key >> 4) & 255, (key >> 12) & 255, (key >> 20) & 1, (key >> 21) & 15,
			(double)transparent_survey.stencil_draws[index] / (double)frames);
	}
	for (index = 0; index < 16 && transparent_survey.blend_keys[index]; index++)
	{
		uint32_t key = transparent_survey.blend_keys[index] - 1;

		host_log("transparent survey: blend source %u destination %u operation %u: %.1f draws a frame", key & 255,
			(key >> 8) & 255, (key >> 16) & 255, (double)transparent_survey.blend_draws[index] / (double)frames);
	}
}

static int transparent_class(const struct metal_draw *draw)
{
	int stage;
	metal_u32 source = draw->blend.source, destination = draw->blend.destination;

	for (stage = 0; stage < METAL_STAGES; stage++)
	{
		id<MTLTexture> texture = texture_object(draw->textures[stage]);

		if (texture && (texture.usage & MTLTextureUsageRenderTarget) && texture.width * texture.height >= PARK_LARGE_PIXELS / 4)
			return _transparent_samples_screen;
	}
	if (draw->depth_stencil.depth_write || (draw->blend.write_mask & METAL_WRITE_ALPHA))
		return _transparent_writes_depth_or_alpha;
	/* (a stencil test that only reads: the first-person weapon's mask, which
	the queued transparents keep (rasterizer_set_stencil_mode's REJECT and
	ACCEPT), and a downsampled stencil carries; one that writes is apart) */
	if (draw->depth_stencil.stencil_test && draw->depth_stencil.stencil_write_mask &&
		(draw->depth_stencil.stencil_pass || draw->depth_stencil.stencil_fail || draw->depth_stencil.stencil_depth_fail))
	{
		return _transparent_stencil;
	}
	if (!draw->blend.enabled)
		return _transparent_opaque;
	if (draw->blend.operation != METAL_BLEND_OPERATION_ADD)
		return _transparent_not_affine;
	if (source >= METAL_BLEND_DESTINATION_ALPHA || destination >= METAL_BLEND_DESTINATION_COLOR ||
		source == METAL_BLEND_ONE_MINUS_DESTINATION_COLOR)
	{
		return _transparent_constant_or_destination_alpha;
	}
	return _transparent_affine;
}

/* NO: the draw is not drawn (a class HALO_METAL_SKIP_TRANSPARENT_CLASSES names) */
static BOOL transparent_survey_draw(const struct metal_draw *draw)
{
	static int read;
	int kind;

	if (!read)
	{
		const char *setting = getenv("HALO_METAL_SKIP_TRANSPARENT_CLASSES");

		read = 1;
		transparent_survey.enabled = getenv("HALO_METAL_TRANSPARENT_SURVEY") &&
			atoi(getenv("HALO_METAL_TRANSPARENT_SURVEY")) != 0;
		while (setting && *setting)
		{
			int value = atoi(setting);

			if (value >= 0 && value < NUMBER_OF_TRANSPARENT_CLASSES)
				transparent_survey.skip |= 1u << value;
			setting = strchr(setting, ',');
			if (setting)
				setting++;
		}
		if (transparent_survey.enabled)
			atexit(transparent_survey_report);
		if (transparent_survey.enabled || transparent_survey.skip)
			host_log("Metal: transparent survey %d, skipped classes %02x (diagnostic)", transparent_survey.enabled,
				transparent_survey.skip);
	}
	if (profile_phase != 22 || (!transparent_survey.enabled && !transparent_survey.skip))
		return YES;
	kind = transparent_class(draw);
	if (transparent_survey.enabled && frame_serial >= 300)
	{
		if (!transparent_survey.frames_first)
			transparent_survey.frames_first = frame_serial;
		transparent_survey.frames_last = frame_serial;
		transparent_survey.draws[kind]++;
		transparent_survey.vertices[kind] += draw->index_count ? draw->index_count : draw->vertex_count;
		if (draw->depth_stencil.stencil_test)
		{
			int slot;
			uint32_t key = draw->depth_stencil.stencil_function | draw->stencil_reference << 4 |
				draw->depth_stencil.stencil_read_mask << 12 | (uint32_t)draw->depth_stencil.depth_test << 20 |
				draw->depth_stencil.depth_function << 21;

			for (slot = 0; slot < 8 && transparent_survey.stencil_keys[slot] &&
				transparent_survey.stencil_keys[slot] != key; slot++)
			{
			}
			if (slot < 8)
			{
				transparent_survey.stencil_keys[slot] = key;
				transparent_survey.stencil_draws[slot]++;
			}
		}
		if (draw->blend.enabled)
		{
			uint32_t triple = draw->blend.source | draw->blend.destination << 8 | draw->blend.operation << 16;
			int slot;

			for (slot = 0; slot < 16 && transparent_survey.blend_keys[slot] &&
				transparent_survey.blend_keys[slot] != triple + 1; slot++)
			{
			}
			if (slot < 16)
			{
				transparent_survey.blend_keys[slot] = triple + 1;
				transparent_survey.blend_draws[slot]++;
			}
		}
	}
	return !(transparent_survey.skip >> kind & 1);
}

void host_metal_draw(const struct metal_draw *original)
{
	HOST_FRAME_LOG_SCOPE;
	struct metal_draw adjusted, halfres_adjusted;
	const struct metal_draw *draw = original;

	/* (_rasterizer_profile_environment_shadows, _environment_fog,
	_queued_transparents: tools/test_metal_profile.py checks the numbers) */
	host_frame_log_count_draw(profile_phase);
	if (!metal.device || !draw->vertex_shader || !draw->fragment_shader || !draw->vertex_layout)
		return;
	/* HALO_METAL_ONLY: only the draws of these pixel shaders (handles,
	comma-separated), to look at what they draw by themselves */
	{
		static const char *only = (const char *)1;

		if (only == (const char *)1)
			only = getenv("HALO_METAL_ONLY");
		if (only)
		{
			const char *at = only;
			BOOL listed = NO;

			while (at && *at && !listed)
			{
				listed = (unsigned int)atoi(at) == draw->fragment_shader;
				at = strchr(at, ',');
				if (at)
					at++;
			}
			if (!listed)
				return;
		}
	}
	/* HALO_METAL_SKIP_PHASES (diagnostic): the draws of these phases not drawn */
	if (pass_log.skip_phases && profile_phase >= 0 && (pass_log.skip_phases >> profile_phase & 1))
		return;
	if (!transparent_survey_draw(draw))
		return;
	draw = halfres_draw(draw, &halfres_adjusted);
	if (metal.debug & 3)
	{
		adjusted = *draw;
		if (metal.debug & 1)
			adjusted.cull = METAL_CULL_NONE;
		if (metal.debug & 2)
			memset(&adjusted.depth_stencil, 0, sizeof(adjusted.depth_stencil));
		draw = &adjusted;
	}
	/* (a texture the screen's parked pass has drawn: not hoisted) */
	if (park.active)
	{
		int stage;

		for (stage = 0; stage < METAL_STAGES; stage++)
		{
			if (draw->textures[stage] && draw->textures[stage] < HANDLE_COUNT &&
				handles[draw->textures[stage]].commands_written == park.generation)
			{
				@autoreleasepool
				{
					park_end();
				}
				break;
			}
		}
	}
	@autoreleasepool
	{
		id<MTLRenderPipelineState> pipeline;
		id<MTLTexture> textures[METAL_STAGES];
		MTLScissorRect scissor;
		uint32_t index;
		id<MTLTexture> feedback_snapshot = nil;
		BOOL feedback_taken = NO;

		if (profile_enabled && profile_phase >= 0 && profile_phase < 29)
			profile_phase_draws[profile_phase]++;
		/* the textures first: a copy of one that is the target ends the pass.
		   All matching stages read the SAME pre-draw target: snapshot once
		   for this draw, never cache its contents across draws. */
		for (index = 0; index < METAL_STAGES; index++)
		{
			textures[index] = texture_object(draw->textures[index]);
			if (textures[index] && draw->textures[index] == pass.color && pass.color)
			{
				if (!feedback_taken)
				{
					feedback_snapshot = feedback_copy(textures[index]);
					feedback_taken = YES;
				}
				textures[index] = feedback_snapshot;
			}
			if (!textures[index] || (textures[index].textureType == MTLTextureTypeCube) !=
				(draw->texture_types[index] == _metal_texture_cube) || (textures[index].textureType ==
				MTLTextureType3D) != (draw->texture_types[index] == _metal_texture_3d))
			{
				textures[index] = dummy_texture(draw->texture_types[index]);
			}
		}
		/* HALO_METAL_PASS_SPLIT=phase (diagnostic): a pass a phase */
		if (pass_log.split_phases && encoder && pass_log.record && pass_log.record->draws &&
			pass_log.record->last_phase != profile_phase)
		{
			pass_log.end_reason = _pass_end_phase;
			encoder_end();
		}
		if (!encoder_begin())
			return;
		pipeline = pipeline_get(draw);
		if (!pipeline || !scissor_inside(draw->scissor, &scissor))
			return;
		if (pass_log.record)
		{
			struct host_pass_record *record = pass_log.record;

			record->draws++;
			record->fog_draws += profile_phase == 17;
			record->transparent_draws += profile_phase == 22;
			record->target_samples += feedback_taken;
			if (profile_phase >= 0)
			{
				if (record->first_phase < 0)
					record->first_phase = (int8_t)profile_phase;
				record->phase_mask |= 1u << profile_phase;
			}
			record->last_phase = (int8_t)profile_phase;
		}

		if (bound.pipeline != (__bridge void *)pipeline)
		{
			[encoder setRenderPipelineState:pipeline];
			bound.pipeline = (__bridge void *)pipeline;
		}
		{
			id<MTLDepthStencilState> state = depth_stencil_get(&draw->depth_stencil);

			if (bound.depth_stencil != (__bridge void *)state)
			{
				[encoder setDepthStencilState:state];
				bound.depth_stencil = (__bridge void *)state;
			}
		}
		if (bound.stencil_reference != draw->stencil_reference)
		{
			[encoder setStencilReferenceValue:draw->stencil_reference & 0xff];
			bound.stencil_reference = draw->stencil_reference;
		}
		if (bound.cull != draw->cull)
		{
			[encoder setCullMode:(MTLCullMode)draw->cull];
			bound.cull = draw->cull;
		}
		if (bound.winding != draw->winding)
		{
			[encoder setFrontFacingWinding:(MTLWinding)draw->winding];
			bound.winding = draw->winding;
		}
		if (bound.wireframe != draw->wireframe)
		{
			[encoder setTriangleFillMode:draw->wireframe ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
			bound.wireframe = draw->wireframe;
		}
		if (memcmp(&bound.depth_bias, &draw->depth_bias, sizeof(float)) ||
			memcmp(&bound.depth_slope, &draw->depth_slope, sizeof(float)))
		{
			[encoder setDepthBias:draw->depth_bias slopeScale:draw->depth_slope clamp:0.0f];
			bound.depth_bias = draw->depth_bias;
			bound.depth_slope = draw->depth_slope;
		}
		if (draw->blend.enabled && memcmp(bound.blend_color, draw->blend_color, sizeof(bound.blend_color)))
		{
			[encoder setBlendColorRed:draw->blend_color[0] green:draw->blend_color[1] blue:draw->blend_color[2]
				alpha:draw->blend_color[3]];
			memcpy(bound.blend_color, draw->blend_color, sizeof(bound.blend_color));
		}
		if (memcmp(bound.viewport, draw->viewport, sizeof(bound.viewport)))
		{
			MTLViewport viewport = { draw->viewport[0], draw->viewport[1], draw->viewport[2], draw->viewport[3],
				draw->viewport[4], draw->viewport[5] };

			[encoder setViewport:viewport];
			memcpy(bound.viewport, draw->viewport, sizeof(bound.viewport));
		}
		if (bound.scissor[0] != scissor.x || bound.scissor[1] != scissor.y || bound.scissor[2] != scissor.width ||
			bound.scissor[3] != scissor.height)
		{
			[encoder setScissorRect:scissor];
			bound.scissor[0] = (uint32_t)scissor.x;
			bound.scissor[1] = (uint32_t)scissor.y;
			bound.scissor[2] = (uint32_t)scissor.width;
			bound.scissor[3] = (uint32_t)scissor.height;
		}

		/* the uniforms, when they have changed */
		if (bound.vertex_uniforms_serial != draw->vertex_uniforms_serial)
		{
			[encoder setVertexBytes:HOST_POINTER(draw->vertex_uniforms) length:METAL_VERTEX_UNIFORMS_BYTES
				atIndex:METAL_BUFFER_VERTEX_UNIFORMS];
			bound.vertex_uniforms_serial = draw->vertex_uniforms_serial;
		}
		if (bound.vertex_constants_serial != draw->vertex_constants_serial)
		{
			[encoder setVertexBytes:HOST_POINTER(draw->vertex_constants) length:METAL_VERTEX_CONSTANTS_BYTES
				atIndex:METAL_BUFFER_VERTEX_CONSTANTS];
			bound.vertex_constants_serial = draw->vertex_constants_serial;
		}
		if (bound.constant_attributes_serial != draw->constant_attributes_serial)
		{
			[encoder setVertexBytes:HOST_POINTER(draw->constant_attributes) length:METAL_CONSTANT_ATTRIBUTES_BYTES
				atIndex:METAL_BUFFER_CONSTANT_ATTRIBUTES];
			bound.constant_attributes_serial = draw->constant_attributes_serial;
		}
		if (bound.pixel_uniforms_serial != draw->pixel_uniforms_serial)
		{
			[encoder setFragmentBytes:HOST_POINTER(draw->pixel_uniforms) length:METAL_PIXEL_UNIFORMS_BYTES
				atIndex:METAL_BUFFER_PIXEL_UNIFORMS];
			bound.pixel_uniforms_serial = draw->pixel_uniforms_serial;
		}

		for (index = 0; index < METAL_STAGES; index++)
		{
			id<MTLSamplerState> sampler = sampler_get(&draw->samplers[index]);

			if (bound.textures[index] != (__bridge void *)textures[index])
			{
				[encoder setFragmentTexture:textures[index] atIndex:index];
				bound.textures[index] = (__bridge void *)textures[index];
			}
			if (bound.samplers[index] != (__bridge void *)sampler)
			{
				[encoder setFragmentSamplerState:sampler atIndex:index];
				bound.samplers[index] = (__bridge void *)sampler;
			}
			if (draw->textures[index] && draw->textures[index] < HANDLE_COUNT)
			{
				handles[draw->textures[index]].used_serial = frame_serial;
				handles[draw->textures[index]].commands_read = commands_generation;
			}
		}

		/* the vertices, copied */
		for (index = 0; index < METAL_VERTEX_BUFFERS - 1; index++)
		{
			const struct metal_stream *stream = &draw->streams[index];
			id<MTLBuffer> buffer;
			uint32_t offset;
			void *copy;

			if (!stream->data || !stream->bytes)
				continue;
			copy = ring_allocate(stream->bytes, 16, &buffer, &offset);
			memcpy(copy, HOST_POINTER(stream->data), stream->bytes);
			[encoder setVertexBuffer:buffer offset:offset atIndex:index];
		}

		/* the visibility test it counts for: its counter in this pass */
		{
			struct frame *frame = frame_current();
			uint32_t counter = ~0u, test = draw->visibility_test;

			if (test < frame->test_count)
			{
				if (test_counter_encoder[test] == encoder_serial)
				{
					counter = test_counter[test];
				}
				else if (frame->counter_count < VISIBILITY_COUNTERS)
				{
					counter = frame->counter_count++;
					frame->counter_test[counter] = (uint16_t)test;
					test_counter[test] = counter;
					test_counter_encoder[test] = encoder_serial;
				}
			}
			if (bound.counter != counter)
			{
				if (counter != ~0u)
					[encoder setVisibilityResultMode:MTLVisibilityResultModeCounting offset:counter * 8];
				else
					[encoder setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
				bound.counter = counter;
			}
		}

		/* HALO_METAL_TRACE: the first draws' data, logged */
		{
			static int traced = -1;

			if (traced < 0)
				traced = getenv("HALO_METAL_TRACE") ? atoi(getenv("HALO_METAL_TRACE")) : 0;
			if (traced > 0 && draw->indices)
			{
				const uint16_t *source = HOST_POINTER(draw->indices);
				MTLVertexDescriptor *layout = handle_object(draw->vertex_layout, _handle_layout);

				traced--;
				host_log("trace: primitive %u indices %u base %u: %u %u %u %u %u %u; stream 0 %u bytes at %08x stride %lu",
					draw->primitive, draw->index_count, draw->base_vertex, source[0], source[1], source[2], source[3],
					source[4], source[5], draw->streams[0].bytes, draw->streams[0].data,
					(unsigned long)layout.layouts[0].stride);
				if (draw->streams[0].data)
				{
					const float *vertex = HOST_POINTER(draw->streams[0].data);

					host_log("trace: vertex 0 %g %g %g; attribute 0 format %lu buffer %lu offset %lu",
						vertex[0], vertex[1], vertex[2], (unsigned long)layout.attributes[0].format,
						(unsigned long)layout.attributes[0].bufferIndex, (unsigned long)layout.attributes[0].offset);
				}
				host_log("trace: viewport %g %g %g %g %g %g scissor %lu %lu %lu %lu pass %ux%u",
					draw->viewport[0], draw->viewport[1], draw->viewport[2], draw->viewport[3], draw->viewport[4],
					draw->viewport[5], (unsigned long)scissor.x, (unsigned long)scissor.y, (unsigned long)scissor.width,
					(unsigned long)scissor.height, pass.width, pass.height);
			}
		}
		if (draw->indices && draw->index_count)
		{
			const uint16_t *source = HOST_POINTER(draw->indices);
			id<MTLBuffer> buffer;
			uint32_t offset, count = draw->index_count;
			uint16_t *indices = ring_allocate(count * 2, 16, &buffer, &offset);
			uint16_t base = (uint16_t)draw->base_vertex;

			/* (from the streams' first vertex, the draw's base) */
			if (base)
			{
				for (index = 0; index < count; index++)
					indices[index] = (uint16_t)(source[index] - base);
			}
			else
			{
				memcpy(indices, source, count * 2);
			}
			[encoder drawIndexedPrimitives:(MTLPrimitiveType)draw->primitive indexCount:count
				indexType:MTLIndexTypeUInt16 indexBuffer:buffer indexBufferOffset:offset];
		}
		else if (draw->vertex_count)
		{
			[encoder drawPrimitives:(MTLPrimitiveType)draw->primitive vertexStart:0 vertexCount:draw->vertex_count];
		}
	}
}

/* ---------- visibility tests */

/* a test of the frame's: its number, or ~0 with the frame's all taken */
unsigned int host_metal_visibility_begin(void)
{
	struct frame *frame;
	uint32_t test;

	if (!metal.device)
		return ~0u;
	frame_begin();
	frame = frame_current();
	if (frame->test_count >= VISIBILITY_TESTS)
		return ~0u;
	test = frame->test_count++;
	frame->test_ended[test] = 0;
	test_counter_encoder[test] = 0;
	return test;
}

/* the test's count goes to the game's index of it */
void host_metal_visibility_end(unsigned int test, unsigned int index)
{
	struct frame *frame = frame_current();

	if (!commands || test >= frame->test_count)
		return;
	frame->test_index[test] = (uint16_t)(index % METAL_VISIBILITY_SLOTS);
	frame->test_ended[test] = 1;
}

/* the latest count of an index's tests that the GPU has finished; 0 if
none has */
int host_metal_visibility_result(unsigned int index, unsigned int *count)
{
	if (index >= METAL_VISIBILITY_SLOTS || !atomic_load(&index_known[index]))
		return 0;
	*count = atomic_load(&index_count[index]);
	return 1;
}

/* ---------- reading a target back */

/* a colour target's pixels (BGRA, rows from the top, width * 4 bytes
each) into data, once the GPU has drawn everything before: the frame so
far is committed and waited for. 1 if it read them. */
int host_metal_texture_read(unsigned int handle, unsigned int width, unsigned int height, void *data)
{
	HOST_FRAME_LOG_SCOPE;
	id<MTLTexture> texture = texture_object(handle);

	if (!texture || !data || texture.pixelFormat != MTLPixelFormatBGRA8Unorm || width > texture.width ||
		height > texture.height)
	{
		return 0;
	}
	@autoreleasepool
	{
		NSUInteger bytes = (NSUInteger)width * height * 4;
		id<MTLBuffer> buffer = [metal.device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
		id<MTLBlitCommandEncoder> blit;

		park_end();
		blit = blit_begin();
		id<MTLCommandBuffer> submitted = commands;

		[blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
			sourceSize:MTLSizeMake(width, height, 1) toBuffer:buffer destinationOffset:0
			destinationBytesPerRow:(NSUInteger)width * 4 destinationBytesPerImage:bytes];
		[blit endEncoding];
		/* (the frame goes on in a command buffer of its own, the same
		frame: its ring and counters stay) */
		host_lifecycle_gate();
		[submitted commit];
		commands = [metal.queue commandBuffer];
		commands_generation = ++commands_generations;
		if (host_frame_log_detail())
		{
			uint64_t waited = host_frame_log_now();

			[submitted waitUntilCompleted];
			host_frame_acc.wait_readback_ns += host_frame_log_now() - waited;
			/* (this part of the frame's GPU time: its completion is not the
			present's command buffer) */
			if (submitted.GPUEndTime > submitted.GPUStartTime)
				host_frame_acc.gpu_early_ns += (uint64_t)((submitted.GPUEndTime - submitted.GPUStartTime) * 1e9);
			host_frame_acc.gpu_early_buffers++;
		}
		else
		{
			[submitted waitUntilCompleted];
		}
		memcpy(data, buffer.contents, bytes);
		return 1;
	}
}

/* ---------- dynamic resolution (host_dynres.h) */

/* HALO_DYNRES (on/off), HALO_DYNRES_FIXED=<fraction>[,toggle:<n>,<other>]
(diagnostic: a fixed fraction, or two alternating every n frames, for the
frame-by-frame comparisons in PERFORMANCE.md) override the settings */
void host_dynres_settings(int enabled, int floor_lines, int split_floor_lines, int sharpen, int ceiling_render)
{
	char screen[256];

	const char *value = getenv("HALO_DYNRES");
	double floor = floor_lines > 0 ? floor_lines : 1440.0;
	double split_floor = split_floor_lines > 0 ? split_floor_lines : 1080.0;

	/* ("auto": on on the Apple TV, whose GPU cannot hold 60 frames a second
	at 2160 lines in every scene; off on a Mac) */
	if (enabled < 0)
	{
#ifdef HALO_TVOS
		enabled = 1;
#else
		enabled = 0;
#endif
	}
	if (value && *value)
		enabled = strcmp(value, "0") && strcmp(value, "off") && strcmp(value, "false");
	host_dynres_configure(enabled, floor, split_floor);
	value = getenv("HALO_DYNRES_FIXED");
	if (value && *value && atof(value) > 0.0)
	{
		const char *toggle = strstr(value, "toggle:");
		const char *other = toggle ? strchr(toggle, ',') : NULL;

		host_dynres_configure(1, floor, split_floor);
		host_dynres_fixed(atof(value), toggle ? (uint32_t)atoi(toggle + 7) : 0, other ? atof(other + 1) : 1.0);
	}
	metal.ceiling_render = ceiling_render;
	host_telemetry_platform_describe(screen, sizeof(screen));
	host_log("dynamic resolution: %s, floor %.0f lines (split screen %.0f), ceiling the %s's, %s upscale%s; %s",
		host_dynres_enabled() ? "on" : "off", floor, split_floor, ceiling_render ? "render target" : "display",
		sharpen ? "sharp" : "bilinear", value && *value ? " (HALO_DYNRES_FIXED)" : "", screen);
}

float host_dynres_fraction(int views, int lines)
{
	/* (the ceiling: the drawable's lines, which the display shows; the
	picture is letterboxed to the back buffer's shape in it. With
	display.dynamic_resolution_ceiling "render", the back buffer's) */
	double ceiling = 0.0;

	if (metal.layer && metal.present_width && metal.present_height && !metal.ceiling_render)
	{
		double width = metal.layer.drawableSize.width, height = metal.layer.drawableSize.height;

		ceiling = width * metal.present_height < height * metal.present_width ?
			width * metal.present_height / metal.present_width : height;
	}
	return (float)host_dynres_frame_fraction(frame_serial, views, metal.thermal, metal.slot_ns,
		host_frame_log_now(), (double)lines, ceiling);
}

void host_dynres_frame(float fraction)
{
	metal.dynres_fraction = fraction;
}

static id<MTLRenderPipelineState> upscale_pipeline(uint32_t filter, MTLPixelFormat color, MTLPixelFormat depth)
{
	NSString *key;

	if (filter > METAL_UPSCALE_SHARP)
		filter = METAL_UPSCALE_BILINEAR;
	key = [NSString stringWithFormat:@"%lu/%lu", (unsigned long)color, (unsigned long)depth];
	if (!metal.upscale_pipelines[filter] || ![metal.upscale_pipelines[filter].label isEqualToString:key])
	{
		MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
		NSError *error = nil;

		descriptor.vertexFunction = metal.upscale_vertex;
		descriptor.fragmentFunction = metal.upscale_fragments[filter];
		descriptor.colorAttachments[0].pixelFormat = color;
		descriptor.depthAttachmentPixelFormat = depth;
		descriptor.stencilAttachmentPixelFormat = depth;
		descriptor.label = key;
		metal.upscale_pipelines[filter] = [metal.device newRenderPipelineStateWithDescriptor:descriptor error:&error];
		if (!metal.upscale_pipelines[filter])
			host_log("Metal: no upscale pipeline: %s", error.localizedDescription.UTF8String);
	}
	return metal.upscale_pipelines[filter];
}

/* a view's scene scaled up into the back buffer: the view's rectangle of
it drawn over with the scene target's, in a pass of the back buffer and its
depth buffer (depth and stencil neither tested nor written), which the
HUD's draws then go on in */
void host_metal_scene_upscale(const struct metal_upscale *upscale)
{
	HOST_FRAME_LOG_SCOPE;
	id<MTLTexture> scene = texture_object(upscale->scene), back = texture_object(upscale->back_buffer);
	id<MTLRenderPipelineState> pipeline;
	struct { float source[4]; float size[2]; float sharpness; float padding; } values;
	MTLScissorRect scissor;
	MTLViewport viewport;

	static int skip = -1;

	if (!metal.device || !scene || !back)
		return;
	/* (HALO_DYNRES_NO_UPSCALE, diagnostic: the upscale's draw not drawn,
	to measure what it costs with HALO_DYNRES_FIXED; the picture is then
	wrong) */
	if (skip < 0)
		skip = getenv("HALO_DYNRES_NO_UPSCALE") && atoi(getenv("HALO_DYNRES_NO_UPSCALE")) > 0;
	if (skip)
		return;
	@autoreleasepool
	{
		host_metal_set_targets(upscale->back_buffer, upscale->depth);
		if (!encoder_begin())
			return;
		pipeline = upscale_pipeline(upscale->filter, pass.color_format, pass.depth_format);
		if (!pipeline)
			return;
		if (!scissor_inside(upscale->destination, &scissor))
			return;
		viewport = (MTLViewport){ upscale->destination[0], upscale->destination[1], upscale->destination[2],
			upscale->destination[3], 0.0, 1.0 };
		values.source[0] = (float)upscale->source[0];
		values.source[1] = (float)upscale->source[1];
		values.source[2] = (float)upscale->source[2];
		values.source[3] = (float)upscale->source[3];
		values.size[0] = (float)scene.width;
		values.size[1] = (float)scene.height;
		values.sharpness = upscale->sharpness;
		values.padding = 0.0f;
		[encoder setRenderPipelineState:pipeline];
		[encoder setDepthStencilState:depth_stencil_get(&(struct metal_depth_stencil){ 0 })];
		[encoder setCullMode:MTLCullModeNone];
		[encoder setTriangleFillMode:MTLTriangleFillModeFill];
		[encoder setViewport:viewport];
		[encoder setScissorRect:scissor];
		[encoder setVertexBytes:&values length:sizeof(values) atIndex:0];
		[encoder setFragmentBytes:&values length:sizeof(values) atIndex:0];
		[encoder setFragmentTexture:scene atIndex:0];
		[encoder setFragmentSamplerState:metal.present_sampler atIndex:0];
		if (bound.counter != ~0u)
			[encoder setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
		[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		if (pass_log.record)
			pass_log.record->draws++;
		handles[upscale->scene].commands_read = commands_generation;
		handles[upscale->scene].used_serial = frame_serial;
		bound_forget();
	}
}

/* ---------- presentation */

void host_metal_present_target(unsigned int texture, unsigned int width, unsigned int height)
{
	metal.present_source = texture;
	metal.present_width = width;
	metal.present_height = height;
}

void host_metal_present(void)
{
	HOST_FRAME_LOG_SCOPE;
	if (!metal.device)
		return;
	/* (not active: nothing is committed, and no drawable taken, until it is
	again; host_lifecycle.h) */
	host_lifecycle_gate();
	@autoreleasepool
	{
		id<MTLTexture> picture;
		struct frame *frame;
		uint64_t serial = frame_serial;

		pass_flush();
		park_end();
		pass_log.end_reason = _pass_end_present;
		encoder_end();
		frame_begin();
		/* (the feedback copy, a whole screen target's size (33 MB at 4K),
		let go after FEEDBACK_IDLE_FRAMES without a draw that needed it: the
		few effects that do come and go; the command buffers that used it
		hold it until the GPU is done) */
		if (metal.feedback && frame_serial - feedback_serial > FEEDBACK_IDLE_FRAMES)
			metal.feedback = nil;
		picture = texture_object(metal.present_source);
		if (host_telemetry_enabled)
		{
			host_telemetry_sizes(metal.present_width, metal.present_height,
				metal.layer ? (uint32_t)metal.layer.drawableSize.width : 0,
				metal.layer ? (uint32_t)metal.layer.drawableSize.height : 0);
		}
		if (metal.layer)
		{
			int width = 0, height = 0;
			id<CAMetalDrawable> drawable;

			SDL_GetWindowSizeInPixels(metal.window, &width, &height);
			if (width > 0 && height > 0 &&
				(metal.layer.drawableSize.width != width || metal.layer.drawableSize.height != height))
			{
				metal.layer.drawableSize = CGSizeMake(width, height);
			}
			/* (the display's pixels, which the picture is scaled to: on a TV
			in a 1080p output mode, not the picture's 2160 lines; at the first
			frame and when either changes) */
			{
				static uint32_t logged[4];
				uint32_t now[4] = { (uint32_t)metal.layer.drawableSize.width, (uint32_t)metal.layer.drawableSize.height,
					metal.present_width, metal.present_height };

				if (memcmp(logged, now, sizeof(now)))
				{
					memcpy(logged, now, sizeof(now));
					host_log("Metal: drawable %ux%u, picture %ux%u", now[0], now[1], now[2], now[3]);
				}
			}
			if (host_frame_log_detail())
			{
				uint64_t waited = host_frame_log_now();

				drawable = [metal.layer nextDrawable];
				host_frame_acc.wait_drawable_ns += host_frame_log_now() - waited;
			}
			else
			{
				drawable = [metal.layer nextDrawable];
			}
			/* when the display showed it (0: it was not, another drawable
			replaced it first): the frame log's presented_ms, the
			telemetry's fps and missed slots, and the dynamic resolution's
			late frames. (tvOS has the handler too, from tvOS 10.3: an
			earlier comment here said it had not) */
			if (drawable && (host_frame_log_level || host_dynres_enabled()))
			{
				[drawable addPresentedHandler:^(id<MTLDrawable> shown)
				{
					uint64_t shown_ns = shown.presentedTime > 0.0 ? (uint64_t)(shown.presentedTime * 1e9) : 0;

					if (host_frame_log_level)
						host_frame_log_presented(serial, shown.presentedTime);
					host_dynres_shown(serial, shown_ns);
				}];
			}
			if (drawable)
			{
				MTLRenderPassDescriptor *descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
				id<MTLRenderCommandEncoder> window_encoder;

				descriptor.colorAttachments[0].texture = drawable.texture;
				descriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
				descriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
				descriptor.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
				window_encoder = [commands renderCommandEncoderWithDescriptor:descriptor];
				if (picture && metal.present_width && metal.present_height)
				{
					/* letterboxed to the picture's shape */
					double drawable_width = (double)drawable.texture.width;
					double drawable_height = (double)drawable.texture.height;
					double picture_width = drawable_width;
					double picture_height = drawable_width * metal.present_height / metal.present_width;
					MTLViewport viewport;

					if (picture_height > drawable_height)
					{
						picture_height = drawable_height;
						picture_width = drawable_height * metal.present_width / metal.present_height;
					}
					viewport.originX = (double)(long)((drawable_width - picture_width) / 2.0);
					viewport.originY = (double)(long)((drawable_height - picture_height) / 2.0);
					viewport.width = (double)(long)picture_width;
					viewport.height = (double)(long)picture_height;
					viewport.znear = 0.0;
					viewport.zfar = 1.0;
					[window_encoder setRenderPipelineState:metal.present_pipeline];
					[window_encoder setViewport:viewport];
					[window_encoder setFragmentTexture:picture atIndex:0];
					[window_encoder setFragmentSamplerState:metal.present_sampler atIndex:0];
					[window_encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
				}
				[window_encoder endEncoding];
				[commands presentDrawable:drawable];
			}
		}
		profile_interval_copies += profile_feedback_copies;
		const BOOL sample_profile = profile_enabled && serial % 30 == 0;
		if (sample_profile)
		{
			host_log("Metal interval frame %llu: effect calls %llu convolution frames %llu feedback snapshots %llu",
				(unsigned long long)serial, (unsigned long long)profile_effect_calls,
				(unsigned long long)profile_convolution_frames, (unsigned long long)profile_interval_copies);
			profile_effect_calls = profile_convolution_frames = profile_interval_copies = 0;
		}
		const uint64_t feedback_copies = profile_feedback_copies;
		const uint64_t feedback_bytes = profile_feedback_bytes;
		const int effect_type = profile_effect_frame == serial ? profile_effect_type : -1;
		const int effect_passes = profile_effect_frame == serial ? profile_effect_passes : 0;
		const float effect_radius = profile_effect_frame == serial ? profile_effect_radius : 0.0f;
		const int effect_video = profile_effect_frame == serial ? profile_effect_video : 0;
		if (profile_enabled && (sample_profile || profile_shader_ns > 2000000 || profile_pipeline_ns > 2000000))
		{
			host_log("Metal phases frame %llu: fog-plane %u/%u fog-screen %u/%u transparents %u/%u "
				"screen-effect %u/%u (draw attempts/feedback snapshots); shader CPU %.3f ms pipeline CPU %.3f ms",
				(unsigned long long)serial, profile_phase_draws[17], profile_phase_copies[17],
				profile_phase_draws[18], profile_phase_copies[18], profile_phase_draws[22], profile_phase_copies[22],
				profile_phase_draws[26], profile_phase_copies[26], profile_shader_ns / 1000000.0,
				profile_pipeline_ns / 1000000.0);
		}
		memset(profile_phase_draws, 0, sizeof(profile_phase_draws));
		memset(profile_phase_copies, 0, sizeof(profile_phase_copies));
		profile_shader_ns = profile_pipeline_ns = 0;
		profile_feedback_copies = profile_feedback_bytes = 0;
		/* (the dynamic resolution's: the fraction this frame's scenes drew
		at, -1 for none; the thermal state, a second at a time) */
		host_dynres_frame_done(serial, metal.dynres_fraction);
		/* (the controller's state in the log, every ten seconds) */
		if (host_dynres_enabled() && serial % 600 == 0)
		{
			struct host_dynres_status status;

			host_dynres_status(&status);
			host_log("dynamic resolution frame %llu: %.0f%% of %u lines, GPU %.2f ms; %llu frames counted, %llu late "
				"(%llu the GPU's); %llu steps down, %llu up, %llu tries failed; thermal %d",
				(unsigned long long)serial, status.fraction * 100.0, metal.present_height, status.gpu_ms,
				(unsigned long long)status.frames, (unsigned long long)status.late, (unsigned long long)status.late_gpu,
				(unsigned long long)status.steps_down, (unsigned long long)status.steps_up,
				(unsigned long long)status.probes_failed, metal.thermal);
		}
		host_frame_log_render_scale(metal.dynres_fraction);
		metal.dynres_fraction = -1.0f;
		if (host_dynres_enabled() && serial % 60 == 0)
			metal.thermal = (int)[NSProcessInfo processInfo].thermalState;
		frame = frame_current();
		pass_log_clock();
		NSArray *hoisted = frame->hoisted;
		frame->hoisted = nil;
		[commands addCompletedHandler:^(id<MTLCommandBuffer> buffer)
		{
			const uint64_t *counts = frame->visibility.contents;

			/* (the passes hoisted ahead of the frame's: done before its
			buffers are used again, and their GPU time the frame's) */
			static _Atomic uint64_t previous_end;
			uint64_t before = atomic_load(&previous_end), busy = 0;

			for (id<MTLCommandBuffer> early in hoisted)
			{
				[early waitUntilCompleted];
				/* (the dynamic resolution's GPU time: each buffer's own, from
				when the GPU was done with what came before it; not the span
				from the first to the last, which holds the GPU's idle wait
				for the frame's own buffer, committed at the present: with
				the display's pacing that span is always about an interval) */
				if (early.GPUEndTime > early.GPUStartTime)
				{
					uint64_t start = (uint64_t)(early.GPUStartTime * 1e9), end = (uint64_t)(early.GPUEndTime * 1e9);

					if (before > start)
						start = before < end ? before : end;
					busy += end - start;
					if (end > before)
						before = end;
				}
				if (host_frame_log_level)
					host_frame_log_gpu_hoisted(serial, early.GPUStartTime, early.GPUEndTime);
				if (early.status == MTLCommandBufferStatusError)
					host_log("Metal: frame %llu's hoisted passes failed on the GPU: %s", (unsigned long long)serial,
						early.error.localizedDescription.UTF8String);
			}

			pass_log_resolve((uint32_t)(serial % FRAMES_IN_FLIGHT), serial);
			uint32_t index;
			static int errors;

			if (buffer.status == MTLCommandBufferStatusError && errors < 20)
			{
				errors++;
				host_log("Metal: frame %llu failed on the GPU: %s", (unsigned long long)serial,
					buffer.error.localizedDescription.UTF8String);
			}
			if (host_frame_log_level)
			{
				host_frame_log_gpu(serial, buffer.GPUStartTime, buffer.GPUEndTime,
					buffer.status == MTLCommandBufferStatusError);
			}
			/* (the dynamic resolution's: the frame's own time on the GPU, its
			hoisted buffers' and its own, each from when the GPU was done
			with what came before it: not their waits) */
			if (buffer.GPUEndTime > buffer.GPUStartTime)
			{
				uint64_t start = (uint64_t)(buffer.GPUStartTime * 1e9), end = (uint64_t)(buffer.GPUEndTime * 1e9);

				if (before > start)
					start = before < end ? before : end;
				busy += end - start;
				atomic_store(&previous_end, end);
				if (host_dynres_enabled())
					host_dynres_gpu(serial, busy);
			}
			if (sample_profile && buffer.status == MTLCommandBufferStatusCompleted)
			{
				uint64_t now = SDL_GetTicksNS();
				uint64_t before = atomic_exchange(&profile_last_completion_ns, now);
				uint64_t previous_serial = atomic_exchange(&profile_last_completion_serial, serial);
				double gpu_ms = buffer.GPUEndTime > buffer.GPUStartTime ?
					(buffer.GPUEndTime - buffer.GPUStartTime) * 1000.0 : 0.0;
				host_log("Metal profile frame %llu: GPU %.3f ms, completion avg %.3f ms; effect %d passes %d "
					"radius %.4f video %d; feedback %llu copies %.2f MiB",
					(unsigned long long)serial, gpu_ms, before && serial > previous_serial ?
						(now - before) / (1000000.0 * (serial - previous_serial)) : 0.0,
					effect_type, effect_passes, effect_radius, effect_video,
					(unsigned long long)feedback_copies, feedback_bytes / 1048576.0);
			}
			memset(frame->sums, 0, frame->test_count * sizeof(frame->sums[0]));
			for (index = 0; index < frame->counter_count; index++)
				frame->sums[frame->counter_test[index]] += counts[index];
			for (index = 0; index < frame->test_count; index++)
			{
				if (frame->test_ended[index])
				{
					atomic_store(&index_count[frame->test_index[index]], (uint32_t)frame->sums[index]);
					atomic_store(&index_known[frame->test_index[index]], 1);
				}
			}
			atomic_store(&completed_serial, serial);
			dispatch_semaphore_signal(frames_free);
		}];
		[commands commit];
		commands = nil;
		frame_serial++;
		if ((depth_discard.enabled || depth_discard.toggle) && frame_serial % 3600 == 0)
			host_log("Metal: HALO_METAL_DEPTH_DISCARD: %llu passes' depth not stored, %llu loads after a discard",
				(unsigned long long)depth_discard.passes, (unsigned long long)depth_discard.misses);
		if (park.toggle && pass_log.timing != 2)
			park.enabled = (frame_serial / park.toggle) % 2 == 0;
		if (depth_discard.toggle && !depth_discard.broken)
			depth_discard.enabled = (frame_serial / depth_discard.toggle) % 2 == 0;
		/* (the record's own: the present's host time, up to here, is the
		frame's; the rest goes to the next) */
		if (host_frame_log_level)
		{
			host_frame_log_scope_end(&host_frame_log_scope);
			host_frame_log_scope = 0;
			host_frame_log_present(serial);
		}
		host_lifecycle_frame();
	}
}

/* ---------- the app's lifecycle (host_lifecycle.h) */

/* (uploads_last: the last command buffer committed outside a frame's, the
texture uploads; the frames' own are waited for by their slots) */
void host_metal_drain(void)
{
	/* (the frame being encoded holds a slot of its own: not waited for, it
	is not committed) */
	int busy = FRAMES_IN_FLIGHT - (commands ? 1 : 0), index, waited = 0;
	uint64_t started = host_frame_log_now();
	dispatch_semaphore_t slots = frames_free;

	if (!metal.device || !slots)
		return;
	/* (each slot taken back once its frame is done, and given back: not a
	frame's wait, the frame log's in-flight wait does not count it) */
	for (index = 0; index < busy; index++)
	{
		if (dispatch_semaphore_wait(slots, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC)) != 0)
		{
			host_log("Metal: a frame in flight did not finish in 2 s");
			break;
		}
		waited++;
	}
	for (index = 0; index < waited; index++)
		dispatch_semaphore_signal(slots);
	/* (and the frame being encoded's own committed so far: passes hoisted
	ahead of it, a readback's part) */
	if (commands)
	{
		for (id<MTLCommandBuffer> early in frame_current()->hoisted)
		{
			if (early.status >= MTLCommandBufferStatusCommitted && early.status < MTLCommandBufferStatusCompleted)
				[early waitUntilCompleted];
		}
	}
	if (uploads_last && uploads_last.status < MTLCommandBufferStatusCompleted)
		[uploads_last waitUntilCompleted];
	uploads_last = nil;
	host_log("Metal: the GPU's work finished (%d frames waited for, %.1f ms)", waited,
		(host_frame_log_now() - started) / 1e6);
}

/* (allocatedSize, or the pixels' bytes where it is 0: the simulator's) */
static int64_t texture_bytes_of(id<MTLTexture> texture)
{
	if (!texture)
		return 0;
	return texture.allocatedSize ? (int64_t)texture.allocatedSize : (int64_t)texture.width * texture.height * 4;
}

/* what is cheap to make again, a step at a time (the main thread's, between
frames or with the game stopped): not the texture cache, whose budget is the
guest's (port/linux/src/xbox_textures.c), nor the pipelines, which the draws
hold */
int64_t host_metal_trim(int step, int stopped)
{
	uint64_t done = atomic_load(&completed_serial);
	/* (while the game goes on, only what has not been used for a second or
	so: what it uses every frame would only be made again) */
	uint64_t idle = stopped ? done : (done > 60 ? done - 60 : 0);
	int64_t freed = 0;
	int index;

	if (!metal.device)
		return -1;
	@autoreleasepool
	{
		switch (step)
		{
		case 0:
			/* the renamed targets the park keeps for reuse, free again */
			for (index = 0; index < PARK_POOL; index++)
			{
				if (park.pool[index] && park.pool_serial[index] <= idle)
				{
					freed += texture_bytes_of(park.pool[index]);
					park.pool[index] = nil;
				}
			}
			return freed;
		case 1:
			/* the feedback copy (made again at its next use; a frame that has
			it keeps it) */
			if (metal.feedback && feedback_serial <= idle)
			{
				freed = texture_bytes_of(metal.feedback);
				metal.feedback = nil;
			}
			return freed;
		case 2:
			if (stopped)
			{
				/* the rings of the frames not being encoded, all finished
				(host_metal_drain): made again at their next frame */
				struct frame *current = commands ? frame_current() : NULL;

				for (index = 0; index < FRAMES_IN_FLIGHT; index++)
				{
					if (&frames[index] != current && frames[index].ring)
					{
						freed += (int64_t)(frames[index].ring.allocatedSize ? frames[index].ring.allocatedSize :
							frames[index].ring.length);
						frames[index].ring = nil;
					}
				}
			}
			else if (prepared.condition)
			{
				/* the shader cache's prepared shaders no draw has taken yet:
				one asked for later is compiled then (a memory warning's only:
				they are what keeps a level's first draws from compiling) */
				uint32_t dropped = 0;

				[prepared.condition lock];
				for (NSString *key in prepared.by_text.allKeys)
				{
					PreparedShader *shader = prepared.by_text[key];

					if (shader.state == _prepared_compiling)
						continue;
					freed += (int64_t)shader.text.length;
					shader.state = _prepared_taken;
					shader.function = nil;
					shader.text = nil;
					[prepared.by_text removeObjectForKey:key];
					dropped++;
				}
				[prepared.condition unlock];
				if (dropped)
					host_log("Metal: %u prepared shaders no draw had taken dropped", dropped);
			}
			return freed;
		default:
			return -1;
		}
	}
}
