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
#include "host_frame_log.h"
#include "host_shader_manifest.h"
#include "host_telemetry.h"
#include "host_textures.h"
#include "metal_protocol.h"

SDL_Window *host_sdl_window(unsigned int window);

#define FRAMES_IN_FLIGHT 3
#define RING_BYTES (64u << 20)
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

static void cache_add(struct cache *cache, const void *key, uint32_t size, uint64_t hash, id object)
{
	struct cache_entry *entry = malloc(sizeof(*entry) + size);

	if (!entry)
		return;
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
} metal;

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
		descriptor.stencilAttachment.texture = depth;
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
		}
		metal.view = SDL_Metal_CreateView(object);
		if (!metal.view)
			return 0;
		metal.layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(metal.view);
		metal.layer.device = metal.device;
		metal.layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
		metal.layer.framebufferOnly = YES;
		metal.layer.maximumDrawableCount = 3;
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
/* the conversions: how many, their blocks' bytes and the time */
static uint64_t conversions, conversion_source_bytes, conversion_ns;
/* a texture defined next is the GPU's alone (host_metal_texture_bc) */
static int define_private;

static void textures_report(void);

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
	if (texture_converted[handle])
		textures_converted_total -= texture_bytes[handle];
	texture_bytes[handle] = texture ? texture_size(texture) : 0;
	texture_converted[handle] = (uint8_t)(texture && converted);
	textures_total += texture_bytes[handle];
	if (texture_converted[handle])
		textures_converted_total += texture_bytes[handle];
	if (textures_total > textures_most)
		textures_most = textures_total;
	if (host_frame_log_level && texture && converted && conversions % 256 == 255)
		textures_report();
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
			[uploads commit];
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

host_shader_manifest.h's record of the shaders and pipelines made, in the
app's caches (HALO_METAL_SHADER_CACHE: a file name there, a path, or 0 for
none; HALO_METAL_SHADER_CACHE_RESET=1 empties it first, for measuring a
first launch). At the next launch its shaders and then its pipelines are compiled
again on other threads, as soon as the device is made. A draw's shader
whose text is exactly a prepared one's (the same source, compiled the same
way) takes the function compiled for it (prepared_take); every other is
compiled on the main thread as before, and finds the system's caches of
compiled shaders and pipelines filled by the threads (on a Mac, about
0.01 ms a shader instead of 9). The prepared pipelines are dropped: the
draws make their own, from the system's cache. So a record that is stale,
of another build or damaged can cost time but not change a picture. The
main thread alone writes the record; the threads read only the prepared
shaders, under their lock. */

static struct
{
	BOOL enabled;
	struct shader_manifest manifest;
	/* each shader handle's record, plus 1 (0: none) */
	uint32_t *shader_records;
	/* the records' sources (fragment's '1' or '0' first), to their indices,
	and the pipelines recorded */
	NSMutableDictionary<NSData *, NSNumber *> *sources;
	NSMutableSet<NSData *> *pipelines;
	/* each vertex layout handle's layout */
	NSMutableDictionary<NSNumber *, NSData *> *layouts;
} shader_cache;

/* a recorded shader the threads compile: its text (shader_text's), and, once
compiled, its function. A draw's compile of exactly this text and kind, on
the main thread, takes the function (the same compile's result) or waits for
the thread compiling it; one not started yet is the main thread's to
compile, and the threads skip it */
enum
{
	_prepared_waiting,
	_prepared_compiling,
	_prepared_done,
	/* the main thread compiles it, or has taken it */
	_prepared_taken,
};

@interface PreparedShader : NSObject
@property(nonatomic, strong) NSString *text;
@property(nonatomic) int fragment, state;
@property(nonatomic, strong) id<MTLFunction> function;
@end

@implementation PreparedShader
@end

static struct
{
	/* every recorded shader, in the record's order (the pipelines' indices),
	and the ones not compiled yet by text (fragment's '1' or '0' first) */
	NSMutableArray<PreparedShader *> *shaders;
	NSMutableDictionary<NSString *, PreparedShader *> *by_text;
	NSMutableArray<NSData *> *pipelines;
	/* guards the shaders' state and functions, and by_text */
	NSCondition *condition;
} prepared;

static NSString *prepared_key(NSString *text, int fragment)
{
	return [(fragment ? @"1" : @"0") stringByAppendingString:text];
}

static void shader_cache_read_shader(void *context, uint32_t index, int fragment, const char *source, uint32_t length)
{
	PreparedShader *shader = [PreparedShader new];
	char *copy = malloc(length + 1);

	(void)context;
	if (!copy)
		return;
	memcpy(copy, source, length);
	copy[length] = 0;
	/* (an entry for every record, compiled or not, as the pipelines name
	them by index) */
	shader.text = strlen(copy) == length ? shader_text(copy, fragment) : nil;
	shader.fragment = fragment;
	shader.state = shader.text ? _prepared_waiting : _prepared_taken;
	[prepared.shaders addObject:shader];
	if (shader.text)
		prepared.by_text[prepared_key(shader.text, fragment)] = shader;
	if (shader_cache.manifest.kept)
	{
		NSMutableData *key = [NSMutableData dataWithBytes:fragment ? "1" : "0" length:1];

		[key appendBytes:source length:length];
		shader_cache.sources[key] = @(index);
	}
	free(copy);
}

static void shader_cache_read_pipeline(void *context, const struct shader_manifest_pipeline *pipeline)
{
	NSData *record = [NSData dataWithBytes:pipeline length:sizeof(*pipeline)];

	(void)context;
	[prepared.pipelines addObject:record];
	if (shader_cache.manifest.kept)
		[shader_cache.pipelines addObject:record];
}

/* the main thread's compile of this text: the prepared function, after
waiting for a thread compiling it; nil if it is the main thread's to compile
(*claimed then the record's entry, if there is one, for prepared_give) */
static id<MTLFunction> prepared_take(NSString *text, int fragment, PreparedShader **claimed)
{
	PreparedShader *shader;
	id<MTLFunction> function = nil;

	*claimed = nil;
	if (!prepared.condition)
		return nil;
	[prepared.condition lock];
	shader = prepared.by_text[prepared_key(text, fragment)];
	if (shader)
	{
		while (shader.state == _prepared_compiling)
			[prepared.condition wait];
		if (shader.state == _prepared_done)
			function = shader.function;
		else if (shader.state == _prepared_waiting)
		{
			shader.state = _prepared_taken;
			*claimed = shader;
		}
		/* (one entry for one handle: a second shader of the same source
		compiles its own, as before) */
		[prepared.by_text removeObjectForKey:prepared_key(text, fragment)];
	}
	[prepared.condition unlock];
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

/* the shaders and then the pipelines of the record, compiled on threads
(two at a time: the game's main thread keeps a core), the shaders' kept for
the draws (prepared_take) and the pipelines dropped */
static void shader_cache_prepare(void)
{
	MTLCompileOptions *options = compile_options();
	id<MTLDevice> device = metal.device;
	uint64_t started = SDL_GetTicksNS();
	/* (user-initiated: the main thread may wait for one, during the load) */
	dispatch_queue_t queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
	NSArray<PreparedShader *> *shaders = [prepared.shaders copy];
	NSArray<NSData *> *pipelines = [prepared.pipelines copy];

	dispatch_async(queue, ^
	{
		dispatch_group_t group = dispatch_group_create();
		__block _Atomic NSUInteger next = 0;
		__block _Atomic uint32_t compiled = 0, failed_shaders = 0, failed_pipelines = 0;
		int worker;
		uint64_t shaders_done;

		for (worker = 0; worker < 2; worker++)
		{
			dispatch_group_async(group, queue, ^
			{
				NSUInteger at;

				while ((at = atomic_fetch_add(&next, 1)) < shaders.count)
				{
					@autoreleasepool
					{
						PreparedShader *shader = shaders[at];
						NSError *error = nil;
						id<MTLLibrary> library;
						id<MTLFunction> function;

						[prepared.condition lock];
						if (shader.state != _prepared_waiting)
						{
							[prepared.condition unlock];
							continue;
						}
						shader.state = _prepared_compiling;
						[prepared.condition unlock];
						library = [device newLibraryWithSource:shader.text options:options error:&error];
						function = [library newFunctionWithName:shader_entry_point(shader.fragment)];
						[prepared.condition lock];
						shader.function = function;
						shader.state = function ? _prepared_done : _prepared_taken;
						[prepared.condition broadcast];
						[prepared.condition unlock];
						atomic_fetch_add(function ? &compiled : &failed_shaders, 1);
					}
				}
			});
		}
		dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
		shaders_done = SDL_GetTicksNS();
		atomic_store(&next, 0);
		for (worker = 0; worker < 2; worker++)
		{
			dispatch_group_async(group, queue, ^
			{
				NSUInteger at;

				while ((at = atomic_fetch_add(&next, 1)) < pipelines.count)
				{
					@autoreleasepool
					{
						struct shader_manifest_pipeline pipeline;
						id<MTLFunction> vertex, fragment;
						NSError *error = nil;

						memcpy(&pipeline, pipelines[at].bytes, sizeof(pipeline));
						[prepared.condition lock];
						vertex = shaders[pipeline.vertex_shader].function;
						fragment = shaders[pipeline.fragment_shader].function;
						[prepared.condition unlock];
						/* (one whose shader failed, or the main thread is still
						compiling, is the draw's to make) */
						if (!vertex || !fragment)
							continue;
						if (!shader_cache_pipeline_sane(&pipeline) ||
							![device newRenderPipelineStateWithDescriptor:pipeline_descriptor_make(vertex, fragment,
								vertex_descriptor_make(&pipeline.layout), pipeline.color_format, pipeline.depth_format,
								&pipeline.blend) error:&error])
						{
							atomic_fetch_add(&failed_pipelines, 1);
						}
					}
				}
			});
		}
		dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
		host_log("Metal: shader cache: %u of %lu shaders prepared in %.0f ms (%u failed; the rest the draws "
			"compiled first), %lu pipelines in %.0f ms (%u failed), on other threads", atomic_load(&compiled),
			(unsigned long)shaders.count, (shaders_done - started) / 1e6, atomic_load(&failed_shaders),
			(unsigned long)pipelines.count, (SDL_GetTicksNS() - shaders_done) / 1e6, atomic_load(&failed_pipelines));
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

/* the record opened, and what it holds prepared */
static void shader_cache_start(void)
{
	const char *setting = getenv("HALO_METAL_SHADER_CACHE");
	NSString *path, *key, *build;
	struct shader_manifest_reader reader;
	MTLCompileOptions *options = compile_options();
	uint64_t started = SDL_GetTicksNS();

	if (setting && (!strcmp(setting, "0") || !strcmp(setting, "off")))
	{
		host_log("Metal: shader cache: off (HALO_METAL_SHADER_CACHE)");
		return;
	}
	shader_settings_read();
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

		[[NSFileManager defaultManager] createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil
			error:nil];
		path = [folder stringByAppendingPathComponent:setting && *setting ?
			[[NSString stringWithUTF8String:setting] lastPathComponent] : @"metal_shaders.manifest"];
	}
	if (getenv("HALO_METAL_SHADER_CACHE_RESET") && atoi(getenv("HALO_METAL_SHADER_CACHE_RESET")))
		unlink(path.fileSystemRepresentation);
	/* (what must match for the records to be read: the GPU, and what the
	shaders are compiled as; what must match for them to be kept: the build
	that wrote them, and the system) */
	key = [NSString stringWithFormat:@"gpu %@|msl %lx|invariance %d|layout %zu", metal.device.name,
		(unsigned long)options.languageVersion, options.preserveInvariance ? 1 : 0,
		sizeof(struct shader_manifest_pipeline)];
	build = [NSString stringWithFormat:@"%@|%@", build_identifier(),
		[NSProcessInfo processInfo].operatingSystemVersionString];
	shader_cache.sources = [NSMutableDictionary dictionary];
	shader_cache.pipelines = [NSMutableSet set];
	shader_cache.layouts = [NSMutableDictionary dictionary];
	prepared.shaders = [NSMutableArray array];
	prepared.by_text = [NSMutableDictionary dictionary];
	prepared.pipelines = [NSMutableArray array];
	prepared.condition = [NSCondition new];
	reader.context = NULL;
	reader.shader = shader_cache_read_shader;
	reader.pipeline = shader_cache_read_pipeline;
	if (!shader_manifest_open(&shader_cache.manifest, path.fileSystemRepresentation, key.UTF8String,
		build.UTF8String, &reader))
	{
		host_log("Metal: shader cache: %s cannot be opened (another copy of the game has it?): none kept",
			path.fileSystemRepresentation);
		shader_cache.sources = nil;
		shader_cache.pipelines = nil;
		shader_cache.layouts = nil;
		prepared.shaders = nil;
		prepared.by_text = nil;
		prepared.pipelines = nil;
		prepared.condition = nil;
		return;
	}
	shader_cache.shader_records = calloc(HANDLE_COUNT, sizeof(*shader_cache.shader_records));
	shader_cache.enabled = shader_cache.shader_records != NULL;
	host_log("Metal: shader cache: %s: %lu shaders and %lu pipelines read in %.1f ms%s%s%s", path.fileSystemRepresentation,
		(unsigned long)prepared.shaders.count, (unsigned long)prepared.pipelines.count,
		(SDL_GetTicksNS() - started) / 1e6,
		shader_cache.manifest.kept || !prepared.shaders.count ? "" : " (another build's: started again)",
		shader_cache.manifest.discarded[0] ? "; not read past " : "", shader_cache.manifest.discarded);
	if (prepared.shaders.count)
		shader_cache_prepare();
}

static void shader_cache_record_shader(uint32_t handle, const char *source, int fragment)
{
	NSMutableData *key;
	NSNumber *index;
	size_t length;

	if (!shader_cache.enabled || handle >= HANDLE_COUNT)
		return;
	length = strlen(source);
	key = [NSMutableData dataWithBytes:fragment ? "1" : "0" length:1];
	[key appendBytes:source length:length];
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

static void shader_cache_record_layout(uint32_t handle, const struct metal_vertex_layout *layout)
{
	if (shader_cache.enabled)
		shader_cache.layouts[@(handle)] = [NSData dataWithBytes:layout length:sizeof(*layout)];
}

static void shader_cache_record_pipeline(const struct pipeline_key *key)
{
	struct shader_manifest_pipeline pipeline;
	NSData *layout, *record;

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
	if ([shader_cache.pipelines containsObject:record])
		return;
	{
		uint64_t started = host_frame_log_detail() ? host_frame_log_now() : 0;

		if (shader_manifest_add_pipeline(&shader_cache.manifest, &pipeline))
			[shader_cache.pipelines addObject:record];
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

void host_metal_draw(const struct metal_draw *original)
{
	HOST_FRAME_LOG_SCOPE;
	struct metal_draw adjusted;
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
	if (metal.debug & 3)
	{
		adjusted = *original;
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
#ifndef HALO_TVOS
			/* (tvOS's SDK has no presented handler: presented_ms stays 0 there) */
			if (drawable && host_frame_log_level)
			{
				[drawable addPresentedHandler:^(id<MTLDrawable> shown)
				{
					host_frame_log_presented(serial, shown.presentedTime);
				}];
			}
#endif
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
		frame = frame_current();
		pass_log_clock();
		NSArray *hoisted = frame->hoisted;
		frame->hoisted = nil;
		[commands addCompletedHandler:^(id<MTLCommandBuffer> buffer)
		{
			const uint64_t *counts = frame->visibility.contents;

			/* (the passes hoisted ahead of the frame's: done before its
			buffers are used again, and their GPU time the frame's) */
			for (id<MTLCommandBuffer> early in hoisted)
			{
				[early waitUntilCompleted];
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
	}
}
