/*
METAL_PROTOCOL.H

What the macOS build's Direct3D device (port/macos/guest/d3d8_metal.c, in
the guest) hands the Metal renderer (port/macos/host/host_metal.m, in the
host) for each draw, clear and texture: structs of 32-bit integers and
floats only, so that they have the same layout in the guest's wasm32 and
the host's arm64; a guest pointer in one is its offset into the guest's
memory (guest_address), which the host turns into a pointer. Metal's
objects (textures, shaders, vertex layouts) are small handles the host
gives out.

Enumerations are Metal's own values (MTLPixelFormat, MTLCompareFunction and
the rest), which d3d8_metal.c translates the Xbox's into.
*/

#ifndef __HALO_METAL_PROTOCOL_H
#define __HALO_METAL_PROTOCOL_H

typedef unsigned int metal_u32;
typedef unsigned int guest_address;

/* ---------- Metal's values (the subset the device uses) */

#define METAL_FORMAT_BGRA8 80
#define METAL_FORMAT_RGBA8 70
#define METAL_FORMAT_DEPTH32_STENCIL8 260
#define METAL_FORMAT_BC1 130
#define METAL_FORMAT_BC2 132
#define METAL_FORMAT_BC3 134
#define METAL_FORMAT_ASTC_4X4 204

#define METAL_VERTEX_FORMAT_INVALID 0
#define METAL_VERTEX_FORMAT_FLOAT 28
#define METAL_VERTEX_FORMAT_FLOAT2 29
#define METAL_VERTEX_FORMAT_FLOAT3 30
#define METAL_VERTEX_FORMAT_FLOAT4 31
#define METAL_VERTEX_FORMAT_UCHAR4_NORMALIZED_BGRA 42
#define METAL_VERTEX_FORMAT_SHORT 50
#define METAL_VERTEX_FORMAT_SHORT2 16
#define METAL_VERTEX_FORMAT_SHORT3 17
#define METAL_VERTEX_FORMAT_SHORT4 18
#define METAL_VERTEX_FORMAT_SHORT_NORMALIZED 52
#define METAL_VERTEX_FORMAT_SHORT2_NORMALIZED 22
#define METAL_VERTEX_FORMAT_SHORT3_NORMALIZED 23
#define METAL_VERTEX_FORMAT_SHORT4_NORMALIZED 24
#define METAL_VERTEX_FORMAT_UCHAR_NORMALIZED 47
#define METAL_VERTEX_FORMAT_UCHAR2_NORMALIZED 7
#define METAL_VERTEX_FORMAT_UCHAR3_NORMALIZED 8
#define METAL_VERTEX_FORMAT_UCHAR4_NORMALIZED 9
#define METAL_VERTEX_FORMAT_UINT 36

/* ---------- telemetry (port/macos/host/host_telemetry.h): the game's
state, once a frame (source/main/main.c's main_game_render) */

#define METAL_TELEMETRY_VIEWS 4
struct metal_telemetry_game
{
	/* game_time_get's ticks (-1: no game), local players, player views
	drawn (1 in a cinematic), the structure BSP's index (-1: none), a
	cinematic playing */
	int game_ticks, local_players, windows, bsp, cinematic, reserved;
	/* each view's camera */
	float positions[METAL_TELEMETRY_VIEWS][3];
	/* the scenario's name, its last part (\"a10\") */
	char map[32];
};

#define METAL_PRIMITIVE_POINT 0
#define METAL_PRIMITIVE_LINE 1
#define METAL_PRIMITIVE_LINE_STRIP 2
#define METAL_PRIMITIVE_TRIANGLE 3
#define METAL_PRIMITIVE_TRIANGLE_STRIP 4

#define METAL_CULL_NONE 0
#define METAL_CULL_FRONT 1
#define METAL_CULL_BACK 2
#define METAL_WINDING_CLOCKWISE 0
#define METAL_WINDING_COUNTER_CLOCKWISE 1

#define METAL_COMPARE_NEVER 0
#define METAL_COMPARE_ALWAYS 7

#define METAL_STENCIL_KEEP 0
#define METAL_STENCIL_ZERO 1
#define METAL_STENCIL_REPLACE 2
#define METAL_STENCIL_INCREMENT_CLAMP 3
#define METAL_STENCIL_DECREMENT_CLAMP 4
#define METAL_STENCIL_INVERT 5
#define METAL_STENCIL_INCREMENT_WRAP 6
#define METAL_STENCIL_DECREMENT_WRAP 7

#define METAL_BLEND_ZERO 0
#define METAL_BLEND_ONE 1
#define METAL_BLEND_SOURCE_COLOR 2
#define METAL_BLEND_ONE_MINUS_SOURCE_COLOR 3
#define METAL_BLEND_SOURCE_ALPHA 4
#define METAL_BLEND_ONE_MINUS_SOURCE_ALPHA 5
#define METAL_BLEND_DESTINATION_COLOR 6
#define METAL_BLEND_ONE_MINUS_DESTINATION_COLOR 7
#define METAL_BLEND_DESTINATION_ALPHA 8
#define METAL_BLEND_ONE_MINUS_DESTINATION_ALPHA 9
#define METAL_BLEND_SOURCE_ALPHA_SATURATED 10
#define METAL_BLEND_BLEND_COLOR 11
#define METAL_BLEND_ONE_MINUS_BLEND_COLOR 12
#define METAL_BLEND_BLEND_ALPHA 13
#define METAL_BLEND_ONE_MINUS_BLEND_ALPHA 14

#define METAL_BLEND_OPERATION_ADD 0
#define METAL_BLEND_OPERATION_SUBTRACT 1
#define METAL_BLEND_OPERATION_REVERSE_SUBTRACT 2
#define METAL_BLEND_OPERATION_MIN 3
#define METAL_BLEND_OPERATION_MAX 4

#define METAL_FILTER_NEAREST 0
#define METAL_FILTER_LINEAR 1
#define METAL_MIP_NONE 0
#define METAL_MIP_NEAREST 1
#define METAL_MIP_LINEAR 2
#define METAL_ADDRESS_CLAMP_TO_EDGE 0
#define METAL_ADDRESS_REPEAT 2
#define METAL_ADDRESS_MIRROR_REPEAT 3
#define METAL_ADDRESS_CLAMP_TO_BORDER 5
#define METAL_BORDER_TRANSPARENT_BLACK 0
#define METAL_BORDER_OPAQUE_BLACK 1
#define METAL_BORDER_OPAQUE_WHITE 2

#define METAL_SWIZZLE_ZERO 0
#define METAL_SWIZZLE_ONE 1
#define METAL_SWIZZLE_RED 2
#define METAL_SWIZZLE_GREEN 3
#define METAL_SWIZZLE_BLUE 4
#define METAL_SWIZZLE_ALPHA 5
/* (red, green, blue, alpha from themselves: the swizzle word of none) */
#define METAL_SWIZZLE_IDENTITY (2u | (3u << 8) | (4u << 16) | (5u << 24))

/* color write mask bits (MTLColorWriteMask) */
#define METAL_WRITE_RED 8
#define METAL_WRITE_GREEN 4
#define METAL_WRITE_BLUE 2
#define METAL_WRITE_ALPHA 1

/* ---------- textures */

enum
{
	_metal_texture_2d,
	_metal_texture_cube,
	_metal_texture_3d,
};

/* the blocks of a DXT texture (host_metal_texture_bc) */
enum
{
	_metal_bc1,
	_metal_bc2,
	_metal_bc3,
};

/* what a GPU without BC formats converts them into */
enum
{
	/* the texels the guest's decoding gives */
	_metal_bc_exact,
	/* ASTC 4x4: a quarter of the bytes, not always the same texels */
	_metal_bc_astc,
};

enum
{
	/* sampled only */
	_metal_usage_sampled,
	/* a render target, sampled too */
	_metal_usage_color_target,
	_metal_usage_depth_target,
};

/* ---------- vertex layouts

A vertex shader reads all sixteen attributes; those no stream gives read
the constant attribute buffer (XGPU_METAL_VERTEX_CONSTANT_ATTRIBUTES_BUFFER:
their values, sixteen float4s, as constants). */

#define METAL_VERTEX_ATTRIBUTES 16
#define METAL_VERTEX_BUFFERS 17

struct metal_vertex_attribute
{
	metal_u32 format;
	metal_u32 buffer;
	metal_u32 offset;
};

struct metal_vertex_buffer_layout
{
	/* 0: not used; a stride of 0 reads the same vertex for every one */
	metal_u32 stride;
	/* 1: every vertex reads the first */
	metal_u32 constant;
};

struct metal_vertex_layout
{
	struct metal_vertex_attribute attributes[METAL_VERTEX_ATTRIBUTES];
	struct metal_vertex_buffer_layout buffers[METAL_VERTEX_BUFFERS];
};

/* ---------- a draw's state */

struct metal_sampler
{
	metal_u32 min_filter, mag_filter, mip_filter;
	metal_u32 address_u, address_v, address_w;
	metal_u32 border;
	metal_u32 anisotropy;
	float min_lod;
};

struct metal_blend
{
	metal_u32 enabled;
	metal_u32 source, destination, operation;
	metal_u32 write_mask;
};

struct metal_depth_stencil
{
	metal_u32 depth_test, depth_function, depth_write;
	metal_u32 stencil_test, stencil_function;
	metal_u32 stencil_fail, stencil_depth_fail, stencil_pass;
	metal_u32 stencil_read_mask, stencil_write_mask;
};

/* a vertex stream: the guest's bytes the draw reads, which the host copies
into the frame's buffer (offset 0 of it is the first vertex the draw
draws) */
struct metal_stream
{
	guest_address data;
	metal_u32 bytes;
};

#define METAL_STAGES 4

/* the uniform buffers' sizes (xgpu.h's xgpu_vertex_uniforms, the 192
vertex constants, the sixteen constant attributes, xgpu_pixel_uniforms) */
#define METAL_VERTEX_UNIFORMS_BYTES 48
#define METAL_VERTEX_CONSTANTS_BYTES (192 * 16)
/* (sixteen float4s, the input registers' values, and a zero after them,
which an unbound NORMPACKED3 attribute reads) */
#define METAL_CONSTANT_ATTRIBUTES_BYTES (17 * 16)
#define METAL_PIXEL_UNIFORMS_BYTES 752

/* the buffer indices the shaders read them from (xgpu.h) */
#define METAL_BUFFER_CONSTANT_ATTRIBUTES 16
#define METAL_BUFFER_VERTEX_UNIFORMS 17
#define METAL_BUFFER_VERTEX_CONSTANTS 18
#define METAL_BUFFER_PIXEL_UNIFORMS 0

struct metal_draw
{
	/* the program: a vertex and a fragment shader, and the vertex layout
	(host_metal_shader, host_metal_vertex_layout) */
	metal_u32 vertex_shader, fragment_shader, vertex_layout;
	struct metal_blend blend;
	float blend_color[4];
	struct metal_depth_stencil depth_stencil;
	metal_u32 stencil_reference;
	metal_u32 cull, winding, wireframe;
	float depth_bias, depth_slope;
	/* in the target's pixels, from its top left; the scissor the same */
	float viewport[6];
	metal_u32 scissor[4];

	struct metal_stream streams[METAL_VERTEX_BUFFERS - 1];
	/* the constant attributes (16 float4s), and the uniforms: each
	re-sent only when its serial differs from the last draw's */
	guest_address constant_attributes;
	guest_address vertex_uniforms, vertex_constants, pixel_uniforms;
	metal_u32 constant_attributes_serial, vertex_uniforms_serial, vertex_constants_serial, pixel_uniforms_serial;

	/* each stage's texture (0: none), and the kind the pixel shader reads
	(_metal_texture_*), which a stage without one gets an empty one of */
	metal_u32 textures[METAL_STAGES];
	metal_u32 texture_types[METAL_STAGES];
	struct metal_sampler samplers[METAL_STAGES];

	metal_u32 primitive;
	metal_u32 vertex_count;
	/* indexed (16-bit indices, of index_count, in the guest's memory),
	each index read from the first vertex of the streams (base_vertex is
	what the streams' first vertex is numbered: indices are less it) */
	guest_address indices;
	metal_u32 index_count;
	metal_u32 base_vertex;
	/* the visibility test it counts for (host_metal_visibility_begin), or ~0 */
	metal_u32 visibility_test;
};

/* a clear: the rectangle of the targets (pixels, from the top left), what
it clears (METAL_WRITE_* of the color, and depth and stencil) */
struct metal_clear
{
	metal_u32 rectangle[4];
	metal_u32 color_mask;
	metal_u32 depth, stencil;
	float color[4];
	float depth_value;
	metal_u32 stencil_value;
};

/* visibility tests: a frame's tests (host_metal_visibility_begin), and the
game's indices for them (D3DDevice_EndVisibilityTest's, modulo this) */
#define METAL_VISIBILITY_SLOTS 4096

#endif
