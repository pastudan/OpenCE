/*
D3D8_METAL.C

The Xbox Direct3D 8 device of the macOS build (port/macos/README.md),
drawing with Metal: the desktop OpenGL device's (port/linux/src/d3d8_gl.c)
Direct3D state, render targets, shader translation and caches, with each
draw, clear and texture handed to the host's Metal renderer
(port/macos/host/host_metal.m) as the structs of metal_protocol.h.

The game drives the device through the XDK's inline functions, which keep
the "simple" render states in D3D__RenderState and call into this file for
everything else. At each draw the full state is read back from there and
translated: the vertex program into Metal Shading Language once per shader
(nv2a_vsh.c), the pixel shader - texture stages and register combiners, 57
render states - once per combination (nv2a_psh.c), and the rest into the
draw's Metal state (the host keeps a pipeline for each combination).

Conventions carried over from the Xbox:
- Clip space is D3D's (depth 0..1, y down in window space), which is
  Metal's: viewports, scissors and texture rows have D3D's top-left origin.
- Render targets and textures are identified by the physical address in
  their Data field. A texture whose data is a render target samples the
  target's texture directly (render-to-texture).
- Vertex data is read from guest memory at draw time: the host copies what
  a draw reads into the frame's buffer, so the game may rewrite it at once.
*/

#include "xgpu.h"
#include "sdl_platform.h"
#include "halo_ui_pointer.h"
#include "port_config.h"
#include "main/console.h"
#include "guest_host.h"
#include "metal_protocol.h"

#include <SDL3/SDL.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* a guest pointer as the host takes it: its offset in the guest's memory */
#define GUEST_ADDRESS(pointer) ((guest_address)(uintptr_t)(pointer))

/* the uniform buffers as the shaders lay them out (xgpu.h) */
typedef char vertex_uniforms_size_assert[sizeof(struct xgpu_vertex_uniforms) == METAL_VERTEX_UNIFORMS_BYTES ? 1 : -1];
typedef char pixel_uniforms_size_assert[sizeof(struct xgpu_pixel_uniforms) == METAL_PIXEL_UNIFORMS_BYTES ? 1 : -1];
typedef char draw_size_assert[sizeof(struct metal_draw) % 4 == 0 ? 1 : -1];

void d3d8_surface_initialize(D3DSurface *surface, D3DFORMAT format, unsigned long width, unsigned long height);
void d3d8_surface_resize(D3DSurface *surface, D3DFORMAT format, unsigned long width, unsigned long height);


/* ---------- the screen's width

The Xbox screen is 640x480. The native ports can draw a wider one: 480
lines, and as many columns as the display's shape gives. On Android that is
display.screen_width (port_config.c; 640 keeps 4:3); on the desktop, the
shape of the window (of the display while the game is fullscreen, or of
display.resolution), and 640 where display.resolution_scaling is "original".
A height in display.resolution_scaling ("720" to "2160") draws that many
pixel rows whatever the window's, which the display blit scales.
The game's camera derives its horizontal field of view from the viewport, so
the 3D view simply widens. The menus and full-screen overlays are laid out
for 640 columns; while they draw (halo_screen_ui_offset), everything shifts
right to center them.

The desktop also draws at that resolution (platform_screen_mode): render
targets the size of the screen get that many pixels (screen_scale), and
viewports, clears and visibility counts are scaled to match, so the game
still works in its 480 lines; "original" draws 640x480, scaled up at
presentation. The width and the scale change only between frames, after one
is presented (halo_screen_commit). */

#define SCREEN_HEIGHT 480
#define SCREEN_MAXIMUM_WIDTH 1920

/* the width the game draws, 0 until first asked, and how many pixels a
render target the size of the screen has per unit of it */
static long screen_width;
static float screen_scale[2] = { 1.0f, 1.0f };
static long ui_offset;
#define UI_OFFSET ((GLint)ui_offset)

/* ---------- anti-aliasing

display.anti_aliasing, off unless it is set: the Xbox drew without any.
"fxaa" and "smaa" are passes over each window's 3D view before the HUD and
menus are drawn over it (halo_screen_anti_alias, xgpu_post.c); "ssaa2x"
draws the screen's targets at twice the resolution each way
(screen_mode_choose), which the display blit scales down; "msaa2x" to
"msaa8x" draw the back buffer and its depth buffer with that many samples a
pixel (render_target_multisample). The setting is read again between frames
(halo_screen_commit), so that a change applies from the next one. */

enum
{
	_anti_aliasing_off,
	_anti_aliasing_fxaa,
	_anti_aliasing_smaa,
	_anti_aliasing_ssaa,
	_anti_aliasing_msaa,
};

static const struct
{
	const char *name;
	int mode;
	int samples;
} anti_aliasing_values[] =
{
	{ "off", _anti_aliasing_off, 0 },
	{ "fxaa", _anti_aliasing_fxaa, 0 },
	{ "smaa", _anti_aliasing_smaa, 0 },
	{ "ssaa2x", _anti_aliasing_ssaa, 0 },
	{ "msaa2x", _anti_aliasing_msaa, 2 },
	{ "msaa4x", _anti_aliasing_msaa, 4 },
	{ "msaa8x", _anti_aliasing_msaa, 8 },
};

#define NUMBER_OF_ANTI_ALIASING_VALUES ((int)(sizeof(anti_aliasing_values) / sizeof(anti_aliasing_values[0])))

/* the value in effect, -1 until the setting is first read; multisampling's
samples a pixel, at most the GPU's (anti_aliasing_prepare); and the GPU's
most samples and largest target (texture and renderbuffer), 0 until the GL
context exists */
static int anti_aliasing_value = -1;
static int anti_aliasing_samples;
static GLint anti_aliasing_maximum_samples, maximum_target_size;

static void anti_aliasing_prepare(void);

/* display.anti_aliasing's value (none of them: the first, off) */
static void anti_aliasing_read(void)
{
	const char *setting = config_string("display.anti_aliasing");
	int value;

	for (value = NUMBER_OF_ANTI_ALIASING_VALUES - 1;
		value > 0 && strcmp(setting, anti_aliasing_values[value].name);
		value--)
	{
	}
	if (value == anti_aliasing_value)
		return;
	anti_aliasing_value = value;
	if (strcmp(setting, anti_aliasing_values[value].name))
		platform_log("anti-aliasing: \"%s\" is unknown, so off", setting);
	else
		platform_log("anti-aliasing: %s", setting);
	anti_aliasing_prepare();
}

static int anti_aliasing(void)
{
	if (anti_aliasing_value < 0)
		anti_aliasing_read();
	return anti_aliasing_values[anti_aliasing_value].mode;
}

static void screen_mode_choose(long *width, float scale[2])
{
	long display_width, display_height;

	*width = 640;
	scale[0] = scale[1] = 1.0f;
	if (platform_screen_mode(&display_width, &display_height) && display_width > 0 && display_height > 0)
	{
		long wanted = (SCREEN_HEIGHT * display_width + display_height / 2) / display_height;

		*width = wanted < 640 ? 640 : wanted > SCREEN_MAXIMUM_WIDTH ? SCREEN_MAXIMUM_WIDTH : wanted & ~1L;
		scale[0] = (float)display_width / (float)*width;
		scale[1] = (float)display_height / (float)SCREEN_HEIGHT;
		/* a display narrower or wider than the game can be: the picture
		keeps its shape and the display blit letterboxes it */
		if (*width != wanted && *width != (wanted & ~1L))
			scale[0] = scale[1] = scale[0] < scale[1] ? scale[0] : scale[1];
		/* a height in display.resolution_scaling above what the GPU's
		largest target holds ("2160" on a wide window): as many as it has */
		if (maximum_target_size > 0)
		{
			float factor = 1.0f;

			if (*width * scale[0] > (float)maximum_target_size)
				factor = (float)maximum_target_size / (*width * scale[0]);
			if (SCREEN_HEIGHT * scale[1] * factor > (float)maximum_target_size)
				factor = (float)maximum_target_size / (SCREEN_HEIGHT * scale[1]);
			scale[0] *= factor;
			scale[1] *= factor;
		}
		/* supersampling: twice the pixels each way, or as many as the GPU's
		largest target has (once it is known), which the display blit
		scales down; not with "original" resolution scaling, which draws
		the Xbox's 640x480 */
		if (anti_aliasing() == _anti_aliasing_ssaa && maximum_target_size > 0)
		{
			float factor = 2.0f;

			if (*width * scale[0] * factor > (float)maximum_target_size)
				factor = (float)maximum_target_size / (*width * scale[0]);
			if (SCREEN_HEIGHT * scale[1] * factor > (float)maximum_target_size)
				factor = (float)maximum_target_size / (SCREEN_HEIGHT * scale[1]);
			if (factor > 1.0f)
			{
				scale[0] *= factor;
				scale[1] *= factor;
			}
		}
	}
}

long halo_screen_width(void)
{
	if (!screen_width)
	{
		screen_mode_choose(&screen_width, screen_scale);
		platform_log("screen: %ldx%d drawn at %.0fx%.0f", screen_width, SCREEN_HEIGHT,
			screen_width * screen_scale[0], SCREEN_HEIGHT * screen_scale[1]);
	}
	return screen_width;
}

/* the display's pixels for each of the 480 lines (text_hires.c) */
float halo_screen_pixel_scale(void)
{
	halo_screen_width();
	return screen_scale[1];
}

/* how many pixels the screen's targets draw to the Xbox's one, the larger
of the two ways (the screen effects' convolutions: rasterizer_xbox_screen_effect.c) */
float halo_screen_scale(void)
{
	halo_screen_width();
	return screen_scale[0] > screen_scale[1] ? screen_scale[0] : screen_scale[1];
}

/* display.shadow_resolution: the size the shadow maps are drawn at.

Each object's shadow is drawn from above into a 128x128 map, blurred into
another and projected onto the level under it (rasterizer_xbox_shadows.c).
On a large screen, a shadow's 128 texels show as steps along its edge, which
crawl as the object moves. The two maps, the game's only R5G6B5 render
targets (rasterizer_xbox.c), can be drawn larger as the screen's targets are
(render_target_get): the game's viewports, clears and quads, in its 128 units,
scale up with them. The scale is a power of two up to 8 (1024x1024), which
the blur needs to cover the same part of the map as the Xbox's
(rasterizer_shadow_convolve); 1, the default, draws them as the Xbox did. It
changes only between frames (halo_screen_commit). */
#define SHADOW_MAP_SIZE 128
#define SHADOW_MAP_MAXIMUM_SCALE 8

/* 0 until first asked */
static long shadow_scale;
static unsigned long shadow_scale_read_at;

static long shadow_scale_choose(void)
{
	long resolution = config_integer("display.shadow_resolution");
	long scale = 1;

	while (scale < SHADOW_MAP_MAXIMUM_SCALE && SHADOW_MAP_SIZE * scale * 2 <= resolution)
		scale *= 2;
	return scale;
}

/* the shadow maps' pixels for each of their 128 texels each way */
long halo_shadow_map_scale(void)
{
	if (!shadow_scale)
	{
		shadow_scale_read_at = config_changes();
		shadow_scale = shadow_scale_choose();
		platform_log("shadow maps: %ldx%ld", SHADOW_MAP_SIZE * shadow_scale, SHADOW_MAP_SIZE * shadow_scale);
	}
	return shadow_scale;
}

void halo_screen_ui_offset(unsigned char centered)
{
	ui_offset = centered ? (halo_screen_width() - 640) / 2 : 0;
}

/* ---------- state the XDK header's inline functions read and write */

DWORD D3D__RenderState[D3DRS_MAX];
DWORD D3D__TextureState[D3DTSS_MAXSTAGES][D3DTSS_MAX];
WORD *D3D__IndexData;
BYTE D3D__StateBlockDirty[1024];

/* ---------- vertex shaders */

#define VERTEX_SHADER_SIGNATURE 0x76736864UL /* 'vshd' */
#define VERTEX_PROGRAM_SLOTS 136

struct vertex_element
{
	unsigned char reg;
	unsigned char stream;
	unsigned char type;
	unsigned char bytes;
	unsigned short offset;
};

struct vertex_shader_object
{
	unsigned long signature;
	unsigned long id;
	DWORD *instructions;
	unsigned long instruction_count;
	struct vertex_element elements[XGPU_VERTEX_ATTRIBUTE_COUNT];
	unsigned long element_count;
	unsigned long packed_mask;
	/* [0] streams per the declaration, [1] immediate mode (all floats) */
	GLuint shader[2];
	/* one of the game's model lighting programs (halo_vertex_shader_lighting),
	whose draws can be lit for each pixel (display.per_pixel_lighting): where
	its lighting's normal and position are (lighting.lights is 0 for the
	others), and its shaders that hand them on, as shader[] */
	struct nv2a_vertex_lighting lighting;
	GLuint lit_shader[2];
	/* a shader lit for each pixel failed to compile or link: lit as the
	vertex shader lights it from then on */
	BOOL lighting_failed;
	/* the vertex layout its draws last used (layout_get), and what it was
	made of */
	unsigned int layout;
	unsigned long layout_hash;
	/* which of shader[] and lit_shader[] have been compiled (bits 0 and 1,
	and 2 and 3): one that failed stays 0 and is not compiled again at each
	draw */
	unsigned char shaders_tried;
};

/* ---------- programs */

struct fragment_entry
{
	struct fragment_entry *next;
	unsigned long hash;
	struct nv2a_pixel_shader_key key;
	GLuint shader;
};

/* the uniforms a draw sets besides the vertex constants */
struct draw_uniforms
{
	float viewport_scale[4];
	float viewport_offset[4];
	float point_size;
	float ps_c0[8][4];
	float ps_c1[8][4];
	float ps_final_c0[4];
	float ps_final_c1[4];
	float fog_color[4];
	float fog_parameters[4];
	float alpha_reference;
	float bump_matrix[4][4];
	float bump_luminance[4][4];
	float texture_scale[4][4];
	float screen_offset;
	float texture_lod_bias[4];
};

#define FRAGMENT_BUCKETS 1024

static struct fragment_entry *fragment_buckets[FRAGMENT_BUCKETS];

/* ---------- render targets */

/* what a render target's scale follows (render_target_get): an entry whose
scale no longer fits is freed (render_targets_evict) */
enum render_target_kind
{
	_render_target_plain,
	/* the screen's targets and the secondary target: screen_scale's */
	_render_target_screen,
	/* a shadow map: halo_shadow_map_scale's */
	_render_target_shadow_map,
	/* a half-resolution target (half_res, below): a fraction of the
	scene's scale */
	_render_target_half,
};

struct render_target_entry
{
	struct render_target_entry *next;
	/* the next with the same address bucket (render_target_bucket) */
	struct render_target_entry *next_in_bucket;
	struct xgpu_render_target target;
	unsigned long last_rendered;
	/* the back buffer or its depth buffer, which the 3D view is drawn into:
	multisampled with multisampling (render_target_multisample) */
	BOOL screen_buffer;
	enum render_target_kind kind;
	/* a half-resolution target's: the scene's scale it was made for */
	float scene_scale[2];
};

/* every draw looks up its targets and whether its textures are render
targets, of which there are dozens */
#define RENDER_TARGET_BUCKET_COUNT 256

static struct render_target_entry *render_target_buckets[RENDER_TARGET_BUCKET_COUNT];

static struct render_target_entry **render_target_bucket(unsigned long data)
{
	return &render_target_buckets[((data >> 12) ^ (data >> 20)) % RENDER_TARGET_BUCKET_COUNT];
}

static struct render_target_entry *render_targets;

static void render_targets_evict(void);

/* ---------- the device */

#define VISIBILITY_TEST_SLOTS METAL_VISIBILITY_SLOTS

struct metal_device
{
	D3DPRESENT_PARAMETERS presentation;
	D3DSurface back_buffer;
	D3DSurface depth_buffer;
	D3DSurface *render_target;
	D3DSurface *depth_stencil;
	D3DVIEWPORT8 viewport;
	D3DMATRIX transforms[D3DTS_MAX];
	D3DBaseTexture *textures[D3DTSS_MAXSTAGES];
	D3DPalette *palettes[D3DTSS_MAXSTAGES];
	D3DSHADERCONSTANTMODE shader_constant_mode;

	struct vertex_shader_object *vertex_shader;
	struct vertex_shader_object *program_slots[VERTEX_PROGRAM_SLOTS];
	unsigned long program_address;
	float constants[XGPU_VERTEX_CONSTANT_COUNT][4];
	float viewport_scale[4];
	float viewport_offset[4];

	struct
	{
		DWORD data;
		UINT stride;
	} streams[16];
	/* SetIndices' base vertex: added to every index of an indexed draw (the
	dynamic vertex buffers keep each buffer's vertices at an offset into one
	vertex buffer, and their triangles count from 0: contrails, lightning) */
	UINT base_vertex_index;

	/* the current value of each input register (SetVertexData) */
	float attributes[XGPU_VERTEX_ATTRIBUTE_COUNT][4];
	BOOL immediate_active;
	D3DPRIMITIVETYPE immediate_type;
	/* the input registers written since Begin, a bit each */
	unsigned long immediate_written;
	float *immediate_vertices;
	unsigned long immediate_count;
	unsigned long immediate_capacity;

	/* the game's visibility tests: each index's test has ended since the
	device began, and the pixels each of the game's pixels covered in its
	target (render_target_get), which its count is divided by; the test
	running (the host's number for it) */
	BOOL query_pending[VISIBILITY_TEST_SLOTS];
	float query_area[VISIBILITY_TEST_SLOTS];
	BOOL visibility_test_active;
	unsigned int visibility_test;

	unsigned long frame;
	unsigned long next_vertex_shader_id;
	/* the window and the host's renderer are there to draw with */
	BOOL ready;
	BOOL created;
};

static struct metal_device device;

/* debug.gpu_stats prints these once a second */
static struct
{
	unsigned long draws, immediate_draws, clears, presents;
	unsigned long skipped_no_program, skipped_no_target, skipped_link;
	unsigned long target_changes;
	/* vertex and index bytes drawn from the mirror, and streamed */
	unsigned long mirrored_bytes, streamed_bytes;
} stats;

static D3DDevice *device_pointer(void)
{
	return (D3DDevice *)&device;
}

static float dword_to_float(DWORD value)
{
	union { DWORD d; float f; } u;

	u.d = value;
	return u.f;
}

static void color_to_vec4(D3DCOLOR color, float *out)
{
	out[0] = ((color >> 16) & 0xff) / 255.0f;
	out[1] = ((color >> 8) & 0xff) / 255.0f;
	out[2] = (color & 0xff) / 255.0f;
	out[3] = ((color >> 24) & 0xff) / 255.0f;
}

/* ---------- debugging settings, read once (gl_initialize) */

static struct
{
	/* debug.gpu_skip_vertex_shaders "<id>,<id>..." drops draws by vertex
	shader, for finding which pass produces something (port_config.c) */
	const char *skip_vertex_shaders;
	const char *dump_shaders;
	BOOL statistics;
} debug_settings;

/* ---------- what the OpenGL device's helpers did

The texture code shared with the OpenGL device (xbox_textures.c) tells it
when a texture changes behind its state cache; Metal's state is the host's,
set again for each draw. */

void xgpu_gl_state_invalidate(void)
{
}

void xgpu_gl_texture_deleted(unsigned int texture)
{
	(void)texture;
}

/* the source blend factor the pixel shader applies (nv2a_pixel_shader_key's
source_blend): none, Metal blends any factors */
static unsigned char source_blend_in_shader(void)
{
	return 0;
}

/* ---------- vertex layouts

A draw's vertex layout (each attribute's format, stream and offset, and
each stream's stride) is a host object, made once for each layout: the
host's pipelines are made of them (host_metal_vertex_layout). An attribute
no stream gives reads its constant value (the device's attributes, buffer
METAL_BUFFER_CONSTANT_ATTRIBUTES), a NORMPACKED3 one the integer zero after
them. */

#define LAYOUT_BUCKET_COUNT 256

struct layout_entry
{
	struct layout_entry *next;
	unsigned long hash;
	struct metal_vertex_layout layout;
	unsigned int handle;
};

static struct layout_entry *layout_buckets[LAYOUT_BUCKET_COUNT];

static unsigned long hash_words(const void *data, unsigned long size);

static unsigned int layout_get(const struct metal_vertex_layout *layout, unsigned long *hash_out)
{
	unsigned long hash = hash_words(layout, sizeof(*layout));
	struct layout_entry **bucket = &layout_buckets[hash % LAYOUT_BUCKET_COUNT];
	struct layout_entry *entry;

	if (hash_out)
		*hash_out = hash;
	for (entry = *bucket; entry; entry = entry->next)
	{
		if (entry->hash == hash && !memcmp(&entry->layout, layout, sizeof(*layout)))
			return entry->handle;
	}
	entry = calloc(1, sizeof(*entry));
	if (!entry)
		return 0;
	entry->hash = hash;
	entry->layout = *layout;
	entry->handle = host_metal_vertex_layout(layout);
	entry->next = *bucket;
	*bucket = entry;
	return entry->handle;
}

/* every attribute reading its constant (packed_mask's, the integer zero) */
static void layout_constants(struct metal_vertex_layout *layout, unsigned long packed_mask)
{
	unsigned long index;

	memset(layout, 0, sizeof(*layout));
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
	{
		struct metal_vertex_attribute *attribute = &layout->attributes[index];

		attribute->buffer = METAL_BUFFER_CONSTANT_ATTRIBUTES;
		if (packed_mask & (1UL << index))
		{
			attribute->format = METAL_VERTEX_FORMAT_UINT;
			attribute->offset = XGPU_VERTEX_ATTRIBUTE_COUNT * 16;
		}
		else
		{
			attribute->format = METAL_VERTEX_FORMAT_FLOAT4;
			attribute->offset = (metal_u32)(index * 16);
		}
	}
	layout->buffers[METAL_BUFFER_CONSTANT_ATTRIBUTES].stride = 16;
	layout->buffers[METAL_BUFFER_CONSTANT_ATTRIBUTES].constant = 1;
}

/* the attributes' constant values (and a zero for NORMPACKED3), sent again
when any has changed */
static float constant_attributes[XGPU_VERTEX_ATTRIBUTE_COUNT + 1][4];
static unsigned int constant_attributes_serial;

static void draw_constant_attributes(struct metal_draw *draw)
{
	if (!constant_attributes_serial || memcmp(constant_attributes, device.attributes, sizeof(device.attributes)))
	{
		memcpy(constant_attributes, device.attributes, sizeof(device.attributes));
		if (!++constant_attributes_serial)
			constant_attributes_serial = 1;
	}
	draw->constant_attributes = GUEST_ADDRESS(constant_attributes);
	draw->constant_attributes_serial = constant_attributes_serial;
}

/* ---------- vertical blank emulation */

#define VERTICAL_BLANK_NANOSECONDS (1000000000L / 60)

static pthread_mutex_t vertical_blank_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vertical_blank_condition = PTHREAD_COND_INITIALIZER;
static D3DCALLBACK vertical_blank_callback;
static unsigned long vertical_blank_count;
static volatile unsigned int flip_count;
static unsigned long pending_flips;
static BOOL vertical_blank_thread_started = FALSE;

static void *vertical_blank_thread(void *unused)
{
	struct timespec next;

	(void)unused;
	clock_gettime(CLOCK_MONOTONIC, &next);
	for (;;)
	{
		D3DCALLBACK callback;

		next.tv_nsec += VERTICAL_BLANK_NANOSECONDS;
		if (next.tv_nsec >= 1000000000L)
		{
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

		pthread_mutex_lock(&vertical_blank_lock);
		vertical_blank_count++;
		/* a presented frame becomes visible at the next vertical blank */
		if (pending_flips)
		{
			pending_flips--;
			flip_count++;
		}
		callback = vertical_blank_callback;
		pthread_cond_broadcast(&vertical_blank_condition);
		pthread_mutex_unlock(&vertical_blank_lock);

		if (callback)
			callback(0);
	}
	return NULL;
}

static void vertical_blank_start(void)
{
	pthread_mutex_lock(&vertical_blank_lock);
	if (!vertical_blank_thread_started)
	{
		pthread_t thread;

		if (pthread_create(&thread, NULL, vertical_blank_thread, NULL) == 0)
		{
			pthread_detach(thread);
			vertical_blank_thread_started = TRUE;
		}
		else
		{
			platform_log("cannot start the vertical blank thread");
		}
	}
	pthread_mutex_unlock(&vertical_blank_lock);
}

/* replaces main/d3d_intimacy.cpp, which reads the counter out of the Xbox
Direct3D runtime's private device structure */
volatile unsigned int *d3d_find_flipcount(void)
{
	return &flip_count;
}

void WINAPI D3DDevice_SetVerticalBlankCallback(D3DCALLBACK callback)
{
	pthread_mutex_lock(&vertical_blank_lock);
	vertical_blank_callback = callback;
	pthread_mutex_unlock(&vertical_blank_lock);
	vertical_blank_start();
}

void WINAPI D3DDevice_BlockUntilVerticalBlank(void)
{
	unsigned long count;

	vertical_blank_start();
	pthread_mutex_lock(&vertical_blank_lock);
	count = vertical_blank_count;
	while (vertical_blank_count == count)
		pthread_cond_wait(&vertical_blank_condition, &vertical_blank_lock);
	pthread_mutex_unlock(&vertical_blank_lock);
}


/* ---------- render targets */

static void surface_dimensions(const D3DSurface *surface, unsigned long *width, unsigned long *height, BOOL *depth)
{
	struct xgpu_texture_description description;
	DWORD format;

	xgpu_texture_describe(surface->Format, surface->Size, &description);
	*width = description.width;
	*height = description.height;
	format = description.format;
	*depth = format == D3DFMT_D24S8 || format == D3DFMT_F24S8 || format == D3DFMT_D16 || format == D3DFMT_F16 ||
		format == D3DFMT_LIN_D24S8 || format == D3DFMT_LIN_F24S8 || format == D3DFMT_LIN_D16 || format == D3DFMT_LIN_F16;
}

/* the shadow maps: the game's only R5G6B5 render targets, 128x128
(rasterizer_xbox.c) */
static BOOL surface_is_shadow_map(const D3DSurface *surface)
{
	struct xgpu_texture_description description;

	xgpu_texture_describe(surface->Format, surface->Size, &description);
	return description.format == D3DFMT_R5G6B5 && description.width == SHADOW_MAP_SIZE &&
		description.height == SHADOW_MAP_SIZE;
}

/* the secondary render target, the size of the Xbox's (rasterizer_xbox.c) */
#define SECONDARY_TARGET_WIDTH 320
#define SECONDARY_TARGET_HEIGHT 240

/* ---------- half resolution (display.half_res_convolution)

Opt-in: the screen convolution's passes between its first and its last
(rasterizer_xbox_screen_effect.c) are drawn into a pair of targets at half
the scene's resolution each way, a quarter of its pixels. They keep the
screen's size in the game's units (its width and 480 lines), so the game's
viewports, quads and texture coordinates are unchanged; only their pixels
per unit are halved (render_target_get). The first pass reads the screen
at full resolution, the last writes it, sampling the half-resolution
result with bilinear filtering: a blur loses little.

halfres_scene_scale is the one place that knows the scene's resolution
(the screen's targets' pixels per unit, screen_scale): a renderer that
draws the scene at another size than the display (dynamic resolution)
changes it there. HALO_HALF_RES_CYCLE=N (diagnostic) turns the feature on
and off in alternate blocks of N frames, whatever the settings, to compare
the two in one run (tools/halfres_compare.py). */

#define HALF_RES_TARGETS 2

static struct
{
	D3DSurface surface;
	D3DBaseTexture texture;
} half_res_targets[HALF_RES_TARGETS];

/* the setting (config_changes when read), and the diagnostic cycle's
frames a block (0: none) */
static int half_res_convolution_setting = -1;
static unsigned long half_res_read_at;
static long half_res_cycle = -1;

/* the scene's pixels per unit of the game's screen, each way: the
adapter for a renderer whose scene is drawn at another size than the
screen's targets (dynamic resolution) */
static void halfres_scene_scale(float scale[2])
{
	/* (dynamic resolution's scenes are scaled up into the back buffer at
	their end, before the HUD's screen effect, whose convolution this is:
	it reads and writes the screen at the screen's scale, whatever the
	scene's fraction was) */
	scale[0] = screen_scale[0];
	scale[1] = screen_scale[1];
}

/* the half-resolution targets' pixels per unit */
static void half_res_scale(float scale[2])
{
	halfres_scene_scale(scale);
	scale[0] *= 0.5f;
	scale[1] *= 0.5f;
}

/* which half-resolution target a surface or texture's data is: -1 none */
static int half_res_index(DWORD data)
{
	int index;

	for (index = 0; index < HALF_RES_TARGETS; index++)
	{
		if (data && half_res_targets[index].surface.Data == data)
			return index;
	}
	return -1;
}

/* the diagnostic cycle's block this frame is in (0, 1, ...), of count;
-1 without HALO_HALF_RES_CYCLE */
static long half_res_cycle_block(long count)
{
	if (half_res_cycle < 0)
	{
		const char *setting = getenv("HALO_HALF_RES_CYCLE");

		half_res_cycle = setting && atol(setting) > 0 ? atol(setting) : 0;
		if (half_res_cycle)
			platform_log("half resolution: HALO_HALF_RES_CYCLE=%ld (diagnostic): on and off in blocks of %ld frames",
				half_res_cycle, half_res_cycle);
	}
	if (!half_res_cycle)
		return -1;
	return (long)((device.frame / (unsigned long)half_res_cycle) % (unsigned long)count);
}

/* the convolution's middle passes at half resolution, this frame: the
setting, or the diagnostic cycle (even blocks off, odd on). (No radius
below which it is left at full resolution: a scope's warp and a blur of a
radius of half an Xbox pixel to ten measured 44-51 dB PSNR against full
resolution at 2160 lines, port/tvos/PERFORMANCE.md.) */
int halo_half_res_convolution(void)
{
	long block;

	if (half_res_convolution_setting < 0 || half_res_read_at != config_changes())
	{
		half_res_read_at = config_changes();
		half_res_convolution_setting = config_boolean("display.half_res_convolution") != 0;
		platform_log("half resolution: screen convolution %s", half_res_convolution_setting ? "on" : "off");
	}
	block = half_res_cycle_block(2);
	if (block >= 0)
		return block == 1;
	return half_res_convolution_setting;
}

/* a half-resolution target's surface (to draw into) and texture (to
sample), index 0 or 1: the screen's size in the game's units */
void halo_half_res_target(int index, D3DSurface **surface, D3DBaseTexture **texture)
{
	if (!half_res_targets[index].surface.Data)
	{
		void *memory = platform_contiguous_alloc(D3DTEXTURE_ALIGNMENT, D3DTEXTURE_ALIGNMENT,
			PLATFORM_ANY_PHYSICAL_ADDRESS, PAGE_READWRITE);

		/* (the address names the target; its memory holds no pixels) */
		half_res_targets[index].surface.Common = D3DCOMMON_TYPE_SURFACE | 1;
		half_res_targets[index].surface.Data = memory ? PLATFORM_VIRTUAL_TO_PHYSICAL(memory) : 0;
		half_res_targets[index].texture.Common = D3DCOMMON_TYPE_TEXTURE | 1;
		half_res_targets[index].texture.Data = half_res_targets[index].surface.Data;
	}
	/* the back buffer's format and size, as the screen's copy target's */
	half_res_targets[index].surface.Format = device.back_buffer.Format;
	half_res_targets[index].surface.Size = device.back_buffer.Size;
	half_res_targets[index].texture.Format = device.back_buffer.Format | (1 << D3DFORMAT_MIPMAP_SHIFT);
	half_res_targets[index].texture.Size = device.back_buffer.Size;
	*surface = &half_res_targets[index].surface;
	*texture = &half_res_targets[index].texture;
}

/* ---------- dynamic resolution (display.dynamic_resolution)

Each player view's 3D scene (render.c: halo_screen_scene_begin to
halo_screen_anti_alias) can be drawn at a fraction of the screen's
resolution, which the host's controller picks once a frame
(port/macos/host/host_dynres.h). While the scene is drawn at less than the
whole, the back buffer's colour target is the scene target instead: a
target of the back buffer's size, drawn into in its top left at the
fraction's scale (target_scale: viewports, scissors and clears scaled, as
resolution_scaling scales them), so nothing is allocated again when the
fraction changes. Everything that reads the back buffer as a texture in
the scene (active camouflage's and the sun glow's copies) reads the scene
target, its texture coordinates in the game's units scaled to the scene's
part of it (bind_textures). The depth buffer is the back buffer's own, at
the screen's size: the scene uses its top left part, and it is cleared by
the game at each view's start (rasterizer_window_begin), its depth and
stencil read by nothing after the scene (the HUD's draws test neither).
At the scene's end the host scales the view's rectangle of the scene
target up into the back buffer (host_metal_scene_upscale); the HUD, menus
and text are then drawn over it at the screen's resolution. At a fraction
of 1 nothing of this happens: the frames are drawn as before. */

static struct
{
	/* the fraction the frame's scenes are drawn at (1: the whole), asked
	of the host at the frame's first scene; -1 until it is asked */
	float frame_fraction;
	/* a scene being drawn at less than the whole, and its view's
	rectangle in the game's units */
	BOOL active;
	float fraction;
	short view[4];
	/* the scene target (a texture of the back buffer's size, made again
	only when that changes) and its size */
	GLuint texture;
	unsigned long width, height;
	/* display.dynamic_resolution's sharpening */
	int sharpen;
	BOOL configured;
	/* the scene's first clear is still to come (D3DDevice_Clear) */
	BOOL first_clear;
} scene;

/* the scene target's entry: the back buffer's own, with the scene's
texture and scale (render_target_get gives it while a scene is active) */
static struct render_target_entry scene_entry;

/* the targets render_target_get found last, by what it found them from:
each draw asks again for the same two (bind_targets) */
#define RECENT_RENDER_TARGET_COUNT 4

static struct
{
	DWORD data, format, size;
	long screen_width;
	float scale[2];
	struct render_target_entry *entry;
} recent_render_targets[RECENT_RENDER_TARGET_COUNT];
static unsigned long recent_render_target_next;

static struct render_target_entry *render_target_remember(const D3DSurface *surface, long screen,
	struct render_target_entry *entry)
{
	unsigned long slot = recent_render_target_next++ % RECENT_RENDER_TARGET_COUNT;

	recent_render_targets[slot].data = surface->Data;
	recent_render_targets[slot].format = surface->Format;
	recent_render_targets[slot].size = surface->Size;
	recent_render_targets[slot].screen_width = screen;
	recent_render_targets[slot].scale[0] = screen_scale[0];
	recent_render_targets[slot].scale[1] = screen_scale[1];
	recent_render_targets[slot].entry = entry;
	return entry;
}

static struct render_target_entry *render_target_get(const D3DSurface *surface)
{
	struct render_target_entry *entry;
	unsigned long width, height, slot;
	long screen;
	BOOL depth;

	if (!surface || !surface->Data)
		return NULL;
	float scale[2] = { 1.0f, 1.0f };
	enum render_target_kind kind = _render_target_plain;

	/* (a scene drawn at less than the whole: the back buffer's colour is
	the scene target) */
	if (scene.active && surface->Data == device.back_buffer.Data)
		return &scene_entry;

	/* (an entry lives until its scale no longer fits: render_targets_evict) */
	screen = halo_screen_width();
	for (slot = 0; slot < RECENT_RENDER_TARGET_COUNT; slot++)
	{
		if (recent_render_targets[slot].entry && recent_render_targets[slot].data == surface->Data &&
			recent_render_targets[slot].format == surface->Format && recent_render_targets[slot].size == surface->Size &&
			recent_render_targets[slot].screen_width == screen && recent_render_targets[slot].scale[0] == screen_scale[0] &&
			recent_render_targets[slot].scale[1] == screen_scale[1])
		{
			return recent_render_targets[slot].entry;
		}
	}
	surface_dimensions(surface, &width, &height, &depth);
	/* a half-resolution target: half the scene's scale (half_res_scale),
	not remembered above, whose key is the screen's scale alone */
	if (half_res_index(surface->Data) >= 0)
	{
		half_res_scale(scale);
		kind = _render_target_half;
		for (entry = *render_target_bucket(surface->Data); entry; entry = entry->next_in_bucket)
		{
			if (entry->target.data == surface->Data && entry->target.width == width &&
				entry->target.height == height && !entry->target.depth &&
				entry->target.scale[0] == scale[0] && entry->target.scale[1] == scale[1])
			{
				return entry;
			}
		}
		goto make;
	}
	/* the screen's targets are drawn at the screen's scale, and so is the
	secondary target (the reflections' and active camouflage's: its size is
	rasterizer_xbox.c's), which keeps the game's 320x240 units, that its
	shaders' constants are in, and gets the screen's pixels a unit; the
	shadow maps at display.shadow_resolution's (halo_shadow_map_scale) */
	if ((width == (unsigned long)halo_screen_width() && height == SCREEN_HEIGHT) ||
		(!depth && width == SECONDARY_TARGET_WIDTH && height == SECONDARY_TARGET_HEIGHT))
	{
		scale[0] = screen_scale[0];
		scale[1] = screen_scale[1];
		kind = _render_target_screen;
	}
	else if (!depth && surface_is_shadow_map(surface))
	{
		scale[0] = (float)halo_shadow_map_scale();
		scale[1] = scale[0];
		kind = _render_target_shadow_map;
	}
	for (entry = *render_target_bucket(surface->Data); entry; entry = entry->next_in_bucket)
	{
		if (entry->target.data == surface->Data && entry->target.width == width &&
			entry->target.height == height && entry->target.depth == depth &&
			entry->target.scale[0] == scale[0] && entry->target.scale[1] == scale[1])
		{
			return render_target_remember(surface, screen, entry);
		}
	}
make:
	entry = calloc(1, sizeof(*entry));
	entry->target.data = surface->Data;
	entry->target.width = width;
	entry->target.height = height;
	entry->target.depth = depth;
	entry->target.scale[0] = scale[0];
	entry->target.scale[1] = scale[1];
	entry->kind = kind;
	entry->target.gl_width = (unsigned long)(width * scale[0] + 0.5f);
	entry->target.gl_height = (unsigned long)(height * scale[1] + 0.5f);
	entry->target.texture = host_metal_texture_new();
	host_metal_texture_define(entry->target.texture, _metal_texture_2d,
		depth ? METAL_FORMAT_DEPTH32_STENCIL8 : METAL_FORMAT_BGRA8, (unsigned int)entry->target.gl_width,
		(unsigned int)entry->target.gl_height, 1, 1, depth ? _metal_usage_depth_target : _metal_usage_color_target,
		METAL_SWIZZLE_IDENTITY);
	entry->screen_buffer = surface->Data == (depth ? device.depth_buffer.Data : device.back_buffer.Data);
	entry->next = render_targets;
	render_targets = entry;
	entry->next_in_bucket = *render_target_bucket(entry->target.data);
	*render_target_bucket(entry->target.data) = entry;
	return entry;
}

struct xgpu_render_target *xgpu_render_target_find(unsigned long data)
{
	struct render_target_entry *entry, *best = NULL;

	/* (the back buffer read in a scene drawn at less than the whole: the
	scene target, what the scene has drawn so far) */
	if (scene.active && data == device.back_buffer.Data)
		return &scene_entry.target;

	for (entry = *render_target_bucket(data); entry; entry = entry->next_in_bucket)
	{
		if (entry->target.data == data && !entry->target.depth && (!best || entry->last_rendered > best->last_rendered))
			best = entry;
	}
	return best ? &best->target : NULL;
}



/* counts draws and clears into render targets (xgpu_render_target.written) */
static unsigned long render_target_write_serial;

/* the pixels per unit of the bound targets (render_target_get) */
static float target_scale[2] = { 1.0f, 1.0f };

/* the pixel edge of a coordinate in a target's units, at its scale:
floorf's, without its call (on 32-bit x86 it saves and restores the FPU's
rounding, several times a draw) */
static GLint scaled_pixel(float coordinate, float scale)
{
	float value = coordinate * scale + 0.5f;
	GLint pixel = (GLint)value;

	/* (the conversion is toward zero: a negative value with a fraction
	rounds down one more) */
	if ((float)pixel > value)
		pixel--;
	return pixel;
}

/* ... in the bound targets' units */
static GLint target_pixel(float coordinate, int axis)
{
	return scaled_pixel(coordinate, target_scale[axis]);
}

/* the host's targets, the current ones; FALSE if there is nothing to draw
into */
static BOOL bind_targets(BOOL *has_depth)
{
	struct render_target_entry *color = render_target_get(device.render_target);
	struct render_target_entry *depth = render_target_get(device.depth_stencil);

	if (depth && !depth->target.depth)
		depth = NULL;
	if (color && color->target.depth)
		color = NULL;
	if (!color && !depth)
		return FALSE;
	if (color)
	{
		color->last_rendered = device.frame + 1;
		color->target.written = ++render_target_write_serial;
	}
	/* viewports and clears are in the targets' units (render_target_get) */
	target_scale[0] = color ? color->target.scale[0] : depth->target.scale[0];
	target_scale[1] = color ? color->target.scale[1] : depth->target.scale[1];
	host_metal_set_targets(color ? color->target.texture : 0, depth ? depth->target.texture : 0);
	*has_depth = depth != NULL;
	return TRUE;
}

/* ---------- device creation */

static void metal_initialize(void)
{
	int index;

	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
		device.attributes[index][3] = 1.0f;
	memory_watch_initialize();
	debug_settings.skip_vertex_shaders = config_string("debug.gpu_skip_vertex_shaders");
	debug_settings.dump_shaders = *config_string("debug.gpu_dump_shaders") ?
		config_string("debug.gpu_dump_shaders") : NULL;
	debug_settings.statistics = config_boolean("debug.gpu_stats");
	/* (Apple silicon's: 16384 pixels a side; no multisampling yet) */
	anti_aliasing_maximum_samples = 0;
	maximum_target_size = 16384;
	device.ready = TRUE;
	if (anti_aliasing_value < 0)
		anti_aliasing_read();
	else
		anti_aliasing_prepare();
}

/* what display.anti_aliasing's value needs: supersampling is the screen's
scale (screen_mode_choose); the passes and multisampling are not drawn with
Metal yet */
static void anti_aliasing_prepare(void)
{
	int mode;

	if (!device.ready || anti_aliasing_value < 0)
		return;
	mode = anti_aliasing_values[anti_aliasing_value].mode;
	anti_aliasing_samples = 0;
	if (mode == _anti_aliasing_fxaa || mode == _anti_aliasing_smaa || mode == _anti_aliasing_msaa)
	{
		platform_log("anti-aliasing: %s is not drawn with Metal yet, so the 3D view is not antialiased",
			anti_aliasing_values[anti_aliasing_value].name);
	}
}

Direct3D *WINAPI Direct3DCreate8(UINT sdk_version)
{
	(void)sdk_version;
	return (Direct3D *)1;
}

void WINAPI Direct3D_SetPushBufferSize(DWORD push_buffer_size, DWORD segment_count)
{
	(void)push_buffer_size;
	(void)segment_count;
}

/* the vertex constants' serial: changed whenever a register does, so that
a draw sends the host all 192 (3 KB, inline with the draw) only when one
has changed since the draw before it (prepare_draw); never 0 */
static unsigned int constants_serial = 1;

static void constants_store(unsigned long first, const void *data, unsigned long count)
{
	const float (*values)[4] = data;
	unsigned long index;

	for (index = 0; index < count; index++)
	{
		if (memcmp(device.constants[first + index], values[index], sizeof(device.constants[0])))
		{
			memcpy(device.constants[first + index], values[index], sizeof(device.constants[0]));
			if (!++constants_serial)
				constants_serial = 1;
		}
	}
}

static void viewport_update_constants(void)
{
	/* Direct3D's reserved constants c[-38] and c[-37] map clip space to
	the screen; zscale is the depth buffer's range */
	float zscale = 16777215.0f;
	unsigned long width, height;
	BOOL depth;

	if (device.depth_stencil)
	{
		struct xgpu_texture_description description;

		xgpu_texture_describe(device.depth_stencil->Format, device.depth_stencil->Size, &description);
		if (description.format == D3DFMT_D16 || description.format == D3DFMT_LIN_D16 ||
			description.format == D3DFMT_F16 || description.format == D3DFMT_LIN_F16)
		{
			zscale = 65535.0f;
		}
	}
	(void)width; (void)height; (void)depth;
	device.viewport_scale[0] = device.viewport.Width * 0.5f;
	device.viewport_scale[1] = -(float)device.viewport.Height * 0.5f;
	device.viewport_scale[2] = zscale * (device.viewport.MaxZ - device.viewport.MinZ);
	device.viewport_scale[3] = 0.0f;
	device.viewport_offset[0] = device.viewport.X + device.viewport.Width * 0.5f;
	device.viewport_offset[1] = device.viewport.Y + device.viewport.Height * 0.5f;
	device.viewport_offset[2] = zscale * device.viewport.MinZ;
	device.viewport_offset[3] = 0.0f;
	if (!(device.shader_constant_mode & D3DSCM_NORESERVEDCONSTANTS))
	{
		constants_store(XGPU_VERTEX_CONSTANT_BIAS - 38, device.viewport_scale, 1);
		constants_store(XGPU_VERTEX_CONSTANT_BIAS - 37, device.viewport_offset, 1);
	}
}

HRESULT WINAPI Direct3D_CreateDevice(UINT adapter, D3DDEVTYPE device_type, void *unused, DWORD behavior_flags,
	D3DPRESENT_PARAMETERS *presentation_parameters, D3DDevice **returned_device)
{
	unsigned long width, height;
	int index;

	(void)adapter;
	(void)device_type;
	(void)unused;
	(void)behavior_flags;
	if (!device.created)
	{
		memset(&device, 0, sizeof(device));
		if (presentation_parameters)
			device.presentation = *presentation_parameters;
		width = device.presentation.BackBufferWidth ? device.presentation.BackBufferWidth : 640;
		height = device.presentation.BackBufferHeight ? device.presentation.BackBufferHeight : 480;
		/* room for the widest screen, which F11 can switch to (the screen's
		width, above) */
		d3d8_surface_initialize(&device.back_buffer, D3DFMT_LIN_A8R8G8B8, SCREEN_MAXIMUM_WIDTH, height);
		d3d8_surface_initialize(&device.depth_buffer, D3DFMT_LIN_D24S8, SCREEN_MAXIMUM_WIDTH, height);
		d3d8_surface_resize(&device.back_buffer, D3DFMT_LIN_A8R8G8B8, width, height);
		d3d8_surface_resize(&device.depth_buffer, D3DFMT_LIN_D24S8, width, height);
		device.render_target = &device.back_buffer;
		device.depth_stencil = &device.depth_buffer;
		for (index = 0; index < D3DTS_MAX; index++)
		{
			device.transforms[index]._11 = 1.0f;
			device.transforms[index]._22 = 1.0f;
			device.transforms[index]._33 = 1.0f;
			device.transforms[index]._44 = 1.0f;
		}
		device.viewport.Width = width;
		device.viewport.Height = height;
		device.viewport.MaxZ = 1.0f;
		device.next_vertex_shader_id = 1;
		D3D__RenderState[D3DRS_ZENABLE] = TRUE;
		D3D__RenderState[D3DRS_ZWRITEENABLE] = TRUE;
		D3D__RenderState[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
		D3D__RenderState[D3DRS_COLORWRITEENABLE] = D3DCOLORWRITEENABLE_ALL;
		D3D__RenderState[D3DRS_SRCBLEND] = D3DBLEND_ONE;
		D3D__RenderState[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
		D3D__RenderState[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
		D3D__RenderState[D3DRS_CULLMODE] = D3DCULL_CCW;
		D3D__RenderState[D3DRS_FRONTFACE] = D3DFRONT_CW;
		D3D__RenderState[D3DRS_FILLMODE] = D3DFILL_SOLID;
		D3D__RenderState[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
		D3D__RenderState[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
		D3D__RenderState[D3DRS_STENCILMASK] = 0xff;
		D3D__RenderState[D3DRS_STENCILWRITEMASK] = 0xff;
		D3D__RenderState[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
		D3D__RenderState[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;
		D3D__RenderState[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
		for (index = 0; index < D3DTSS_MAXSTAGES; index++)
		{
			D3D__TextureState[index][D3DTSS_ADDRESSU] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_ADDRESSW] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_MAGFILTER] = D3DTEXF_POINT;
			D3D__TextureState[index][D3DTSS_MINFILTER] = D3DTEXF_POINT;
			D3D__TextureState[index][D3DTSS_MAXANISOTROPY] = 1;
		}
		viewport_update_constants();

		if (!config_boolean("debug.null_renderer") && platform_video_initialize(width, height))
			metal_initialize();
		else
			platform_log("Direct3D: running without a window (nothing is displayed)");
		device.created = TRUE;
	}
	*returned_device = device_pointer();
	return S_OK;
}

/* ---------- the menus' pointer */

/* a point in the window, as SDL reports it, in the menus' coordinates: the
inverse of the letterboxed display blit at presentation, the screen's
width and the menus' centering (halo_screen_ui_offset); or, not centered,
the screen's (the game's drawing: the scoreboard's); x and y are -1 if there
is no back buffer or window yet */
static void ui_point_from_window_on(float window_x, float window_y, int centered, short *x, short *y)
{
	struct render_target_entry *back_buffer = render_target_get(&device.back_buffer);
	int window_width, window_height, pixel_width, pixel_height, width, height, left, top;
	float screen_x, screen_y;

	*x = *y = -1;
	if (!back_buffer)
		return;
	platform_video_window_size(&window_width, &window_height);
	platform_video_drawable_size(&pixel_width, &pixel_height);
	if (window_width <= 0 || window_height <= 0)
		return;
	width = pixel_width;
	height = (int)((long)pixel_width * back_buffer->target.gl_height / back_buffer->target.gl_width);
	if (height > pixel_height)
	{
		height = pixel_height;
		width = (int)((long)pixel_height * back_buffer->target.gl_width / back_buffer->target.gl_height);
	}
	left = (pixel_width - width) / 2;
	top = (pixel_height - height) / 2;
	screen_x = (window_x * pixel_width / window_width - left) * (float)back_buffer->target.width / (float)width;
	screen_y = (window_y * pixel_height / window_height - top) * (float)back_buffer->target.height / (float)height;
	*x = (short)floorf(screen_x - (centered ? (float)(halo_screen_width() - 640) / 2.0f : 0.0f));
	*y = (short)floorf(screen_y);
}

static void ui_point_from_window(float window_x, float window_y, short *x, short *y)
{
	ui_point_from_window_on(window_x, window_y, TRUE, x, y);
}

int halo_scoreboard_pointer_update(int offered, struct halo_ui_pointer *pointer)
{
	struct platform_ui_pointer state;

	memset(pointer, 0, sizeof(*pointer));
	if (!platform_scoreboard_pointer(offered != 0, &state) || !device.ready)
		return 0;
	ui_point_from_window_on(state.x, state.y, FALSE, &pointer->x, &pointer->y);
	ui_point_from_window_on(state.click_x, state.click_y, FALSE, &pointer->click_x, &pointer->click_y);
	pointer->moved = state.moved != FALSE;
	pointer->left_clicks = (unsigned char)(state.left_clicks < 255 ? state.left_clicks : 255);
	return 1;
}

int halo_ui_pointer_update(int menus_active, struct halo_ui_pointer *pointer)
{
	struct platform_ui_pointer state;

	platform_menus_set_active(menus_active != 0);
	platform_ui_pointer_set_active(menus_active != 0);
	if (!menus_active || !device.ready || !platform_ui_pointer_read(&state))
		return 0;
	memset(pointer, 0, sizeof(*pointer));
	ui_point_from_window(state.x, state.y, &pointer->x, &pointer->y);
	ui_point_from_window(state.click_x, state.click_y, &pointer->click_x, &pointer->click_y);
	pointer->moved = state.moved != FALSE;
	pointer->left_clicks = (unsigned char)(state.left_clicks < 255 ? state.left_clicks : 255);
	pointer->right_clicks = (unsigned char)(state.right_clicks < 255 ? state.right_clicks : 255);
	pointer->wheel_steps = (signed char)(state.wheel_steps < -8 ? -8 : state.wheel_steps > 8 ? 8 : state.wheel_steps);
	pointer->touch = state.touch != FALSE;
	pointer->downs = (unsigned char)(state.downs < 255 ? state.downs : 255);
	ui_point_from_window(state.down_x, state.down_y, &pointer->down_x, &pointer->down_y);
	return 1;
}

/* takes up the display's shape and resolution, or the window's, if they
have changed; between frames, since the game's layout and the targets must
agree for a whole frame. Returns the width the game draws. */
long halo_screen_commit(void)
{
	long width;
	float scale[2];

	/* (display.anti_aliasing, as Settings or config.toml has it now:
	supersampling changes the scale below) */
	anti_aliasing_read();
	/* display.shadow_resolution, if it has changed: the maps' entries at
	the old scale are freed (render_targets_evict), new ones made at the new */
	if (shadow_scale && shadow_scale_read_at != config_changes())
	{
		long shadow = shadow_scale_choose();

		shadow_scale_read_at = config_changes();
		if (shadow != shadow_scale)
		{
			platform_log("shadow maps: %ldx%ld", SHADOW_MAP_SIZE * shadow, SHADOW_MAP_SIZE * shadow);
			shadow_scale = shadow;
			render_targets_evict();
		}
	}
	if (!screen_width)
		return halo_screen_width();
	screen_mode_choose(&width, scale);
	if (width != screen_width || scale[0] != screen_scale[0] || scale[1] != screen_scale[1])
	{
		platform_log("screen: %ldx%d drawn at %.0fx%.0f", width, SCREEN_HEIGHT,
			width * scale[0], SCREEN_HEIGHT * scale[1]);
		screen_width = width;
		screen_scale[0] = scale[0];
		screen_scale[1] = scale[1];
		if (device.created)
		{
			device.presentation.BackBufferWidth = (UINT)width;
			d3d8_surface_resize(&device.back_buffer, D3DFMT_LIN_A8R8G8B8, (unsigned long)width, SCREEN_HEIGHT);
			d3d8_surface_resize(&device.depth_buffer, D3DFMT_LIN_D24S8, (unsigned long)width, SCREEN_HEIGHT);
		}
		/* (the entries of the old size and scale, which nothing finds now) */
		render_targets_evict();
	}
	return screen_width;
}

ULONG WINAPI D3DDevice_Release(void)
{
	return 1;
}

void WINAPI D3DDevice_GetDeviceCaps(D3DCAPS8 *caps)
{
	memset(caps, 0, sizeof(*caps));
	caps->DeviceType = D3DDEVTYPE_HAL;
	caps->MaxTextureWidth = 4096;
	caps->MaxTextureHeight = 4096;
	caps->MaxVolumeExtent = 512;
	caps->MaxTextureRepeat = 8192;
	caps->MaxTextureAspectRatio = 4096;
	caps->MaxAnisotropy = 4;
	caps->MaxTextureBlendStages = 4;
	caps->MaxSimultaneousTextures = 4;
	caps->MaxActiveLights = 8;
	caps->MaxVertexBlendMatrices = 4;
	caps->MaxPointSize = 64.0f;
	caps->MaxPrimitiveCount = 0xfffff;
	caps->MaxVertexIndex = 0xffff;
	caps->MaxStreams = 16;
	caps->MaxStreamStride = 255;
	caps->VertexShaderVersion = D3DVS_VERSION(1, 1);
	caps->MaxVertexShaderConst = 192;
	caps->PixelShaderVersion = D3DPS_VERSION(1, 1);
	caps->MaxPixelShaderValue = 1.0f;
}

void WINAPI D3DDevice_GetBackBuffer(INT back_buffer, D3DBACKBUFFER_TYPE type, D3DSurface **result)
{
	(void)back_buffer;
	(void)type;
	/* like Direct3D, the caller gets a reference it must release */
	device.back_buffer.Common++;
	*result = &device.back_buffer;
}

HRESULT WINAPI D3DDevice_GetDepthStencilSurface(D3DSurface **result)
{
	*result = device.depth_stencil;
	if (!*result)
		return D3DERR_NOTFOUND;
	(*result)->Common++;
	return S_OK;
}

static BOOL trace_frame(void);

void WINAPI D3DDevice_SetRenderTarget(D3DSurface *render_target, D3DSurface *depth_stencil)
{
	if (trace_frame())
		platform_log("set render target %08lx depth %08lx", render_target ? (unsigned long)render_target->Data : 0,
			depth_stencil ? (unsigned long)depth_stencil->Data : 0);
	stats.target_changes++;
	if (render_target)
		device.render_target = render_target;
	device.depth_stencil = depth_stencil;
	/* like Direct3D, reset the viewport to the whole new target */
	if (device.render_target)
	{
		unsigned long width, height;
		BOOL depth;

		surface_dimensions(device.render_target, &width, &height, &depth);
		device.viewport.X = 0;
		device.viewport.Y = 0;
		device.viewport.Width = width;
		device.viewport.Height = height;
		device.viewport.MinZ = 0.0f;
		device.viewport.MaxZ = 1.0f;
	}
	viewport_update_constants();
}

void WINAPI D3DDevice_SetViewport(CONST D3DVIEWPORT8 *viewport)
{
	device.viewport = *viewport;
	viewport_update_constants();
}

void WINAPI D3DDevice_SetTransform(D3DTRANSFORMSTATETYPE state, CONST D3DMATRIX *matrix)
{
	if ((unsigned long)state < D3DTS_MAX)
		device.transforms[state] = *matrix;
}

void WINAPI D3DDevice_GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX *matrix)
{
	if ((unsigned long)state < D3DTS_MAX)
		*matrix = device.transforms[state];
}

void WINAPI D3DDevice_SetFlickerFilter(DWORD filter) { (void)filter; }
void WINAPI D3DDevice_SetSoftDisplayFilter(BOOL enable) { (void)enable; }

void WINAPI D3DDevice_SetShaderConstantMode(D3DSHADERCONSTANTMODE mode)
{
	device.shader_constant_mode = mode;
	viewport_update_constants();
}

/* ---------- GPU synchronisation: the host's frames keep their own order */

BOOL WINAPI D3DDevice_IsBusy(void)
{
	return FALSE;
}

void WINAPI D3DDevice_KickPushBuffer(void)
{
}

void WINAPI D3DDevice_InsertCallback(D3DCALLBACKTYPE type, D3DCALLBACK callback, DWORD context)
{
	(void)type;
	/* the "GPU" reaches the callback immediately */
	if (callback)
		callback(context);
}

/* ---------- visibility (occlusion) tests

A test's draws count their samples (the host's counters), which the GPU
has once it has finished the frame: the game reads a result a frame or more
later, and until then each index has its earlier count. */

void WINAPI D3DDevice_BeginVisibilityTest(void)
{
	if (!device.ready || device.visibility_test_active)
		return;
	device.visibility_test_active = TRUE;
	device.visibility_test = host_metal_visibility_begin();
}

HRESULT WINAPI D3DDevice_EndVisibilityTest(DWORD index)
{
	if (!device.ready || !device.visibility_test_active)
		return S_OK;
	device.visibility_test_active = FALSE;
	index %= VISIBILITY_TEST_SLOTS;
	host_metal_visibility_end(device.visibility_test, (unsigned int)index);
	/* the target's pixels to a game pixel: the result is a count of the
	game's pixels (visibility_unscaled), which the game divides by its own
	test's area (lens flares, rasterizer_lights.c), a split-screen window's
	or the screen's alike */
	device.query_area[index] = target_scale[0] * target_scale[1];
	device.query_pending[index] = TRUE;
	return S_OK;
}

/* a count of pixels in the game's pixels */
static unsigned int visibility_unscaled(unsigned int samples, DWORD index)
{
	float area = device.query_area[index];

	return area > 1.0f ? (unsigned int)(samples / area + 0.5f) : samples;
}

HRESULT WINAPI D3DDevice_GetVisibilityTestResult(DWORD index, UINT *result, ULONGLONG *time_stamp)
{
	unsigned int samples = 0;

	if (time_stamp)
		*time_stamp = 0;
	index %= VISIBILITY_TEST_SLOTS;
	if (!device.ready || !device.query_pending[index] || !host_metal_visibility_result((unsigned int)index, &samples))
	{
		if (result)
			*result = 0;
		return S_OK;
	}
	if (result)
		*result = visibility_unscaled(samples, index);
	return S_OK;
}

/* ---------- render and texture stage state */

void D3DFASTCALL D3DDevice_SetRenderState_Simple(DWORD method, DWORD value)
{
	/* callers also store the value in D3D__RenderState themselves */
	(void)method;
	(void)value;
}

void D3DFASTCALL D3DDevice_SetRenderState_Deferred(D3DRENDERSTATETYPE state, DWORD value)
{
	if ((unsigned long)state < D3DRS_MAX)
		D3D__RenderState[state] = value;
}

void WINAPI D3DDevice_SetRenderState_ZBias(DWORD value);

void WINAPI D3DDevice_SetRenderStateNotInline(D3DRENDERSTATETYPE state, DWORD value)
{
	if (state == D3DRS_ZBIAS)
		D3DDevice_SetRenderState_ZBias(value);
	else if ((unsigned long)state < D3DRS_MAX)
		D3D__RenderState[state] = value;
}

/* As the Xbox's D3D8 does it: a z bias is a polygon offset of -bias depth
units plus -bias/4 times the polygon's depth slope, enabled for every fill
mode. Without the slope term, decals (biased by 8) fight with the surface
under them wherever it is seen at an angle. */
void WINAPI D3DDevice_SetRenderState_ZBias(DWORD value)
{
	float offset = -(float)value;
	float slope = offset * 0.25f;
	DWORD enable = value != 0;

	memcpy(&D3D__RenderState[D3DRS_POLYGONOFFSETZSLOPESCALE], &slope, sizeof(slope));
	memcpy(&D3D__RenderState[D3DRS_POLYGONOFFSETZOFFSET], &offset, sizeof(offset));
	D3D__RenderState[D3DRS_POINTOFFSETENABLE] = enable;
	D3D__RenderState[D3DRS_WIREFRAMEOFFSETENABLE] = enable;
	D3D__RenderState[D3DRS_SOLIDOFFSETENABLE] = enable;
	D3D__RenderState[D3DRS_ZBIAS] = value;
}

#define COMPLEX_RENDER_STATE(name, state) \
	void WINAPI D3DDevice_SetRenderState_##name(DWORD value) { D3D__RenderState[state] = value; }

COMPLEX_RENDER_STATE(PSTextureModes, D3DRS_PSTEXTUREMODES)
COMPLEX_RENDER_STATE(VertexBlend, D3DRS_VERTEXBLEND)
COMPLEX_RENDER_STATE(FogColor, D3DRS_FOGCOLOR)
COMPLEX_RENDER_STATE(FillMode, D3DRS_FILLMODE)
COMPLEX_RENDER_STATE(BackFillMode, D3DRS_BACKFILLMODE)
COMPLEX_RENDER_STATE(TwoSidedLighting, D3DRS_TWOSIDEDLIGHTING)
COMPLEX_RENDER_STATE(NormalizeNormals, D3DRS_NORMALIZENORMALS)
COMPLEX_RENDER_STATE(ZEnable, D3DRS_ZENABLE)
COMPLEX_RENDER_STATE(StencilEnable, D3DRS_STENCILENABLE)
COMPLEX_RENDER_STATE(StencilFail, D3DRS_STENCILFAIL)
COMPLEX_RENDER_STATE(FrontFace, D3DRS_FRONTFACE)
COMPLEX_RENDER_STATE(CullMode, D3DRS_CULLMODE)
COMPLEX_RENDER_STATE(TextureFactor, D3DRS_TEXTUREFACTOR)
COMPLEX_RENDER_STATE(LogicOp, D3DRS_LOGICOP)
COMPLEX_RENDER_STATE(EdgeAntiAlias, D3DRS_EDGEANTIALIAS)
COMPLEX_RENDER_STATE(MultiSampleAntiAlias, D3DRS_MULTISAMPLEANTIALIAS)
COMPLEX_RENDER_STATE(MultiSampleMask, D3DRS_MULTISAMPLEMASK)
COMPLEX_RENDER_STATE(MultiSampleType, D3DRS_MULTISAMPLETYPE)
COMPLEX_RENDER_STATE(ShadowFunc, D3DRS_SHADOWFUNC)
COMPLEX_RENDER_STATE(LineWidth, D3DRS_LINEWIDTH)
COMPLEX_RENDER_STATE(Dxt1NoiseEnable, D3DRS_DXT1NOISEENABLE)
COMPLEX_RENDER_STATE(YuvEnable, D3DRS_YUVENABLE)
COMPLEX_RENDER_STATE(OcclusionCullEnable, D3DRS_OCCLUSIONCULLENABLE)
COMPLEX_RENDER_STATE(StencilCullEnable, D3DRS_STENCILCULLENABLE)
COMPLEX_RENDER_STATE(RopZCmpAlwaysRead, D3DRS_ROPZCMPALWAYSREAD)
COMPLEX_RENDER_STATE(RopZRead, D3DRS_ROPZREAD)
COMPLEX_RENDER_STATE(DoNotCullUncompressed, D3DRS_DONOTCULLUNCOMPRESSED)

void D3DFASTCALL D3DDevice_SetTextureState_Deferred(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES && (unsigned long)type < D3DTSS_MAX)
		D3D__TextureState[stage][type] = value;
}

void WINAPI D3DDevice_SetTextureState_TexCoordIndex(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_TEXCOORDINDEX] = value;
}

void WINAPI D3DDevice_SetTextureState_BorderColor(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_BORDERCOLOR] = value;
}

void WINAPI D3DDevice_SetTextureState_ColorKeyColor(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_COLORKEYCOLOR] = value;
}

void WINAPI D3DDevice_SetTextureState_BumpEnv(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES && (unsigned long)type < D3DTSS_MAX)
		D3D__TextureState[stage][type] = value;
}

void WINAPI D3DDevice_SetTexture(DWORD stage, D3DBaseTexture *texture)
{
	if (stage < D3DTSS_MAXSTAGES)
		device.textures[stage] = texture;
}

void WINAPI D3DDevice_SetPalette(DWORD stage, D3DPalette *palette)
{
	if (stage < D3DTSS_MAXSTAGES)
		device.palettes[stage] = palette;
}

void WINAPI D3DDevice_SetPixelShaderProgram(D3DPIXELSHADERDEF *definition)
{
	/* the definition's members are the pixel shader render states */
	if (!definition)
		return;
	memcpy(&D3D__RenderState[D3DRS_PSALPHAINPUTS0], definition->PSAlphaInputs, sizeof(definition->PSAlphaInputs));
	D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSABCD] = definition->PSFinalCombinerInputsABCD;
	D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSEFG] = definition->PSFinalCombinerInputsEFG;
	memcpy(&D3D__RenderState[D3DRS_PSCONSTANT0_0], definition->PSConstant0, sizeof(definition->PSConstant0));
	memcpy(&D3D__RenderState[D3DRS_PSCONSTANT1_0], definition->PSConstant1, sizeof(definition->PSConstant1));
	memcpy(&D3D__RenderState[D3DRS_PSALPHAOUTPUTS0], definition->PSAlphaOutputs, sizeof(definition->PSAlphaOutputs));
	memcpy(&D3D__RenderState[D3DRS_PSRGBINPUTS0], definition->PSRGBInputs, sizeof(definition->PSRGBInputs));
	D3D__RenderState[D3DRS_PSCOMPAREMODE] = definition->PSCompareMode;
	D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT0] = definition->PSFinalCombinerConstant0;
	D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT1] = definition->PSFinalCombinerConstant1;
	memcpy(&D3D__RenderState[D3DRS_PSRGBOUTPUTS0], definition->PSRGBOutputs, sizeof(definition->PSRGBOutputs));
	D3D__RenderState[D3DRS_PSCOMBINERCOUNT] = definition->PSCombinerCount;
	D3D__RenderState[D3DRS_PSTEXTUREMODES] = definition->PSTextureModes;
	D3D__RenderState[D3DRS_PSDOTMAPPING] = definition->PSDotMapping;
	D3D__RenderState[D3DRS_PSINPUTTEXTURE] = definition->PSInputTexture;
}

/* ---------- vertex shaders */

static unsigned long vertex_type_bytes(unsigned long type)
{
	switch (type)
	{
	case D3DVSDT_FLOAT1: return 4;
	case D3DVSDT_FLOAT2: return 8;
	case D3DVSDT_FLOAT3: return 12;
	case D3DVSDT_FLOAT4: return 16;
	case D3DVSDT_D3DCOLOR: return 4;
	case D3DVSDT_SHORT1: return 2;
	case D3DVSDT_SHORT2: return 4;
	case D3DVSDT_SHORT3: return 6;
	case D3DVSDT_SHORT4: return 8;
	case D3DVSDT_NORMSHORT1: return 2;
	case D3DVSDT_NORMSHORT2: return 4;
	case D3DVSDT_NORMSHORT3: return 6;
	case D3DVSDT_NORMSHORT4: return 8;
	case D3DVSDT_NORMPACKED3: return 4;
	case D3DVSDT_PBYTE1: return 1;
	case D3DVSDT_PBYTE2: return 2;
	case D3DVSDT_PBYTE3: return 3;
	case D3DVSDT_PBYTE4: return 4;
	case D3DVSDT_FLOAT2H: return 12;
	default: return 0;
	}
}

static void parse_declaration(struct vertex_shader_object *object, const DWORD *declaration)
{
	unsigned long stream = 0;
	unsigned long offsets[16] = { 0 };

	for (; declaration && *declaration != D3DVSD_END(); declaration++)
	{
		DWORD token = *declaration;
		unsigned long token_type = (token & D3DVSD_TOKENTYPEMASK) >> D3DVSD_TOKENTYPESHIFT;

		switch (token_type)
		{
		case D3DVSD_TOKEN_STREAM:
			stream = token & D3DVSD_STREAMNUMBERMASK;
			break;
		case D3DVSD_TOKEN_STREAMDATA:
			if (token & D3DVSD_DATALOADTYPEMASK)
			{
				/* skip: the count is in dwords, or in bytes with bit 27 */
				unsigned long count = (token & D3DVSD_SKIPCOUNTMASK) >> D3DVSD_SKIPCOUNTSHIFT;

				offsets[stream] += (token & 0x08000000) ? count : count * 4;
			}
			else if (object->element_count < XGPU_VERTEX_ATTRIBUTE_COUNT)
			{
				struct vertex_element *element = &object->elements[object->element_count++];

				element->reg = (unsigned char)(token & D3DVSD_VERTEXREGMASK);
				element->stream = (unsigned char)stream;
				element->type = (unsigned char)((token & D3DVSD_DATATYPEMASK) >> D3DVSD_DATATYPESHIFT);
				element->bytes = (unsigned char)vertex_type_bytes(element->type);
				element->offset = (unsigned short)offsets[stream];
				offsets[stream] += element->bytes;
				if (element->type == D3DVSDT_NORMPACKED3)
					object->packed_mask |= 1UL << element->reg;
			}
			break;
		case D3DVSD_TOKEN_CONSTMEM:
			declaration += ((token & D3DVSD_CONSTCOUNTMASK) >> D3DVSD_CONSTCOUNTSHIFT) * 4;
			break;
		case D3DVSD_TOKEN_EXT:
			declaration += (token & D3DVSD_EXTCOUNTMASK) >> D3DVSD_EXTCOUNTSHIFT;
			break;
		default:
			break;
		}
	}
}

HRESULT WINAPI D3DDevice_CreateVertexShader(CONST DWORD *declaration, CONST DWORD *function, DWORD *handle, DWORD usage)
{
	struct vertex_shader_object *object = calloc(1, sizeof(*object));

	(void)usage;
	if (!object)
		return E_OUTOFMEMORY;
	object->signature = VERTEX_SHADER_SIGNATURE;
	object->id = device.next_vertex_shader_id++;
	if (function)
	{
		/* header: program type in the low word, instruction count in the high */
		object->instruction_count = function[0] >> 16;
		object->instructions = malloc(object->instruction_count * 4 * sizeof(DWORD));
		memcpy(object->instructions, function + 1, object->instruction_count * 4 * sizeof(DWORD));
	}
	parse_declaration(object, declaration);
	/* odd values are FVF codes; programmable shader handles are even */
	*handle = (DWORD)object;
	return S_OK;
}

static struct vertex_shader_object *vertex_shader_from_handle(DWORD handle)
{
	struct vertex_shader_object *object = (struct vertex_shader_object *)handle;

	if (!handle || (handle & 1) || object->signature != VERTEX_SHADER_SIGNATURE)
		return NULL;
	return object;
}

/* the game names its model lighting programs as it creates them
(rasterizer_xbox_vertex_shaders_initialize.c), so that their draws can be lit
for each pixel (display.per_pixel_lighting); one whose lighting is not as
the pixel shader computes it stays lit for each vertex */
void halo_vertex_shader_lighting(unsigned long handle)
{
	struct vertex_shader_object *object = vertex_shader_from_handle((DWORD)handle);

	if (!object || !object->instructions)
		return;
	if (!nv2a_vertex_shader_lighting(object->instructions, object->instruction_count, &object->lighting))
	{
		memset(&object->lighting, 0, sizeof(object->lighting));
		platform_log("GPU: vertex shader %lu is not lit as the pixel shader would light it: lit for each vertex",
			object->id);
	}
}

void WINAPI D3DDevice_DeleteVertexShader(DWORD handle)
{
	/* programs stay cached; the object is small */
	(void)handle;
}

void WINAPI D3DDevice_SetVertexShader(DWORD handle)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	if (object)
	{
		device.vertex_shader = object;
		device.program_address = 0;
		device.program_slots[0] = object;
	}
}

void WINAPI D3DDevice_LoadVertexShader(DWORD handle, DWORD address)
{
	if (address < VERTEX_PROGRAM_SLOTS)
		device.program_slots[address] = vertex_shader_from_handle(handle);
}

void WINAPI D3DDevice_SelectVertexShader(DWORD handle, DWORD address)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	if (object)
		device.vertex_shader = object;
	if (address < VERTEX_PROGRAM_SLOTS)
		device.program_address = address;
}

void WINAPI D3DDevice_GetVertexShaderSize(DWORD handle, UINT *size)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	*size = object ? object->instruction_count : 0;
}

void WINAPI D3DDevice_SetVertexShaderConstant(INT reg, CONST void *constant_data, DWORD constant_count)
{
	long first = reg + XGPU_VERTEX_CONSTANT_BIAS;

	if (first < 0 || first >= XGPU_VERTEX_CONSTANT_COUNT)
		return;
	if (first + (long)constant_count > XGPU_VERTEX_CONSTANT_COUNT)
		constant_count = XGPU_VERTEX_CONSTANT_COUNT - first;
	constants_store((unsigned long)first, constant_data, constant_count);
}

/* the program that runs: the one loaded at the selected address, else the
current shader's own */
static struct vertex_shader_object *current_program(void)
{
	struct vertex_shader_object *program = device.program_slots[device.program_address];

	return program ? program : device.vertex_shader;
}

/* ---------- program cache */

/* size is a multiple of 4 */
static unsigned long hash_words(const void *data, unsigned long size)
{
	const DWORD *words = data;
	unsigned long hash = 2166136261UL;

	for (size /= 4; size; size--)
		hash = (hash ^ *words++) * 16777619UL;
	return hash;
}

/* size is a multiple of 4. Every draw compares its pixel shader key and its
uniforms' inputs (about 300 bytes each) with recent ones: the loop has no
early exit, so that the compiler compares several words at once, where the
C library's memcmp (the Windows build's) compares them one at a time */
static BOOL words_equal(const void *a, const void *b, unsigned long size)
{
	const DWORD *words_a = a;
	const DWORD *words_b = b;
	DWORD difference = 0;
	unsigned long index;

	for (index = 0; index < size / 4; index++)
		difference |= words_a[index] ^ words_b[index];
	return difference == 0;
}

/* the shader text a debug.gpu_dump_shaders directory gets */
static void shader_dump(const char *name, const char *source)
{
	char path[512];
	FILE *file;

	if (!debug_settings.dump_shaders || !source)
		return;
	snprintf(path, sizeof(path), "%s/%s", debug_settings.dump_shaders, name);
	if ((file = fopen(path, "w")) != NULL)
	{
		fputs(source, file);
		fclose(file);
	}
}

/* lit: the shader that hands the lighting's normal and position on
(vertex_shader_object lit_shader) */
static GLuint vertex_shader_get(struct vertex_shader_object *program, BOOL immediate, BOOL lit)
{
	int variant = immediate ? 1 : 0;
	GLuint *shader = lit ? &program->lit_shader[variant] : &program->shader[variant];
	unsigned char tried = (unsigned char)(1 << (variant + (lit ? 2 : 0)));

	if (!*shader && !(program->shaders_tried & tried))
	{
		char *source = nv2a_vertex_shader_to_glsl(program->instructions, program->instruction_count,
			immediate ? 0 : device.vertex_shader->packed_mask, lit ? &program->lighting : NULL);
		char name[64];

		program->shaders_tried |= tried;
		*shader = source ? host_metal_shader(source, 0) : 0;
		snprintf(name, sizeof(name), "vs%03lu_%d%s_%03u.metal", program->id, variant, lit ? "_lit" : "", *shader);
		shader_dump(name, source);
		free(source);
	}
	return *shader;
}

typedef char pixel_shader_key_size_assert[sizeof(struct nv2a_pixel_shader_key) % 4 == 0 ? 1 : -1];

static GLuint fragment_shader_get(const struct nv2a_pixel_shader_key *key)
{
	/* consecutive draws mostly use one of a few pixel shaders (an object's
	parts take turns) */
#define RECENT_FRAGMENT_COUNT 4
	static struct fragment_entry *recent[RECENT_FRAGMENT_COUNT];
	static unsigned long recent_next;
	unsigned long hash, index;
	struct fragment_entry **bucket;
	struct fragment_entry *entry;
	char *source;
	char name[64];

	for (index = 0; index < RECENT_FRAGMENT_COUNT; index++)
	{
		if (recent[index] && words_equal(&recent[index]->key, key, sizeof(*key)))
			return recent[index]->shader;
	}
	hash = hash_words(key, sizeof(*key));
	bucket = &fragment_buckets[hash % FRAGMENT_BUCKETS];
	for (entry = *bucket; entry; entry = entry->next)
	{
		if (entry->hash == hash && words_equal(&entry->key, key, sizeof(*key)))
		{
			recent[recent_next++ % RECENT_FRAGMENT_COUNT] = entry;
			return entry->shader;
		}
	}
	entry = calloc(1, sizeof(*entry));
	if (!entry)
		return 0;
	entry->hash = hash;
	entry->key = *key;
	source = nv2a_pixel_shader_to_glsl(key);
	entry->shader = source ? host_metal_shader(source, 1) : 0;
	snprintf(name, sizeof(name), "ps%03u_%08lx.metal", entry->shader, hash);
	shader_dump(name, source);
	free(source);
	entry->next = *bucket;
	*bucket = entry;
	recent[recent_next++ % RECENT_FRAGMENT_COUNT] = entry;
	return entry->shader;
}


/* ---------- per-draw state */

static unsigned long stage_texture_mode(int stage)
{
	return (D3D__RenderState[D3DRS_PSTEXTUREMODES] >> (5 * stage)) & 0x1f;
}

static metal_u32 address_mode(DWORD mode)
{
	switch (mode)
	{
	case D3DTADDRESS_MIRROR: return METAL_ADDRESS_MIRROR_REPEAT;
	case D3DTADDRESS_CLAMP: return METAL_ADDRESS_CLAMP_TO_EDGE;
	case D3DTADDRESS_BORDER: return METAL_ADDRESS_CLAMP_TO_BORDER;
	case D3DTADDRESS_CLAMPTOEDGE: return METAL_ADDRESS_CLAMP_TO_EDGE;
	default: return METAL_ADDRESS_REPEAT;
	}
}

/* Metal's border colors are three: transparent black, opaque black and
opaque white, the nearest of which stands in for the stage's */
static metal_u32 border_color(D3DCOLOR color)
{
	unsigned long luminance = ((color >> 16) & 0xff) + ((color >> 8) & 0xff) + (color & 0xff);

	if (((color >> 24) & 0xff) < 0x80)
		return METAL_BORDER_TRANSPARENT_BLACK;
	return luminance >= 3 * 0x80 ? METAL_BORDER_OPAQUE_WHITE : METAL_BORDER_OPAQUE_BLACK;
}

/* a stage's sampler (the host makes one for each state); hires: a high-res
HUD texture (hud_hires.h), drawn smaller than it is, so filtered and from
its mip levels whatever the game asks: the HUD's meters are point sampled
for one player, to keep the Xbox bitmaps' texels sharp. The stage's LOD
bias is the pixel shader's (texture_lod_bias). */
static void configure_sampler(struct metal_sampler *sampler, int stage, BOOL mipmapped, BOOL hires)
{
	DWORD *state = D3D__TextureState[stage];
	DWORD min_filter = hires ? D3DTEXF_LINEAR : state[D3DTSS_MINFILTER];
	DWORD mip_filter = hires ? D3DTEXF_LINEAR : mipmapped ? state[D3DTSS_MIPFILTER] : D3DTEXF_NONE;
	DWORD mag_filter = hires ? D3DTEXF_LINEAR : state[D3DTSS_MAGFILTER];

	sampler->min_filter = min_filter == D3DTEXF_POINT ? METAL_FILTER_NEAREST : METAL_FILTER_LINEAR;
	sampler->mag_filter = mag_filter == D3DTEXF_POINT ? METAL_FILTER_NEAREST : METAL_FILTER_LINEAR;
	sampler->mip_filter = mip_filter == D3DTEXF_NONE ? METAL_MIP_NONE :
		mip_filter == D3DTEXF_POINT ? METAL_MIP_NEAREST : METAL_MIP_LINEAR;
	sampler->address_u = address_mode(state[D3DTSS_ADDRESSU]);
	sampler->address_v = address_mode(state[D3DTSS_ADDRESSV]);
	sampler->address_w = address_mode(state[D3DTSS_ADDRESSW]);
	sampler->border = border_color(state[D3DTSS_BORDERCOLOR]);
	sampler->anisotropy = min_filter == D3DTEXF_ANISOTROPIC && state[D3DTSS_MAXANISOTROPY] > 1 ?
		(metal_u32)state[D3DTSS_MAXANISOTROPY] : 1;
	sampler->min_lod = hires ? 0.0f : (float)state[D3DTSS_MAXMIPLEVEL];
}


/* ---------- render targets sampled with their mip chain

The game renders some textures one mip level at a time, each level being a
surface of its own (the water's ripple map). Sampling such a texture needs
every level in one GL texture, so the levels' render targets are copied into
a mipmapped composite. Each draw of the water binds it, some maps (a30) more
than once a frame, so the copy (and the mipmaps of the levels the game did not
render) is redone only once a level's target has been drawn into since the
last one. */

#define MIP_COMPOSITE_LEVELS 16

struct mip_composite
{
	struct mip_composite *next;
	unsigned long data, width, height, levels;
	GLuint texture;
	/* the levels last copied, and each one's target's texture and written
	serial then */
	unsigned long rendered_levels;
	GLuint level_sources[MIP_COMPOSITE_LEVELS];
	unsigned long level_written[MIP_COMPOSITE_LEVELS];
};

static struct mip_composite *mip_composites;

/* an entry render_target_get no longer finds: the screen's targets at
another scale or screen width than now, the shadow maps at another
display.shadow_resolution */
static BOOL render_target_stale(const struct render_target_entry *entry)
{
	switch (entry->kind)
	{
	case _render_target_screen:
		return entry->target.scale[0] != screen_scale[0] || entry->target.scale[1] != screen_scale[1] ||
			(entry->target.height == SCREEN_HEIGHT && entry->target.width != (unsigned long)halo_screen_width());
	case _render_target_shadow_map:
		return entry->target.scale[0] != (float)halo_shadow_map_scale();
	case _render_target_half:
	{
		float scale[2];

		half_res_scale(scale);
		return entry->target.scale[0] != scale[0] || entry->target.scale[1] != scale[1] ||
			entry->target.width != (unsigned long)halo_screen_width();
	}
	default:
		return FALSE;
	}
}


/* frees an entry taken out of render_targets: its texture, and what
remembered it */
static void render_target_delete(struct render_target_entry *entry)
{
	struct render_target_entry **link;
	struct mip_composite *composite;

	for (link = render_target_bucket(entry->target.data); *link != entry; link = &(*link)->next_in_bucket)
		;
	*link = entry->next_in_bucket;
	/* (a composite that copied from the texture copies again, rather than
	take a new texture with the same handle for it) */
	for (composite = mip_composites; composite; composite = composite->next)
	{
		unsigned long level;

		for (level = 0; level < composite->rendered_levels; level++)
		{
			if (composite->level_sources[level] == entry->target.texture)
			{
				composite->rendered_levels = 0;
				break;
			}
		}
	}
	host_metal_texture_delete(entry->target.texture);
	free(entry);
}

/* (between frames, when the screen's or the shadow maps' scale has
changed: halo_screen_commit) frees the entries render_target_get no longer
finds, which a frame at the old scale left */
static void render_targets_evict(void)
{
	struct render_target_entry *entry, **link = &render_targets;
	BOOL any = FALSE;

	while ((entry = *link) != NULL)
	{
		if (render_target_stale(entry))
		{
			*link = entry->next;
			render_target_delete(entry);
			any = TRUE;
		}
		else
		{
			link = &entry->next;
		}
	}
	if (any)
	{
		memset(recent_render_targets, 0, sizeof(recent_render_targets));
		xgpu_gl_state_invalidate();
	}
}


static GLuint mip_composite_get(const struct xgpu_texture_description *description, unsigned long data)
{
	struct mip_composite *composite;
	struct xgpu_render_target *targets[MIP_COMPOSITE_LEVELS];
	unsigned long level, rendered_levels = 0;
	BOOL changed;

	for (composite = mip_composites; composite; composite = composite->next)
	{
		if (composite->data == data && composite->width == description->width &&
			composite->height == description->height && composite->levels == description->levels)
		{
			break;
		}
	}
	if (!composite)
	{
		composite = calloc(1, sizeof(*composite));
		if (!composite)
			return 0;
		composite->data = data;
		composite->width = description->width;
		composite->height = description->height;
		composite->levels = description->levels;
		composite->texture = host_metal_texture_new();
		host_metal_texture_define(composite->texture, _metal_texture_2d, METAL_FORMAT_BGRA8,
			(unsigned int)description->width, (unsigned int)description->height, 1, (unsigned int)description->levels,
			_metal_usage_color_target, METAL_SWIZZLE_IDENTITY);
		composite->rendered_levels = ~0UL;
		composite->next = mip_composites;
		mip_composites = composite;
	}
	for (level = 0; level < description->levels && level < MIP_COMPOSITE_LEVELS; level++)
	{
		unsigned long width = description->width >> level ? description->width >> level : 1;
		unsigned long height = description->height >> level ? description->height >> level : 1;
		struct xgpu_render_target *target =
			xgpu_render_target_find(data + xgpu_texture_level_offset(description, level));

		if (!target || target->width != width || target->height != height ||
			target->gl_width != width || target->gl_height != height)
			break;
		targets[level] = target;
		rendered_levels++;
	}
	changed = rendered_levels != composite->rendered_levels;
	for (level = 0; level < rendered_levels && !changed; level++)
	{
		changed = targets[level]->texture != composite->level_sources[level] ||
			targets[level]->written != composite->level_written[level];
	}
	if (!changed)
		return composite->texture;
	composite->rendered_levels = rendered_levels;
	for (level = 0; level < rendered_levels; level++)
	{
		struct xgpu_render_target *target = targets[level];

		composite->level_sources[level] = target->texture;
		composite->level_written[level] = target->written;
		host_metal_texture_copy_level(target->texture, composite->texture, (unsigned int)level,
			(unsigned int)target->width, (unsigned int)target->height);
	}
	/* levels the game did not render come from the ones it did */
	if (rendered_levels < description->levels)
		host_metal_texture_generate_mipmaps(composite->texture, rendered_levels ? (unsigned int)rendered_levels - 1 : 0);
	return composite->texture;
}


static void bind_textures(struct nv2a_pixel_shader_key *key, float texture_scale[4][4], struct metal_draw *draw)
{
	int stage;

	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		D3DBaseTexture *texture = device.textures[stage];
		unsigned long mode = stage_texture_mode(stage);

		texture_scale[stage][0] = texture_scale[stage][1] = 1.0f;
		texture_scale[stage][2] = texture_scale[stage][3] = 1.0f;
		if (!texture || !texture->Data || mode == 0 || mode == 0x04 || mode == 0x05 || mode == 0x11)
		{
			draw->textures[stage] = 0;
			draw->texture_types[stage] = _metal_texture_2d;
			key->sampler_type[stage] = mode == 0x11 ? _xgpu_sampler_2d : _xgpu_sampler_none;
			continue;
		}
		{
			struct xgpu_render_target *target = xgpu_render_target_find(texture->Data);
			struct xgpu_texture_description description;
			GLenum gl_target;
			GLuint handle;

			if (target)
			{
				xgpu_texture_describe(texture->Format, texture->Size, &description);
				handle = target->texture;
				gl_target = GL_TEXTURE_2D;
				if (description.linear)
				{
					texture_scale[stage][0] = 1.0f / (float)target->width;
					texture_scale[stage][1] = 1.0f / (float)target->height;
					/* (the scene target: the scene is in its top left part) */
					if (target == &scene_entry.target)
					{
						texture_scale[stage][0] *= scene.fraction;
						texture_scale[stage][1] *= scene.fraction;
					}
				}
				if (!description.linear && !description.cube_map && description.levels > 1 &&
					target->width == description.width && target->height == description.height)
					handle = mip_composite_get(&description, texture->Data);
				else
					description.levels = 1;
			}
			else
			{
				const D3DCOLOR *palette = device.palettes[stage] && device.palettes[stage]->Data ?
					(const D3DCOLOR *)PLATFORM_PHYSICAL_TO_VIRTUAL(device.palettes[stage]->Data) : NULL;

				handle = xgpu_texture_get((const DWORD *)texture, palette, &gl_target, &description);
				if (description.linear)
				{
					texture_scale[stage][0] = 1.0f / (float)description.width;
					texture_scale[stage][1] = 1.0f / (float)description.height;
				}
			}
			draw->textures[stage] = handle;
			draw->texture_types[stage] = gl_target == GL_TEXTURE_CUBE_MAP ? _metal_texture_cube :
				gl_target == GL_TEXTURE_3D ? _metal_texture_3d : _metal_texture_2d;
			configure_sampler(&draw->samplers[stage], stage, description.levels > 1, description.hires);
			if (stage == 0)
			{
				key->coverage_alpha = description.hires_coverage != FALSE;
				key->point_threshold = description.hires_point_threshold != FALSE;
			}
			key->sampler_type[stage] = gl_target == GL_TEXTURE_CUBE_MAP ? _xgpu_sampler_cube :
				gl_target == GL_TEXTURE_3D ? _xgpu_sampler_3d : _xgpu_sampler_2d;
		}
	}
}

static metal_u32 compare_function(DWORD function)
{
	/* D3DCMP_NEVER (0x200) to D3DCMP_ALWAYS are Metal's in order */
	return function >= D3DCMP_NEVER && function <= D3DCMP_ALWAYS ? (metal_u32)(function - D3DCMP_NEVER) :
		METAL_COMPARE_NEVER;
}

static metal_u32 stencil_operation(DWORD operation)
{
	switch (operation)
	{
	case D3DSTENCILOP_ZERO: return METAL_STENCIL_ZERO;
	case D3DSTENCILOP_REPLACE: return METAL_STENCIL_REPLACE;
	case D3DSTENCILOP_INCRSAT: return METAL_STENCIL_INCREMENT_CLAMP;
	case D3DSTENCILOP_DECRSAT: return METAL_STENCIL_DECREMENT_CLAMP;
	case D3DSTENCILOP_INVERT: return METAL_STENCIL_INVERT;
	case D3DSTENCILOP_INCR: return METAL_STENCIL_INCREMENT_WRAP;
	case D3DSTENCILOP_DECR: return METAL_STENCIL_DECREMENT_WRAP;
	default: return METAL_STENCIL_KEEP;
	}
}

static metal_u32 blend_factor(DWORD factor)
{
	switch (factor)
	{
	case D3DBLEND_ZERO: return METAL_BLEND_ZERO;
	case D3DBLEND_SRCCOLOR: return METAL_BLEND_SOURCE_COLOR;
	case D3DBLEND_INVSRCCOLOR: return METAL_BLEND_ONE_MINUS_SOURCE_COLOR;
	case D3DBLEND_SRCALPHA: return METAL_BLEND_SOURCE_ALPHA;
	case D3DBLEND_INVSRCALPHA: return METAL_BLEND_ONE_MINUS_SOURCE_ALPHA;
	case D3DBLEND_DESTALPHA: return METAL_BLEND_DESTINATION_ALPHA;
	case D3DBLEND_INVDESTALPHA: return METAL_BLEND_ONE_MINUS_DESTINATION_ALPHA;
	case D3DBLEND_DESTCOLOR: return METAL_BLEND_DESTINATION_COLOR;
	case D3DBLEND_INVDESTCOLOR: return METAL_BLEND_ONE_MINUS_DESTINATION_COLOR;
	case D3DBLEND_SRCALPHASAT: return METAL_BLEND_SOURCE_ALPHA_SATURATED;
	case D3DBLEND_CONSTANTCOLOR: return METAL_BLEND_BLEND_COLOR;
	case D3DBLEND_INVCONSTANTCOLOR: return METAL_BLEND_ONE_MINUS_BLEND_COLOR;
	case D3DBLEND_CONSTANTALPHA: return METAL_BLEND_BLEND_ALPHA;
	case D3DBLEND_INVCONSTANTALPHA: return METAL_BLEND_ONE_MINUS_BLEND_ALPHA;
	default: return METAL_BLEND_ONE;
	}
}

static metal_u32 blend_operation(DWORD operation)
{
	switch (operation)
	{
	case D3DBLENDOP_SUBTRACT: return METAL_BLEND_OPERATION_SUBTRACT;
	case D3DBLENDOP_REVSUBTRACT:
	case D3DBLENDOP_REVSUBTRACTSIGNED: return METAL_BLEND_OPERATION_REVERSE_SUBTRACT;
	case D3DBLENDOP_MIN: return METAL_BLEND_OPERATION_MIN;
	case D3DBLENDOP_MAX: return METAL_BLEND_OPERATION_MAX;
	default: return METAL_BLEND_OPERATION_ADD;
	}
}

/* the draw's fixed-function state; what a disabled test or blend would
have read is left zero, so that the host's state objects are found again */
static void apply_raster_state(BOOL has_depth, struct metal_draw *draw)
{
	DWORD *rs = D3D__RenderState;
	DWORD write = rs[D3DRS_COLORWRITEENABLE];
	GLint x0 = target_pixel((float)device.viewport.X, 0);
	GLint y0 = target_pixel((float)device.viewport.Y, 1);
	GLint x1 = target_pixel((float)(device.viewport.X + device.viewport.Width), 0);
	GLint y1 = target_pixel((float)(device.viewport.Y + device.viewport.Height), 1);
	BOOL depth_test = has_depth && rs[D3DRS_ZENABLE];
	BOOL stencil_test = has_depth && rs[D3DRS_STENCILENABLE];

	draw->viewport[0] = (float)x0;
	draw->viewport[1] = (float)y0;
	draw->viewport[2] = (float)(x1 - x0);
	draw->viewport[3] = (float)(y1 - y0);
	draw->viewport[4] = device.viewport.MinZ;
	draw->viewport[5] = device.viewport.MaxZ;
	/* the game never issues a scissor rectangle, and the NV2A scissor
	register defaults to the viewport, so fragment clipping follows the
	viewport: this is what keeps a split-screen window's geometry from
	bleeding across the divider (inside the target: the host's) */
	{
		GLint left = x0 > 0 ? x0 : 0, top = y0 > 0 ? y0 : 0;

		draw->scissor[0] = (metal_u32)left;
		draw->scissor[1] = (metal_u32)top;
		draw->scissor[2] = x1 > left ? (metal_u32)(x1 - left) : 0;
		draw->scissor[3] = y1 > top ? (metal_u32)(y1 - top) : 0;
	}

	if (depth_test)
	{
		draw->depth_stencil.depth_test = 1;
		draw->depth_stencil.depth_function = compare_function(rs[D3DRS_ZFUNC]);
		draw->depth_stencil.depth_write = rs[D3DRS_ZWRITEENABLE] != 0;
	}
	if (stencil_test)
	{
		draw->depth_stencil.stencil_test = 1;
		draw->depth_stencil.stencil_function = compare_function(rs[D3DRS_STENCILFUNC]);
		draw->depth_stencil.stencil_fail = stencil_operation(rs[D3DRS_STENCILFAIL]);
		draw->depth_stencil.stencil_depth_fail = stencil_operation(rs[D3DRS_STENCILZFAIL]);
		draw->depth_stencil.stencil_pass = stencil_operation(rs[D3DRS_STENCILPASS]);
		draw->depth_stencil.stencil_read_mask = rs[D3DRS_STENCILMASK] & 0xff;
		draw->depth_stencil.stencil_write_mask = rs[D3DRS_STENCILWRITEMASK] & 0xff;
		draw->stencil_reference = rs[D3DRS_STENCILREF] & 0xff;
	}

	if (rs[D3DRS_ALPHABLENDENABLE])
	{
		draw->blend.enabled = 1;
		/* (a constant factor the pixel shader applies: source_blend_in_shader) */
		draw->blend.source = source_blend_in_shader() ? METAL_BLEND_ONE : blend_factor(rs[D3DRS_SRCBLEND]);
		draw->blend.destination = blend_factor(rs[D3DRS_DESTBLEND]);
		draw->blend.operation = blend_operation(rs[D3DRS_BLENDOP]);
		color_to_vec4(rs[D3DRS_BLENDCOLOR], draw->blend_color);
	}
	draw->blend.write_mask = ((write & D3DCOLORWRITEENABLE_RED) ? METAL_WRITE_RED : 0) |
		((write & D3DCOLORWRITEENABLE_GREEN) ? METAL_WRITE_GREEN : 0) |
		((write & D3DCOLORWRITEENABLE_BLUE) ? METAL_WRITE_BLUE : 0) |
		((write & D3DCOLORWRITEENABLE_ALPHA) ? METAL_WRITE_ALPHA : 0);

	/* the cull mode names the winding to discard; FRONTFACE names the
	front winding, which is Metal's as it is D3D's (window space, y down) */
	draw->winding = rs[D3DRS_FRONTFACE] == D3DFRONT_CCW ? METAL_WINDING_COUNTER_CLOCKWISE : METAL_WINDING_CLOCKWISE;
	draw->cull = rs[D3DRS_CULLMODE] == D3DCULL_NONE ? METAL_CULL_NONE :
		rs[D3DRS_CULLMODE] == rs[D3DRS_FRONTFACE] ? METAL_CULL_FRONT : METAL_CULL_BACK;
	/* (Metal fills or draws the edges; points are filled) */
	draw->wireframe = rs[D3DRS_FILLMODE] == D3DFILL_WIREFRAME;

	/* D3DRS_ZBIAS is expressed in these states (D3DDevice_SetRenderState_ZBias) */
	if (rs[D3DRS_SOLIDOFFSETENABLE])
	{
		draw->depth_slope = dword_to_float(rs[D3DRS_POLYGONOFFSETZSLOPESCALE]);
		draw->depth_bias = dword_to_float(rs[D3DRS_POLYGONOFFSETZOFFSET]);
	}
}

/* ---------- how the pixel shaders are written (nv2a_pixel_shader_key's
shading, XGPU_SHADING_*; port/tvos/PERFORMANCE.md "Shading (A12)")

The default is the text as it always was (mask 0): none of the changes
gave byte-identical frames (frozen frames, the old text and the new in one
run: PERFORMANCE.md). display.half_precision_shading turns them all on
(faster on the A12, a few levels on some pixels: an opt-in). Diagnostics:
HALO_METAL_SHADING=<mask> sets the mask outright (1 half combiners, 2 half
samples, 4 alpha test as a color of 0, 8 no LOD bias of 0), and
HALO_METAL_SHADING_CYCLE=N alternates it with 0 (the old text) in blocks
of N frames: even blocks 0, odd the mask, for frozen-frame comparisons in
one run. */

#define SHADING_FAST (XGPU_SHADING_HALF_COMBINERS | XGPU_SHADING_HALF_SAMPLES | XGPU_SHADING_ZERO_ALPHA_TEST | \
	XGPU_SHADING_ZERO_BIAS)

static unsigned char shading_mask(void)
{
	static unsigned long read_at = (unsigned long)-1;
	static int forced = -2;
	static long cycle = -1;
	static unsigned char mask;

	if (forced == -2)
	{
		const char *setting = getenv("HALO_METAL_SHADING");
		const char *cycle_setting = getenv("HALO_METAL_SHADING_CYCLE");

		forced = setting && *setting ? (int)(strtol(setting, NULL, 0) & 0xff) : -1;
		cycle = cycle_setting && atol(cycle_setting) > 0 ? atol(cycle_setting) : 0;
		if (forced >= 0 || cycle)
			platform_log("GPU: HALO_METAL_SHADING=%d HALO_METAL_SHADING_CYCLE=%ld (diagnostic)", forced, cycle);
	}
	if (read_at != config_changes())
	{
		read_at = config_changes();
		mask = (unsigned char)(config_boolean("display.half_precision_shading") ? SHADING_FAST : 0);
		platform_log("GPU: display.half_precision_shading %s (pixel shader mask %u)", mask ? "on" : "off", mask);
	}
	{
		unsigned char chosen = forced >= 0 ? (unsigned char)forced : mask;

		if (cycle && (device.frame / (unsigned long)cycle) % 2 == 0)
			return 0;
		return chosen;
	}
}

/* the draw's shading flags: the mask, less what does not apply to it. A
failed alpha test may give the color 0 instead of discarding (a discard
costs Apple's GPUs their hidden surface removal and early tests) only where
that leaves every attachment as the discard would: a blend that adds the
color with the destination kept whole (a destination factor of one, or one
less the source's alpha or color: 0 makes both 1), no depth write, no
stencil operation that writes (a discarded fragment changes no stencil,
even where its depth test fails), and no visibility test counting the
samples */
static unsigned char shading_flags(const struct metal_draw *draw)
{
	unsigned char flags = shading_mask();

	if (flags & XGPU_SHADING_ZERO_ALPHA_TEST)
	{
		const struct metal_blend *blend = &draw->blend;
		const struct metal_depth_stencil *depth_stencil = &draw->depth_stencil;
		BOOL keeps = blend->enabled &&
			(blend->operation == METAL_BLEND_OPERATION_ADD || blend->operation == METAL_BLEND_OPERATION_REVERSE_SUBTRACT) &&
			(blend->destination == METAL_BLEND_ONE || blend->destination == METAL_BLEND_ONE_MINUS_SOURCE_ALPHA ||
				blend->destination == METAL_BLEND_ONE_MINUS_SOURCE_COLOR);

		if (!keeps || (depth_stencil->depth_test && depth_stencil->depth_write) ||
			(depth_stencil->stencil_test && depth_stencil->stencil_write_mask &&
				(depth_stencil->stencil_pass != METAL_STENCIL_KEEP || depth_stencil->stencil_fail != METAL_STENCIL_KEEP ||
					depth_stencil->stencil_depth_fail != METAL_STENCIL_KEEP)) ||
			device.visibility_test_active)
		{
			flags &= (unsigned char)~XGPU_SHADING_ZERO_ALPHA_TEST;
		}
	}
	return flags;
}

/* the uniforms of the latest draws, converted from these inputs; the serial
counts the conversions */
#define DRAW_UNIFORM_INPUT_COUNT (4 + 4 + 16 + 1 + 16 + 2 + 4 + 1 + 1 + 7 * D3DTSS_MAXSTAGES)

static DWORD draw_uniform_inputs[DRAW_UNIFORM_INPUT_COUNT];
static struct draw_uniforms draw_uniforms;
static unsigned long draw_uniforms_serial;

/* the uniform buffers the shaders read (xgpu.h), as the host was last sent
them, and a serial for each that changes with them (never 0) */
static struct xgpu_vertex_uniforms vertex_uniforms;
static struct xgpu_pixel_uniforms pixel_uniforms;
static unsigned int vertex_uniforms_serial, pixel_uniforms_serial;
/* draw_uniforms_serial when they were last converted */
static unsigned long uniforms_converted;

static unsigned int serial_next(unsigned int serial)
{
	return serial + 1 ? serial + 1 : 1;
}

/* display.per_pixel_lighting: the model lighting programs' draws are lit for
each pixel (nv2a_psh.c model_lighting), from shaders of their own; read
again when a setting changes */
static BOOL per_pixel_lighting(void)
{
	static unsigned long read_at = (unsigned long)-1;
	static BOOL enabled;

	if (read_at != config_changes())
	{
		read_at = config_changes();
		enabled = config_boolean("display.per_pixel_lighting") != 0;
	}
	return enabled;
}

/* the vertex constant register of each of the per-pixel lighting's
(XGPU_MODEL_LIGHT_COUNT) */
static unsigned long model_light_register(int light)
{
	return (unsigned long)(XGPU_VERTEX_CONSTANT_BIAS + (light ? -80 + light : -82));
}

/* the draw's program, state and uniforms; FALSE if there is nothing to
draw (no program or target, or a shader that does not build) */
static BOOL prepare_draw(BOOL immediate, struct metal_draw *draw)
{
	struct vertex_shader_object *program = current_program();
	struct nv2a_pixel_shader_key key;
	float texture_scale[4][4];
	GLuint vertex_shader = 0, fragment_shader = 0;
	BOOL has_depth = FALSE;
	int stage;

	if (!device.ready || !program || !device.vertex_shader || !program->instructions)
	{
		stats.skipped_no_program++;
		return FALSE;
	}
	{
		const char *skip = debug_settings.skip_vertex_shaders;

		while (skip && *skip)
		{
			if ((unsigned long)atol(skip) == program->id)
				return FALSE;
			skip = strchr(skip, ',');
			if (skip)
				skip++;
		}
	}
	memset(draw, 0, sizeof(*draw));
	draw->visibility_test = ~0u;
	memset(&key, 0, sizeof(key));
	memcpy(key.combiner_state, D3D__RenderState, sizeof(key.combiner_state));
	/* constants are uniforms, not part of the program */
	memset(&key.combiner_state[D3DRS_PSCONSTANT0_0], 0, 16 * sizeof(DWORD));
	key.combiner_state[D3DRS_PSFINALCOMBINERCONSTANT0] = 0;
	key.combiner_state[D3DRS_PSFINALCOMBINERCONSTANT1] = 0;
	key.texture_modes = D3D__RenderState[D3DRS_PSTEXTUREMODES];
	/* the textures before the targets: a render target the draw samples is
	copied (a mip composite) before the pass that draws */
	bind_textures(&key, texture_scale, draw);
	if (!bind_targets(&has_depth))
	{
		stats.skipped_no_target++;
		return FALSE;
	}
	apply_raster_state(has_depth, draw);
	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		key.alpha_kill[stage] = D3D__TextureState[stage][D3DTSS_ALPHAKILL] == D3DTALPHAKILL_ENABLE;
		key.color_sign[stage] = (unsigned char)((D3D__TextureState[stage][D3DTSS_COLORSIGN] >> 28) & 0xf);
	}
	/* (only with the meter's blend: hud_hires.h, nv2a_pixel_shader_key) */
	key.coverage_alpha = key.coverage_alpha && D3D__RenderState[D3DRS_ALPHABLENDENABLE] &&
		D3D__RenderState[D3DRS_SRCBLEND] == D3DBLEND_CONSTANTCOLOR &&
		D3D__RenderState[D3DRS_DESTBLEND] == D3DBLEND_SRCALPHA;
	key.point_threshold = key.point_threshold && key.coverage_alpha;
	key.alpha_test_function = D3D__RenderState[D3DRS_ALPHATESTENABLE] ? D3D__RenderState[D3DRS_ALPHAFUNC] : 0;
	key.fog_enable = D3D__RenderState[D3DRS_FOGENABLE] != 0;
	key.source_blend = source_blend_in_shader();
	key.fog_table_mode = (unsigned char)D3D__RenderState[D3DRS_FOGTABLEMODE];
	key.shading = shading_flags(draw);
	if (!key.alpha_test_function)
		key.shading &= (unsigned char)~XGPU_SHADING_ZERO_ALPHA_TEST;
	if (key.shading & XGPU_SHADING_ZERO_BIAS)
	{
		for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
		{
			if (D3D__TextureState[stage][D3DTSS_MIPMAPLODBIAS])
				key.lod_bias_stages |= (unsigned char)(1 << stage);
		}
	}

	if (program->lighting.lights && !program->lighting_failed && per_pixel_lighting())
	{
		key.per_pixel_lighting = (unsigned char)program->lighting.lights;
		vertex_shader = vertex_shader_get(program, immediate, TRUE);
		fragment_shader = vertex_shader ? fragment_shader_get(&key) : 0;
		if (!vertex_shader || !fragment_shader)
		{
			/* drawn as the vertex shader lights it instead (not at all,
			were it to fail too) */
			platform_log("GPU: vertex shader %lu cannot be lit for each pixel here (refer to the shader log above): "
				"lit for each vertex", program->id);
			program->lighting_failed = TRUE;
			key.per_pixel_lighting = 0;
			vertex_shader = fragment_shader = 0;
		}
	}
	if (!vertex_shader)
	{
		vertex_shader = vertex_shader_get(program, immediate, FALSE);
		fragment_shader = vertex_shader ? fragment_shader_get(&key) : 0;
	}
	if (!vertex_shader || !fragment_shader)
	{
		stats.skipped_link++;
		return FALSE;
	}
	/* HALO_METAL_DEBUG's 8: texture 0 as it is sampled, or (16) the
	diffuse color, in place of the pixel shader (finding where a color goes
	wrong) */
	{
		static int debug = -1;
		static GLuint debug_shaders[2][4];

		if (debug < 0)
			debug = getenv("HALO_METAL_DEBUG") ? atoi(getenv("HALO_METAL_DEBUG")) : 0;
		if (debug & 32)
		{
			static GLuint uniforms_shader;

			if (!uniforms_shader)
			{
				struct xgpu_text text = { 0 };

				xgpu_text_append(&text, "%s%s", XGPU_METAL_PROLOGUE, nv2a_varyings_declaration(0));
				xgpu_text_append(&text, "fragment vec4 fragment_main(nv2a_varyings in [[stage_in]],"
					" constant xgpu_pixel_uniforms &u [[buffer(0)]])\n{\n"
					"\treturn vec4(u.texture_scale[0].x, u.ps_c0[0].a + u.ps_c0[0].r, u.alpha_reference / 255.0 + 0.5, 1.0);\n}\n");
				uniforms_shader = host_metal_shader(text.buffer, 1);
				free(text.buffer);
			}
			fragment_shader = uniforms_shader;
		}
		if (debug & 24)
		{
			int which = (debug & 8) ? 0 : 1, type = key.sampler_type[0];

			if (!debug_shaders[which][type])
			{
				struct xgpu_text text = { 0 };
				static const char *const textures[] = { "texture2d<float>", "texture2d<float>", "texture3d<float>",
					"texturecube<float>" };

				xgpu_text_append(&text, "%s%s", XGPU_METAL_PROLOGUE, nv2a_varyings_declaration(0));
				xgpu_text_append(&text, "fragment vec4 fragment_main(nv2a_varyings in [[stage_in]], %s tex0 [[texture(0)]],"
					" sampler smp0 [[sampler(0)]])\n{\n\treturn %s;\n}\n", textures[type],
					which ? "in.xD0" : type == _xgpu_sampler_none ? "vec4(1.0, 0.0, 1.0, 1.0)" :
					type == _xgpu_sampler_2d ? "vec4(tex0.sample(smp0, in.xT0.xy).rgb, 1.0)" :
					"vec4(tex0.sample(smp0, in.xT0.xyz).rgb, 1.0)");
				debug_shaders[which][type] = host_metal_shader(text.buffer, 1);
				free(text.buffer);
			}
			fragment_shader = debug_shaders[which][type];
		}
	}
	if (trace_frame())
	{
		platform_log("    metal shaders %u/%u ps_c0 %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx", vertex_shader,
			fragment_shader, D3D__RenderState[D3DRS_PSCONSTANT0_0], D3D__RenderState[D3DRS_PSCONSTANT0_0 + 1],
			D3D__RenderState[D3DRS_PSCONSTANT0_0 + 2], D3D__RenderState[D3DRS_PSCONSTANT0_0 + 3],
			D3D__RenderState[D3DRS_PSCONSTANT0_0 + 4], D3D__RenderState[D3DRS_PSCONSTANT0_0 + 5],
			D3D__RenderState[D3DRS_PSCONSTANT0_0 + 6], D3D__RenderState[D3DRS_PSCONSTANT0_0 + 7]);
		platform_log("    ps_c1 %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx final %08lx %08lx",
			D3D__RenderState[D3DRS_PSCONSTANT1_0], D3D__RenderState[D3DRS_PSCONSTANT1_0 + 1],
			D3D__RenderState[D3DRS_PSCONSTANT1_0 + 2], D3D__RenderState[D3DRS_PSCONSTANT1_0 + 3],
			D3D__RenderState[D3DRS_PSCONSTANT1_0 + 4], D3D__RenderState[D3DRS_PSCONSTANT1_0 + 5],
			D3D__RenderState[D3DRS_PSCONSTANT1_0 + 6], D3D__RenderState[D3DRS_PSCONSTANT1_0 + 7],
			D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT0], D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT1]);
	}
	if (immediate)
		stats.immediate_draws++;
	else
		stats.draws++;
	draw->vertex_shader = vertex_shader;
	draw->fragment_shader = fragment_shader;
	if (device.visibility_test_active)
		draw->visibility_test = device.visibility_test;

	/* the vertex constants, all of them (as the host was last sent them
	unless they have changed since) */
	draw->vertex_constants = GUEST_ADDRESS(device.constants);
	draw->vertex_constants_serial = constants_serial;

	/* the state the other uniforms come from: most draws share it with the
	draw before them, and so share its uniforms */
	{
		DWORD inputs[DRAW_UNIFORM_INPUT_COUNT];
		unsigned long count = 0;

		memcpy(&inputs[count], device.viewport_scale, sizeof(device.viewport_scale));
		count += 4;
		memcpy(&inputs[count], device.viewport_offset, sizeof(device.viewport_offset));
		count += 4;
		memcpy(&inputs[count], texture_scale, sizeof(texture_scale));
		count += 16;
		inputs[count++] = D3D__RenderState[D3DRS_POINTSIZE];
		for (stage = 0; stage < 8; stage++)
		{
			inputs[count++] = D3D__RenderState[D3DRS_PSCONSTANT0_0 + stage];
			inputs[count++] = D3D__RenderState[D3DRS_PSCONSTANT1_0 + stage];
		}
		inputs[count++] = D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT0];
		inputs[count++] = D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT1];
		inputs[count++] = D3D__RenderState[D3DRS_FOGCOLOR];
		inputs[count++] = D3D__RenderState[D3DRS_FOGSTART];
		inputs[count++] = D3D__RenderState[D3DRS_FOGEND];
		inputs[count++] = D3D__RenderState[D3DRS_FOGDENSITY];
		inputs[count++] = D3D__RenderState[D3DRS_ALPHAREF];
		inputs[count++] = (DWORD)UI_OFFSET;
		for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
		{
			DWORD *state = D3D__TextureState[stage];

			inputs[count++] = state[D3DTSS_BUMPENVMAT00];
			inputs[count++] = state[D3DTSS_BUMPENVMAT01];
			inputs[count++] = state[D3DTSS_BUMPENVMAT10];
			inputs[count++] = state[D3DTSS_BUMPENVMAT11];
			inputs[count++] = state[D3DTSS_BUMPENVLSCALE];
			inputs[count++] = state[D3DTSS_BUMPENVLOFFSET];
			inputs[count++] = state[D3DTSS_MIPMAPLODBIAS];
		}
		if (!draw_uniforms_serial || !words_equal(inputs, draw_uniform_inputs, sizeof(inputs)))
		{
			struct draw_uniforms *converted = &draw_uniforms;

			memcpy(draw_uniform_inputs, inputs, sizeof(inputs));
			draw_uniforms_serial++;
			memcpy(converted->viewport_scale, device.viewport_scale, sizeof(converted->viewport_scale));
			memcpy(converted->viewport_offset, device.viewport_offset, sizeof(converted->viewport_offset));
			memcpy(converted->texture_scale, texture_scale, sizeof(converted->texture_scale));
			converted->point_size = D3D__RenderState[D3DRS_POINTSIZE] ?
				dword_to_float(D3D__RenderState[D3DRS_POINTSIZE]) : 1.0f;
			for (stage = 0; stage < 8; stage++)
			{
				color_to_vec4(D3D__RenderState[D3DRS_PSCONSTANT0_0 + stage], converted->ps_c0[stage]);
				color_to_vec4(D3D__RenderState[D3DRS_PSCONSTANT1_0 + stage], converted->ps_c1[stage]);
			}
			color_to_vec4(D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT0], converted->ps_final_c0);
			color_to_vec4(D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT1], converted->ps_final_c1);
			color_to_vec4(D3D__RenderState[D3DRS_FOGCOLOR], converted->fog_color);
			converted->fog_parameters[0] = dword_to_float(D3D__RenderState[D3DRS_FOGSTART]);
			converted->fog_parameters[1] = dword_to_float(D3D__RenderState[D3DRS_FOGEND]);
			converted->fog_parameters[2] = dword_to_float(D3D__RenderState[D3DRS_FOGDENSITY]);
			converted->fog_parameters[3] = 0.0f;
			converted->alpha_reference = (float)(D3D__RenderState[D3DRS_ALPHAREF] & 0xff);
			for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
			{
				DWORD *state = D3D__TextureState[stage];

				converted->bump_matrix[stage][0] = dword_to_float(state[D3DTSS_BUMPENVMAT00]);
				converted->bump_matrix[stage][1] = dword_to_float(state[D3DTSS_BUMPENVMAT01]);
				converted->bump_matrix[stage][2] = dword_to_float(state[D3DTSS_BUMPENVMAT10]);
				converted->bump_matrix[stage][3] = dword_to_float(state[D3DTSS_BUMPENVMAT11]);
				converted->bump_luminance[stage][0] = dword_to_float(state[D3DTSS_BUMPENVLSCALE]);
				converted->bump_luminance[stage][1] = dword_to_float(state[D3DTSS_BUMPENVLOFFSET]);
				converted->bump_luminance[stage][2] = converted->bump_luminance[stage][3] = 0.0f;
				converted->texture_lod_bias[stage] = dword_to_float(state[D3DTSS_MIPMAPLODBIAS]);
			}
			converted->screen_offset = (float)UI_OFFSET;
		}
	}
	/* into the shaders' uniform buffers, when they have changed */
	if (uniforms_converted != draw_uniforms_serial)
	{
		struct xgpu_vertex_uniforms vertex;

		uniforms_converted = draw_uniforms_serial;
		memset(&vertex, 0, sizeof(vertex));
		memcpy(vertex.viewport_scale, draw_uniforms.viewport_scale, sizeof(vertex.viewport_scale));
		memcpy(vertex.viewport_offset, draw_uniforms.viewport_offset, sizeof(vertex.viewport_offset));
		vertex.point_size = draw_uniforms.point_size;
		vertex.screen_offset = draw_uniforms.screen_offset;
		if (!vertex_uniforms_serial || memcmp(&vertex, &vertex_uniforms, sizeof(vertex)))
		{
			vertex_uniforms = vertex;
			vertex_uniforms_serial = serial_next(vertex_uniforms_serial);
		}
		memcpy(pixel_uniforms.ps_c0, draw_uniforms.ps_c0, sizeof(pixel_uniforms.ps_c0));
		memcpy(pixel_uniforms.ps_c1, draw_uniforms.ps_c1, sizeof(pixel_uniforms.ps_c1));
		memcpy(pixel_uniforms.ps_final_c0, draw_uniforms.ps_final_c0, sizeof(pixel_uniforms.ps_final_c0));
		memcpy(pixel_uniforms.ps_final_c1, draw_uniforms.ps_final_c1, sizeof(pixel_uniforms.ps_final_c1));
		memcpy(pixel_uniforms.fog_color, draw_uniforms.fog_color, sizeof(pixel_uniforms.fog_color));
		memcpy(pixel_uniforms.fog_parameters, draw_uniforms.fog_parameters, sizeof(pixel_uniforms.fog_parameters));
		memcpy(pixel_uniforms.bump_matrix, draw_uniforms.bump_matrix, sizeof(pixel_uniforms.bump_matrix));
		memcpy(pixel_uniforms.bump_luminance, draw_uniforms.bump_luminance, sizeof(pixel_uniforms.bump_luminance));
		memcpy(pixel_uniforms.texture_scale, draw_uniforms.texture_scale, sizeof(pixel_uniforms.texture_scale));
		memcpy(pixel_uniforms.texture_lod_bias, draw_uniforms.texture_lod_bias, sizeof(pixel_uniforms.texture_lod_bias));
		pixel_uniforms.alpha_reference = draw_uniforms.alpha_reference;
		pixel_uniforms_serial = serial_next(pixel_uniforms_serial);
	}
	/* the lights of a draw lit for each pixel, from the vertex constants */
	if (key.per_pixel_lighting)
	{
		BOOL changed = FALSE;
		int light;

		for (light = 0; light < XGPU_MODEL_LIGHT_COUNT; light++)
		{
			const float *value = device.constants[model_light_register(light)];

			if (memcmp(pixel_uniforms.model_lights[light], value, sizeof(pixel_uniforms.model_lights[0])))
			{
				memcpy(pixel_uniforms.model_lights[light], value, sizeof(pixel_uniforms.model_lights[0]));
				changed = TRUE;
			}
		}
		if (changed)
			pixel_uniforms_serial = serial_next(pixel_uniforms_serial);
	}
	draw->vertex_uniforms = GUEST_ADDRESS(&vertex_uniforms);
	draw->vertex_uniforms_serial = vertex_uniforms_serial;
	draw->pixel_uniforms = GUEST_ADDRESS(&pixel_uniforms);
	draw->pixel_uniforms_serial = pixel_uniforms_serial;
	draw_constant_attributes(draw);
	return TRUE;
}

/* ---------- tracing (debug.gpu_trace_frame) */

static BOOL trace_frame(void)
{
	static long frame = -2;

	if (frame == -2)
		frame = config_integer("debug.gpu_trace_frame");
	return frame >= 0 && device.frame == (unsigned long)frame;
}

static void trace_draw(const char *kind, D3DPRIMITIVETYPE type, unsigned long count, const float *first_vertex)
{
	struct vertex_shader_object *program = current_program();
	DWORD *rs = D3D__RenderState;

	if (!trace_frame())
		return;
	platform_log("%s type %d count %lu vs %lu (decl %lu) vp %lu,%lu %lux%lu z%.2f-%.2f zen %lu zw %lu zf %lx blend %lu %lx/%lx cull %lx cw %08lx tm %05lx cc %lx fin %08lx/%08lx at %lu/%lx",
		kind, type, count, program ? program->id : 0, device.vertex_shader ? device.vertex_shader->id : 0,
		device.viewport.X, device.viewport.Y, device.viewport.Width, device.viewport.Height,
		device.viewport.MinZ, device.viewport.MaxZ, rs[D3DRS_ZENABLE], rs[D3DRS_ZWRITEENABLE], rs[D3DRS_ZFUNC],
		rs[D3DRS_ALPHABLENDENABLE], rs[D3DRS_SRCBLEND], rs[D3DRS_DESTBLEND], rs[D3DRS_CULLMODE],
		rs[D3DRS_COLORWRITEENABLE], rs[D3DRS_PSTEXTUREMODES], rs[D3DRS_PSCOMBINERCOUNT],
		rs[D3DRS_PSFINALCOMBINERINPUTSABCD], rs[D3DRS_PSFINALCOMBINERINPUTSEFG],
		rs[D3DRS_ALPHATESTENABLE], rs[D3DRS_ALPHAFUNC]);
	{
		int stage;

		for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
		{
			D3DBaseTexture *texture = device.textures[stage];
			struct xgpu_texture_description description;

			if (!texture || !((D3D__RenderState[D3DRS_PSTEXTUREMODES] >> (5 * stage)) & 0x1f))
				continue;
			xgpu_texture_describe(texture->Format, texture->Size, &description);
			platform_log("    t%d: data %08lx format %08lx size %08lx -> fmt %02lx %lux%lux%lu levels %lu linear %d cube %d rt %d min %lu mip %lu bias %g maxmip %lu",
				stage, texture->Data, texture->Format, texture->Size, description.format, description.width,
				description.height, description.depth, description.levels, description.linear, description.cube_map,
				xgpu_render_target_find(texture->Data) != NULL, D3D__TextureState[stage][D3DTSS_MINFILTER],
				D3D__TextureState[stage][D3DTSS_MIPFILTER], dword_to_float(D3D__TextureState[stage][D3DTSS_MIPMAPLODBIAS]),
				D3D__TextureState[stage][D3DTSS_MAXMIPLEVEL]);
		}
	}
	platform_log("    offset enable %lu slope %g offset %g zbias %ld stencil %lu func %lx ref %lx mask %lx write %lx ops %lx/%lx/%lx",
		rs[D3DRS_SOLIDOFFSETENABLE], dword_to_float(rs[D3DRS_POLYGONOFFSETZSLOPESCALE]),
		dword_to_float(rs[D3DRS_POLYGONOFFSETZOFFSET]), (long)rs[D3DRS_ZBIAS], rs[D3DRS_STENCILENABLE],
		rs[D3DRS_STENCILFUNC], rs[D3DRS_STENCILREF], rs[D3DRS_STENCILMASK], rs[D3DRS_STENCILWRITEMASK],
		rs[D3DRS_STENCILFAIL], rs[D3DRS_STENCILZFAIL], rs[D3DRS_STENCILPASS]);
	if (config_boolean("debug.gpu_trace_constants"))
	{
		int constant;

		for (constant = 0; constant < XGPU_VERTEX_CONSTANT_COUNT; constant++)
		{
			const float *value = device.constants[constant];

			if (value[0] || value[1] || value[2] || value[3])
				platform_log("    c[%d] = %g %g %g %g", constant, value[0], value[1], value[2], value[3]);
		}
	}
	if (device.vertex_shader)
	{
		unsigned long index;

		for (index = 0; index < device.vertex_shader->element_count; index++)
		{
			const struct vertex_element *element = &device.vertex_shader->elements[index];

			platform_log("    decl v%lu: stream %lu offset %lu type %02lx", (unsigned long)element->reg,
				(unsigned long)element->stream, (unsigned long)element->offset, (unsigned long)element->type);
		}
		for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
		{
			const float *value = device.attributes[index];

			if (value[0] || value[1] || value[2] || value[3] != 1.0f)
				platform_log("    current v%lu = %g %g %g %g", index, value[0], value[1], value[2], value[3]);
		}
	}
	if (first_vertex)
	{
		int reg;

		for (reg = 0; reg < XGPU_VERTEX_ATTRIBUTE_COUNT; reg++)
		{
			const float *v = first_vertex + reg * 4;

			if (v[0] || v[1] || v[2] || v[3] != 1.0f)
				platform_log("    v%d = %g %g %g %g", reg, v[0], v[1], v[2], v[3]);
		}
	}
}



/* the smallest and largest index of an index range the mirror holds: the
same ranges are drawn frame after frame */
#define INDEX_RANGE_SLOTS 4096

static struct
{
	unsigned long address;
	unsigned long count;
	unsigned long generation;
	WORD minimum;
	WORD maximum;
} index_ranges[INDEX_RANGE_SLOTS];

static void index_extent(const WORD *indices, unsigned long count, unsigned long generation, BOOL cached,
	unsigned long *minimum, unsigned long *maximum)
{
	unsigned long slot = (((unsigned long)indices >> 1) ^ (count * 2654435761UL)) % INDEX_RANGE_SLOTS;
	unsigned long index, low = 0xffff, high = 0;

	if (cached && index_ranges[slot].address == (unsigned long)indices && index_ranges[slot].count == count &&
		index_ranges[slot].generation == generation)
	{
		*minimum = index_ranges[slot].minimum;
		*maximum = index_ranges[slot].maximum;
		return;
	}
	for (index = 0; index < count; index++)
	{
		if (indices[index] < low)
			low = indices[index];
		if (indices[index] > high)
			high = indices[index];
	}
	if (cached)
	{
		index_ranges[slot].address = (unsigned long)indices;
		index_ranges[slot].count = count;
		index_ranges[slot].generation = generation;
		index_ranges[slot].minimum = (WORD)low;
		index_ranges[slot].maximum = (WORD)high;
	}
	*minimum = low;
	*maximum = high;
}

/* ---------- vertex data */

static metal_u32 vertex_format(unsigned char type)
{
	switch (type)
	{
	case D3DVSDT_FLOAT1: return METAL_VERTEX_FORMAT_FLOAT;
	case D3DVSDT_FLOAT2: return METAL_VERTEX_FORMAT_FLOAT2;
	case D3DVSDT_FLOAT3: case D3DVSDT_FLOAT2H: return METAL_VERTEX_FORMAT_FLOAT3;
	case D3DVSDT_FLOAT4: return METAL_VERTEX_FORMAT_FLOAT4;
	case D3DVSDT_D3DCOLOR: return METAL_VERTEX_FORMAT_UCHAR4_NORMALIZED_BGRA;
	case D3DVSDT_SHORT1: return METAL_VERTEX_FORMAT_SHORT;
	case D3DVSDT_SHORT2: return METAL_VERTEX_FORMAT_SHORT2;
	case D3DVSDT_SHORT3: return METAL_VERTEX_FORMAT_SHORT3;
	case D3DVSDT_SHORT4: return METAL_VERTEX_FORMAT_SHORT4;
	case D3DVSDT_NORMSHORT1: return METAL_VERTEX_FORMAT_SHORT_NORMALIZED;
	case D3DVSDT_NORMSHORT2: return METAL_VERTEX_FORMAT_SHORT2_NORMALIZED;
	case D3DVSDT_NORMSHORT3: return METAL_VERTEX_FORMAT_SHORT3_NORMALIZED;
	case D3DVSDT_NORMSHORT4: return METAL_VERTEX_FORMAT_SHORT4_NORMALIZED;
	case D3DVSDT_NORMPACKED3: return METAL_VERTEX_FORMAT_UINT;
	case D3DVSDT_PBYTE1: return METAL_VERTEX_FORMAT_UCHAR_NORMALIZED;
	case D3DVSDT_PBYTE2: return METAL_VERTEX_FORMAT_UCHAR2_NORMALIZED;
	case D3DVSDT_PBYTE3: return METAL_VERTEX_FORMAT_UCHAR3_NORMALIZED;
	case D3DVSDT_PBYTE4: return METAL_VERTEX_FORMAT_UCHAR4_NORMALIZED;
	default: return METAL_VERTEX_FORMAT_FLOAT4;
	}
}

/* the draw's vertices [first, first + count) of every stream the
declaration uses, and its layout: the host copies them, and they start at
vertex 0 of what it copied */
static void setup_streams(struct metal_draw *draw, unsigned long first, unsigned long count)
{
	struct vertex_shader_object *declaration = device.vertex_shader;
	struct metal_vertex_layout layout;
	unsigned long index, streams_used = 0;

	layout_constants(&layout, declaration->packed_mask);
	for (index = 0; index < declaration->element_count; index++)
	{
		const struct vertex_element *element = &declaration->elements[index];
		unsigned long stream = element->stream;
		unsigned long stride = device.streams[stream].stride;
		struct metal_vertex_attribute *attribute = &layout.attributes[element->reg];

		if (!device.streams[stream].data || element->type == D3DVSDT_NONE)
			continue;
		if (!(streams_used & (1UL << stream)))
		{
			unsigned long bytes = stride ? stride * count : 64;

			streams_used |= 1UL << stream;
			draw->streams[stream].data =
				GUEST_ADDRESS(PLATFORM_PHYSICAL_TO_VIRTUAL(device.streams[stream].data)) + (guest_address)(first * stride);
			draw->streams[stream].bytes = (metal_u32)bytes;
			stats.streamed_bytes += bytes;
			/* (a stride of 0: every vertex the first) */
			layout.buffers[stream].stride = stride ? (metal_u32)stride : 64;
			layout.buffers[stream].constant = stride == 0;
		}
		attribute->format = vertex_format(element->type);
		attribute->buffer = (metal_u32)stream;
		attribute->offset = element->offset;
	}
	/* (the same declaration's next draws mostly have the same layout) */
	{
		unsigned long hash = hash_words(&layout, sizeof(layout));

		if (!declaration->layout || declaration->layout_hash != hash)
			declaration->layout = layout_get(&layout, &declaration->layout_hash);
	}
	draw->vertex_layout = declaration->layout;
}

static metal_u32 primitive_type(D3DPRIMITIVETYPE type)
{
	switch (type)
	{
	case D3DPT_POINTLIST: return METAL_PRIMITIVE_POINT;
	case D3DPT_LINELIST: return METAL_PRIMITIVE_LINE;
	case D3DPT_LINELOOP:
	case D3DPT_LINESTRIP: return METAL_PRIMITIVE_LINE_STRIP;
	case D3DPT_TRIANGLESTRIP:
	case D3DPT_QUADSTRIP: return METAL_PRIMITIVE_TRIANGLE_STRIP;
	default: return METAL_PRIMITIVE_TRIANGLE;
	}
}

/* indices that draw what Metal does not draw itself - quads, fans and
polygons as triangles, a line loop as a strip back to its start - from a
draw's indices or (NULL) its vertices in order: TRUE with them (malloc'd,
NULL if there are none) in *result, FALSE for a primitive Metal draws */
static BOOL primitive_indices(D3DPRIMITIVETYPE type, const WORD *indices, unsigned long count, WORD **result,
	unsigned long *result_count)
{
	unsigned long index, out = 0;
	WORD *list = NULL;

#define SOURCE_INDEX(i) (indices ? indices[i] : (WORD)(i))
	*result = NULL;
	*result_count = 0;
	switch (type)
	{
	case D3DPT_QUADLIST:
		if (count >= 4)
			list = malloc((count / 4) * 6 * sizeof(WORD));
		for (index = 0; list && index + 3 < count; index += 4)
		{
			list[out++] = SOURCE_INDEX(index);
			list[out++] = SOURCE_INDEX(index + 1);
			list[out++] = SOURCE_INDEX(index + 2);
			list[out++] = SOURCE_INDEX(index);
			list[out++] = SOURCE_INDEX(index + 2);
			list[out++] = SOURCE_INDEX(index + 3);
		}
		break;
	case D3DPT_TRIANGLEFAN:
	case D3DPT_POLYGON:
		if (count >= 3)
			list = malloc((count - 2) * 3 * sizeof(WORD));
		for (index = 1; list && index + 1 < count; index++)
		{
			list[out++] = SOURCE_INDEX(0);
			list[out++] = SOURCE_INDEX(index);
			list[out++] = SOURCE_INDEX(index + 1);
		}
		break;
	case D3DPT_LINELOOP:
		if (count >= 2)
			list = malloc((count + 1) * sizeof(WORD));
		for (index = 0; list && index < count; index++)
			list[out++] = SOURCE_INDEX(index);
		if (list)
			list[out++] = SOURCE_INDEX(0);
		break;
	default:
		return FALSE;
	}
#undef SOURCE_INDEX
	*result = list;
	*result_count = out;
	return TRUE;
}

void WINAPI D3DDevice_SetStreamSource(UINT stream_number, D3DVertexBuffer *stream_data, UINT stride)
{
	if (stream_number >= 16)
		return;
	device.streams[stream_number].data = stream_data ? stream_data->Data : 0;
	device.streams[stream_number].stride = stride;
}

void WINAPI D3DDevice_SetIndices(D3DIndexBuffer *index_data, UINT base_vertex_index)
{
	device.base_vertex_index = base_vertex_index;
	D3D__IndexData = index_data ? (WORD *)index_data->Data : NULL;
}

void WINAPI D3DDevice_DrawVertices(D3DPRIMITIVETYPE primitive_type_value, UINT start_vertex, UINT vertex_count)
{
	struct metal_draw draw;
	WORD *indices;
	unsigned long count;

	if (!vertex_count || !prepare_draw(FALSE, &draw))
		return;
	trace_draw("draw", primitive_type_value, vertex_count, NULL);
	setup_streams(&draw, start_vertex, vertex_count);
	draw.primitive = primitive_type(primitive_type_value);
	if (primitive_indices(primitive_type_value, NULL, vertex_count, &indices, &count))
	{
		if (!indices)
			return;
		draw.indices = GUEST_ADDRESS(indices);
		draw.index_count = (metal_u32)count;
	}
	else
	{
		draw.vertex_count = vertex_count;
	}
	host_metal_draw(&draw);
	free(indices);
}

void WINAPI D3DDevice_DrawIndexedVertices(D3DPRIMITIVETYPE primitive_type_value, UINT vertex_count,
	CONST WORD *index_data)
{
	struct metal_draw draw;
	unsigned long minimum, maximum, count = vertex_count;
	WORD *indices;
	const WORD *source = index_data;

	if (!vertex_count || !index_data || !prepare_draw(FALSE, &draw))
		return;
	index_extent(index_data, vertex_count, 0, FALSE, &minimum, &maximum);
	trace_draw("indexed", primitive_type_value, vertex_count, NULL);
	/* (the streams from the base vertex on: index i is vertex base + i,
	which the host draws from vertex minimum) */
	setup_streams(&draw, device.base_vertex_index + minimum, maximum - minimum + 1);
	draw.primitive = primitive_type(primitive_type_value);
	if (primitive_indices(primitive_type_value, index_data, vertex_count, &indices, &count))
	{
		if (!indices)
			return;
		source = indices;
	}
	else
	{
		/* (a primitive Metal draws: the draw's own indices, all of them) */
		count = vertex_count;
	}
	draw.indices = GUEST_ADDRESS(source);
	draw.index_count = (metal_u32)count;
	draw.base_vertex = (metal_u32)minimum;
	host_metal_draw(&draw);
	free(indices);
}

/* ---------- immediate mode */

void WINAPI D3DDevice_Begin(D3DPRIMITIVETYPE primitive_type)
{
	device.immediate_active = TRUE;
	device.immediate_type = primitive_type;
	device.immediate_count = 0;
	device.immediate_written = 0;
}

static void immediate_emit(void)
{
	unsigned long floats = XGPU_VERTEX_ATTRIBUTE_COUNT * 4;

	if (device.immediate_count == device.immediate_capacity)
	{
		unsigned long capacity = device.immediate_capacity ? device.immediate_capacity * 2 : 256;
		float *vertices = realloc(device.immediate_vertices, capacity * floats * sizeof(float));

		/* (out of memory: the vertex is dropped) */
		if (!vertices)
			return;
		device.immediate_vertices = vertices;
		device.immediate_capacity = capacity;
	}
	memcpy(device.immediate_vertices + device.immediate_count * floats, device.attributes, floats * sizeof(float));
	device.immediate_count++;
}

void WINAPI D3DDevice_End(void)
{
	/* the registers written since Begin, one after another in each vertex,
	from a buffer of their own; the others are constants (most draws write
	3 of the 16, and a vertex of all of them is 256 bytes) */
	static float *packed;
	static unsigned long packed_capacity;
	unsigned long attribute_size = 4 * sizeof(float);
	unsigned long registers[XGPU_VERTEX_ATTRIBUTE_COUNT];
	unsigned long register_count = 0, vertex_size, vertex, slot, index, count = device.immediate_count;
	D3DPRIMITIVETYPE type = device.immediate_type;
	struct metal_draw draw;
	struct metal_vertex_layout layout;
	WORD *indices;
	unsigned long index_count;

	device.immediate_active = FALSE;
	if (!count || !prepare_draw(TRUE, &draw))
		return;
	trace_draw("immediate", type, count, device.immediate_vertices);
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
	{
		if (device.immediate_written & (1UL << index))
			registers[register_count++] = index;
	}
	if (!register_count)
		return;
	vertex_size = register_count * attribute_size;
	if (count * register_count > packed_capacity)
	{
		float *grown = realloc(packed, count * vertex_size);

		/* (out of memory: the draw is dropped) */
		if (!grown)
			return;
		packed = grown;
		packed_capacity = count * register_count;
	}
	for (vertex = 0; vertex < count; vertex++)
	{
		for (slot = 0; slot < register_count; slot++)
		{
			memcpy(packed + (vertex * register_count + slot) * 4,
				device.immediate_vertices + (vertex * XGPU_VERTEX_ATTRIBUTE_COUNT + registers[slot]) * 4,
				attribute_size);
		}
	}
	draw.streams[0].data = GUEST_ADDRESS(packed);
	draw.streams[0].bytes = (metal_u32)(count * vertex_size);
	stats.streamed_bytes += count * vertex_size;
	{
		/* (the vertex layout of the last few sets of registers) */
		static struct
		{
			unsigned long written;
			unsigned int layout;
		} layouts[8];
		static unsigned long next_layout;

		draw.vertex_layout = 0;
		for (slot = 0; slot < 8; slot++)
		{
			if (layouts[slot].layout && layouts[slot].written == device.immediate_written)
				draw.vertex_layout = layouts[slot].layout;
		}
		if (!draw.vertex_layout)
		{
			layout_constants(&layout, 0);
			for (slot = 0; slot < register_count; slot++)
			{
				layout.attributes[registers[slot]].format = METAL_VERTEX_FORMAT_FLOAT4;
				layout.attributes[registers[slot]].buffer = 0;
				layout.attributes[registers[slot]].offset = (metal_u32)(slot * attribute_size);
			}
			layout.buffers[0].stride = (metal_u32)vertex_size;
			draw.vertex_layout = layout_get(&layout, NULL);
			layouts[next_layout % 8].written = device.immediate_written;
			layouts[next_layout % 8].layout = draw.vertex_layout;
			next_layout++;
		}
	}
	draw.primitive = primitive_type(type);
	if (primitive_indices(type, NULL, count, &indices, &index_count))
	{
		if (!indices)
			return;
		draw.indices = GUEST_ADDRESS(indices);
		draw.index_count = (metal_u32)index_count;
	}
	else
	{
		draw.vertex_count = (metal_u32)count;
	}
	host_metal_draw(&draw);
	free(indices);
}

static void set_attribute(INT reg, float a, float b, float c, float d)
{
	BOOL emit = FALSE;

	if (reg == D3DVSDE_VERTEX)
	{
		reg = 0;
		emit = TRUE;
	}
	if (reg < 0 || reg >= XGPU_VERTEX_ATTRIBUTE_COUNT)
		return;
	device.attributes[reg][0] = a;
	device.attributes[reg][1] = b;
	device.attributes[reg][2] = c;
	device.attributes[reg][3] = d;
	if (device.immediate_active)
		device.immediate_written |= 1UL << reg;
	/* like the hardware, writing register 0 completes a vertex */
	if (device.immediate_active && (emit || reg == 0))
		immediate_emit();
}

void WINAPI D3DDevice_SetVertexData2f(INT reg, FLOAT a, FLOAT b)
{
	set_attribute(reg, a, b, 0.0f, 1.0f);
}

void WINAPI D3DDevice_SetVertexData4f(INT reg, FLOAT a, FLOAT b, FLOAT c, FLOAT d)
{
	set_attribute(reg, a, b, c, d);
}

void WINAPI D3DDevice_SetVertexData2s(INT reg, SHORT a, SHORT b)
{
	set_attribute(reg, (float)a, (float)b, 0.0f, 1.0f);
}

void WINAPI D3DDevice_SetVertexData4ub(INT reg, BYTE a, BYTE b, BYTE c, BYTE d)
{
	set_attribute(reg, a / 255.0f, b / 255.0f, c / 255.0f, d / 255.0f);
}

void WINAPI D3DDevice_SetVertexDataColor(INT reg, D3DCOLOR color)
{
	float value[4];

	color_to_vec4(color, value);
	set_attribute(reg, value[0], value[1], value[2], value[3]);
}

/* ---------- clearing */

/* a clear's rectangle, x0, y0 to x1, y1 in the targets' pixels, inside
them (the host's) */
static void clear_rectangle(struct metal_clear *clear, GLint x0, GLint y0, GLint x1, GLint y1)
{
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	clear->rectangle[0] = (metal_u32)x0;
	clear->rectangle[1] = (metal_u32)y0;
	clear->rectangle[2] = x1 > x0 ? (metal_u32)(x1 - x0) : 0;
	clear->rectangle[3] = y1 > y0 ? (metal_u32)(y1 - y0) : 0;
}

void WINAPI D3DDevice_Clear(DWORD count, CONST D3DRECT *rectangles, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
	struct metal_clear clear;
	BOOL has_depth = FALSE;
	DWORD index;

	if (!device.ready || !bind_targets(&has_depth))
		return;
	if (trace_frame())
		platform_log("clear flags %lx color %08lx z %g count %lu target %08lx depth %08lx", (unsigned long)flags,
			(unsigned long)color, z, (unsigned long)count,
			device.render_target ? (unsigned long)device.render_target->Data : 0,
			device.depth_stencil ? (unsigned long)device.depth_stencil->Data : 0);
	stats.clears++;
	memset(&clear, 0, sizeof(clear));
	/* the Xbox clears the channels named (D3DCLEAR_TARGET_R, _G, _B, _A):
	the fog screen clears only alpha, leaving the picture under the fog */
	if (flags & D3DCLEAR_TARGET)
	{
		color_to_vec4(color, clear.color);
		clear.color_mask = ((flags & D3DCLEAR_TARGET_R) ? METAL_WRITE_RED : 0) |
			((flags & D3DCLEAR_TARGET_G) ? METAL_WRITE_GREEN : 0) |
			((flags & D3DCLEAR_TARGET_B) ? METAL_WRITE_BLUE : 0) | ((flags & D3DCLEAR_TARGET_A) ? METAL_WRITE_ALPHA : 0);
	}
	clear.depth = has_depth && (flags & D3DCLEAR_ZBUFFER);
	clear.depth_value = z;
	clear.stencil = has_depth && (flags & D3DCLEAR_STENCIL);
	clear.stencil_value = stencil & 0xff;
	if (!clear.color_mask && !clear.depth && !clear.stencil)
		return;
	if (!count || !rectangles)
	{
		/* the NV2A clips a viewport-less clear to the viewport, which is what
		keeps a split-screen window's clear from wiping the other window */
		clear_rectangle(&clear, target_pixel((float)device.viewport.X, 0), target_pixel((float)device.viewport.Y, 1),
			target_pixel((float)(device.viewport.X + device.viewport.Width), 0),
			target_pixel((float)(device.viewport.Y + device.viewport.Height), 1));
		/* (a scene's first clear, of its view, in the scene target: the whole
		of it, which the host makes the pass's load action rather than a
		quad over its part and a load of the rest. Nothing outside the view's
		part is read: each view is scaled up at its scene's end, before the
		next view's scene begins) */
		if (scene.active && scene.first_clear && device.render_target &&
			device.render_target->Data == device.back_buffer.Data)
		{
			scene.first_clear = FALSE;
			clear.rectangle[0] = clear.rectangle[1] = 0;
			clear.rectangle[2] = (metal_u32)scene.width;
			clear.rectangle[3] = (metal_u32)scene.height;
		}
		host_metal_clear(&clear);
		return;
	}
	for (index = 0; index < count; index++)
	{
		INT left = rectangles[index].x1 > device.viewport.X ? rectangles[index].x1 : device.viewport.X;
		INT top = rectangles[index].y1 > device.viewport.Y ? rectangles[index].y1 : device.viewport.Y;
		INT right = rectangles[index].x2 < device.viewport.X + device.viewport.Width ?
			rectangles[index].x2 : device.viewport.X + device.viewport.Width;
		INT bottom = rectangles[index].y2 < device.viewport.Y + device.viewport.Height ?
			rectangles[index].y2 : device.viewport.Y + device.viewport.Height;

		if (left >= right || top >= bottom)
			continue;
		clear_rectangle(&clear, target_pixel((float)(left + UI_OFFSET), 0), target_pixel((float)top, 1),
			target_pixel((float)(right + UI_OFFSET), 0), target_pixel((float)bottom, 1));
		host_metal_clear(&clear);
	}
}

/* ---------- the anti-aliasing passes */

/* display.dynamic_resolution's settings, read once (the host's controller
keeps them: a change applies at the next launch) */
static void scene_configure(void)
{
	const char *mode;
	long floor, split_floor;

	if (scene.configured)
		return;
	scene.configured = TRUE;
	mode = config_string("display.dynamic_resolution");
	floor = config_integer("display.dynamic_resolution_floor");
	split_floor = config_integer("display.dynamic_resolution_split_floor");
	scene.sharpen = !strcmp(config_string("display.dynamic_resolution_filter"), "sharp");
	/* ("auto": the host's call, on on the Apple TV: -1) */
	host_dynres_settings(!strcmp(mode, "on") || !strcmp(mode, "true") ? 1 : !strcmp(mode, "auto") ? -1 : 0, (int)floor,
		(int)split_floor, scene.sharpen, !strcmp(config_string("display.dynamic_resolution_ceiling"), "render"));
	scene.frame_fraction = -1.0f;
}

/* the start of a player view's 3D scene (source/render/render.c): drawn at
the frame's fraction of the screen's resolution, in the scene target, when
it is less than the whole */
void halo_screen_scene_begin(short x0, short y0, short x1, short y1, short views)
{
	struct render_target_entry *back;

	if (!device.ready)
		return;
	scene_configure();
	scene.active = FALSE;
	back = render_target_get(&device.back_buffer);
	if (!back || back->target.depth)
		return;
	if (scene.frame_fraction < 0.0f)
		scene.frame_fraction = host_dynres_fraction(views, (int)back->target.gl_height);
	/* (more than 1: the whole, drawn through the scene target all the same,
	which only HALO_DYNRES_FIXED asks for: the check that the scene target's
	path draws the same pixels, PERFORMANCE.md) */
	if (scene.frame_fraction == 1.0f || scene.frame_fraction <= 0.0f)
		return;
	/* the scene target: the back buffer's size (made again only when it is
	another size: resolution_scaling or the window changed) */
	if (!scene.texture || scene.width != back->target.gl_width || scene.height != back->target.gl_height)
	{
		if (!scene.texture)
			scene.texture = host_metal_texture_new();
		scene.width = back->target.gl_width;
		scene.height = back->target.gl_height;
		host_metal_texture_define(scene.texture, _metal_texture_2d, METAL_FORMAT_BGRA8, (unsigned int)scene.width,
			(unsigned int)scene.height, 1, 1, _metal_usage_color_target, METAL_SWIZZLE_IDENTITY);
	}
	scene_entry = *back;
	scene_entry.next = scene_entry.next_in_bucket = NULL;
	scene_entry.target.texture = scene.texture;
	scene.fraction = scene.frame_fraction > 1.0f ? 1.0f : scene.frame_fraction;
	scene_entry.target.scale[0] = back->target.scale[0] * scene.fraction;
	scene_entry.target.scale[1] = back->target.scale[1] * scene.fraction;
	scene_entry.target.written = ++render_target_write_serial;
	scene.view[0] = x0;
	scene.view[1] = y0;
	scene.view[2] = x1;
	scene.view[3] = y1;
	scene.active = TRUE;
	scene.first_clear = TRUE;
	/* (what the recent lookups found was the back buffer's) */
	memset(recent_render_targets, 0, sizeof(recent_render_targets));
}

/* the end of a player view's scene, before the HUD and menus are drawn
over it (source/render/render.c): a scene drawn at less than the whole is
scaled up into the back buffer. (display.anti_aliasing's passes are not
drawn with Metal yet: anti_aliasing_prepare says so.) */
void halo_screen_anti_alias(short x0, short y0, short x1, short y1)
{
	struct render_target_entry *back;
	struct metal_upscale upscale;
	float scale_x, scale_y;

	(void)x0;
	(void)y0;
	(void)x1;
	(void)y1;
	if (!scene.active)
		return;
	scene.active = FALSE;
	memset(recent_render_targets, 0, sizeof(recent_render_targets));
	back = render_target_get(&device.back_buffer);
	if (!back || back->target.depth)
		return;
	back->last_rendered = device.frame + 1;
	back->target.written = ++render_target_write_serial;
	memset(&upscale, 0, sizeof(upscale));
	upscale.scene = scene.texture;
	upscale.back_buffer = back->target.texture;
	{
		struct render_target_entry *depth = render_target_get(&device.depth_buffer);

		upscale.depth = depth && depth->target.depth ? depth->target.texture : 0;
	}
	/* the view's rectangle: in the back buffer at its scale, in the scene
	target at the scene's (scaled_pixel, as the viewports were) */
	scale_x = back->target.scale[0];
	scale_y = back->target.scale[1];
	{
		GLint dx0 = scaled_pixel((float)scene.view[0], scale_x), dy0 = scaled_pixel((float)scene.view[1], scale_y);
		GLint dx1 = scaled_pixel((float)scene.view[2], scale_x), dy1 = scaled_pixel((float)scene.view[3], scale_y);
		GLint sx0 = scaled_pixel((float)scene.view[0], scene_entry.target.scale[0]);
		GLint sy0 = scaled_pixel((float)scene.view[1], scene_entry.target.scale[1]);
		GLint sx1 = scaled_pixel((float)scene.view[2], scene_entry.target.scale[0]);
		GLint sy1 = scaled_pixel((float)scene.view[3], scene_entry.target.scale[1]);

		if (dx0 < 0)
			dx0 = 0;
		if (dy0 < 0)
			dy0 = 0;
		if (sx0 < 0)
			sx0 = 0;
		if (sy0 < 0)
			sy0 = 0;
		if (dx1 <= dx0 || dy1 <= dy0 || sx1 <= sx0 || sy1 <= sy0)
			return;
		upscale.source[0] = (metal_u32)sx0;
		upscale.source[1] = (metal_u32)sy0;
		upscale.source[2] = (metal_u32)(sx1 - sx0);
		upscale.source[3] = (metal_u32)(sy1 - sy0);
		upscale.destination[0] = (metal_u32)dx0;
		upscale.destination[1] = (metal_u32)dy0;
		upscale.destination[2] = (metal_u32)(dx1 - dx0);
		upscale.destination[3] = (metal_u32)(dy1 - dy0);
	}
	upscale.filter = scene.sharpen ? METAL_UPSCALE_SHARP : METAL_UPSCALE_BILINEAR;
	upscale.sharpness = 0.5f;
	host_metal_scene_upscale(&upscale);
}

/* ---------- presentation */

/* debug.screenshot_every's frames, BMPs in debug.screenshot_directory: the
back buffer as the GPU drew it (the frame so far waited for) */
static void write_screenshot(struct render_target_entry *target)
{
	const char *directory = *config_string("debug.screenshot_directory") ?
		config_string("debug.screenshot_directory") : NULL;
	unsigned long width = target->target.gl_width, height = target->target.gl_height;
	unsigned long image_size = width * height * 4, pixel;
	unsigned char header[54] = { 'B', 'M' };
	unsigned char *pixels;
	char path[512];
	FILE *file;

	if (!directory || !(pixels = malloc(image_size)))
		return;
	if (!host_metal_texture_read(target->target.texture, (unsigned int)width, (unsigned int)height, pixels))
	{
		free(pixels);
		return;
	}
	/* the display ignores destination alpha, which the game uses as
	scratch; image viewers would show it as transparency */
	for (pixel = 0; pixel < width * height; pixel++)
		pixels[pixel * 4 + 3] = 0xff;
	snprintf(path, sizeof(path), "%s/frame%05lu.bmp", directory, device.frame);
	file = fopen(path, "wb");
	if (file)
	{
		*(unsigned int *)(header + 2) = (unsigned int)(54 + image_size);
		*(unsigned int *)(header + 10) = 54;
		*(unsigned int *)(header + 14) = 40;
		*(int *)(header + 18) = (int)width;
		*(int *)(header + 22) = -(int)height; /* rows from the top, as read */
		*(unsigned short *)(header + 26) = 1;
		*(unsigned short *)(header + 28) = 32;
		*(unsigned int *)(header + 34) = (unsigned int)image_size;
		fwrite(header, 1, sizeof(header), file);
		fwrite(pixels, 1, image_size, file);
		fclose(file);
	}
	free(pixels);
}

void WINAPI D3DDevice_Present(CONST RECT *source_rectangle, CONST RECT *destination_rectangle,
	void *unused, void *unused2)
{
	static long screenshot_every = -1;

	(void)source_rectangle;
	(void)destination_rectangle;
	(void)unused;
	(void)unused2;
	if (screenshot_every < 0)
		screenshot_every = config_integer("debug.screenshot_every");

	if (device.ready)
	{
		struct render_target_entry *back_buffer;

		/* (the fraction this frame's scenes were drawn at, -1 for none: the
		controller's; the next frame asks again. A scene never ended, which
		the game does not do, is scaled up first) */
		if (scene.configured)
		{
			if (scene.active)
				halo_screen_anti_alias(0, 0, 0, 0);
			host_dynres_frame(scene.frame_fraction > 1.0f ? 1.0f : scene.frame_fraction);
			scene.frame_fraction = -1.0f;
		}
		back_buffer = render_target_get(&device.back_buffer);
		if (trace_frame())
			platform_log("present back buffer %08lx texture %u", (unsigned long)device.back_buffer.Data,
				back_buffer ? back_buffer->target.texture : 0);
		if (back_buffer && screenshot_every > 0 && device.frame % (unsigned long)screenshot_every == 0)
			write_screenshot(back_buffer);
		if (platform_screenshot_take_request())
			console_printf(FALSE, "Screenshots are not taken on macOS yet");
		/* the back buffer, letterboxed to its shape in the window */
		if (back_buffer)
		{
			host_metal_present_target(back_buffer->target.texture, (unsigned int)back_buffer->target.gl_width,
				(unsigned int)back_buffer->target.gl_height);
		}
		platform_video_swap();
		memory_watch_begin_frame();
		xgpu_texture_cache_begin_frame();
	}
	else
	{
		platform_video_swap();
		memory_watch_begin_frame();
	}
	device.frame++;
	stats.presents++;
	if (debug_settings.statistics && device.frame % 60 == 0)
	{
		platform_log("frame %lu: %lu draws, %lu immediate, %lu clears, %lu target changes; skipped %lu no program, "
			"%lu no target, %lu shader; %lu KB streamed",
			device.frame, stats.draws / stats.presents, stats.immediate_draws / stats.presents, stats.clears / stats.presents,
			stats.target_changes / stats.presents, stats.skipped_no_program, stats.skipped_no_target, stats.skipped_link,
			stats.streamed_bytes / stats.presents / 1024);
		memset(&stats, 0, sizeof(stats));
	}
	platform_pump_events();

	pthread_mutex_lock(&vertical_blank_lock);
	/* the Xbox keeps at most two frames queued behind its 60 Hz display;
	with interpolation, frames come at the real display's rate instead,
	paced by vsync (platform_video_swap) */
	if (halo_interpolation_enabled())
	{
		flip_count++;
	}
	else
	{
		while (pending_flips >= 2)
			pthread_cond_wait(&vertical_blank_condition, &vertical_blank_lock);
		pending_flips++;
	}
	pthread_mutex_unlock(&vertical_blank_lock);
}

HRESULT WINAPI D3DDevice_PersistDisplay(void)
{
	return S_OK;
}
