/*
RASTERIZER_XBOX_SHADOWS.C

symbols in this file:
00161950 01b0:
	_D3DDevice_SetRenderState (0000)
00161B00 0050:
	_D3DDevice_SetTextureStageState (0000)
00161B50 0220:
	_IDirect3DDevice8_SetRenderState@12 (0000)
00161D70 0010:
	__rasterizer_environment_shadows_begin (0000)
00161D80 0060:
	_IDirect3DDevice8_SetTextureStageState@16 (0000)
00161DE0 0010:
	_IDirect3DDevice8_SetVertexShaderConstant@16 (0000)
00161DF0 00a0:
	__rasterizer_environment_shadow_model_begin (0000)
00161E90 0010:
	__rasterizer_environment_shadow_model_end (0000)
00161EA0 0020:
	_IDirect3DDevice8_SetVertexData2f@16 (0000)
00161EC0 0010:
	_IDirect3DDevice8_SetVertexData2s@16 (0000)
00161ED0 0010:
	_IDirect3DDevice8_Begin@8 (0000)
00161EE0 0010:
	_IDirect3DDevice8_End@4 (0000)
00161EF0 0080:
	__rasterizer_environment_shadow_end (0000)
00161F70 0010:
	__rasterizer_environment_shadows_end (0000)
00161F80 0300:
	_rasterizer_shadow_convolve (0000)
00162280 03b0:
	__rasterizer_environment_shadow_begin (0000)
00162630 02b0:
	__rasterizer_environment_shadow_model_draw (0000)
001628E0 05c0:
	__rasterizer_environment_shadow_draw (0000)
002929E0 0039:
	??_C@_0DJ@KIPLBCDL@c?3?2halo?2SOURCE?2rasterizer?2xbox?2r@ (0000)
00292A1C 0027:
	??_C@_0CH@OGPNPLFF@?$CD?$CD?$CD?5WARNING?5empty?5shadow?5has?5bee@ (0000)
00292A44 001c:
	??_C@_0BM@OJNHCFOK@object_bounding_radius?$DO0?40f?$AA@ (0000)
00292A60 0037:
	??_C@_0DH@LMLPNHOO@shadow_color?9?$DOblue?5?$DO?$DN0?40f?5?$CG?$CG?5sha@ (0000)
00292A98 0037:
	??_C@_0DH@OODBCLKP@shadow_color?9?$DOgreen?$DO?$DN0?40f?5?$CG?$CG?5sha@ (0000)
00292AD0 0035:
	??_C@_0DF@OLKILINN@shadow_color?9?$DOred?5?$DO?$DN0?40f?5?$CG?$CG?5shad@ (0000)
00292B08 000d:
	??_C@_0N@NKBPFDCL@shadow_color?$AA@ (0000)
00292B18 000e:
	??_C@_0O@MDCFBAFA@shadow_matrix?$AA@ (0000)
0030CF84 0001:
	_shadow_restored (0000)
0046628C 004a:
	_local_shadow_color (0000)
	_local_object_bounding_radius (000c)
	_local_shadow_matrix (0010)
	_local_parameters (0044)
	_shadow_setup (0048)
	_shadow_used (0049)
*/

/* ---------- headers */

#include "cseries.h"
#include "cseries/errors.h"
#include "math/real_math.h"
#include "game/game_globals.h"
#include "bitmaps/bitmap_color_conversion.h"
#include "interface/hud_draw.h"
#include "rasterizer/common/rasterizer_common.h"
#include "rasterizer/rasterizer_geometry.h"
#include "render/render.h"
#include "shaders/shader_definitions.h"
#include "shaders/shaders.h"

/* January retains the stock XDK D3DINLINE out-of-line wrappers emitted by
 * the real device calls below. Keep the stock inline definitions and do not
 * replace these wrappers with handwritten bodies. */
#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_console_vars.h"
#include "rasterizer/rasterizer_model_types.h"
#include <xtl.h>
#include "rasterizer/xbox/rasterizer_xbox.h"
#include "rasterizer/xbox/rasterizer_xbox_pixel_shader.h"
#include "rasterizer/xbox/rasterizer_xbox_draw_primitives.h"

/* ---------- constants */

enum
{
	_rasterizer_target_render_primary = 0,
};

enum
{
	_rasterizer_statistics_mode_enabled = 2,
};

enum
{
	_shader_type_model = 4,
};

enum
{
	_shader_model_detail_after_reflection_bit = 0,
	_shader_model_two_sided_bit,
	_shader_model_not_alpha_tested_bit,
	_shader_model_alpha_blended_decal_bit,
	_shader_model_true_atmospheric_fog_bit,
	_shader_model_nocull_two_sided_bit,
};

enum
{
	RASTERIZER_STENCIL_MODE_NONE = 0,
	RASTERIZER_STENCIL_MODE_REJECT = 2,
};

enum
{
	_shadow_convolution_vertex_shader_index = 0x26,
	_shadow_vertex_shader_index = 0x1d,
	_shadow_model_vertex_shader_index = 0x27,
};

/* ---------- macros */

/* ---------- structures */

struct shader_model_definition
{
	struct shader shader;
	word flags;
	byte reserved2A[0x72];
	real_vector2d map_scale;
	struct tag_reference base_map;
	byte reservedB4[0x24];
	real detail_map_scale;
	struct tag_reference detail_map;
	real detail_map_v_scale;
	byte reservedF0[0xC];
	struct shader_texture_animation animation;
};

typedef char verify_rasterizer_shadows_model_shadow_count_offset[
	offsetof(
		struct rasterizer_frame_statistics_globals,
		model_shadow_count) == 0xF4 ? 1 : -1];
typedef char verify_rasterizer_shadows_model_parameters_size[
	sizeof(struct rasterizer_model_begin_parameters) == 0xCC ? 1 : -1];
typedef char verify_rasterizer_shadows_model_base_map_scale_offset[
	offsetof(struct rasterizer_model_begin_parameters, base_map_scale) == 0xC4
		? 1 : -1];
typedef char verify_rasterizer_shadows_model_animation_offset[
	offsetof(struct rasterizer_model_begin_parameters, animation) == 0x84
		? 1 : -1];
typedef char verify_rasterizer_shadows_shader_model_flags_offset[
	offsetof(struct shader_model_definition, flags) == 0x28 ? 1 : -1];
typedef char verify_rasterizer_shadows_shader_model_map_scale_offset[
	offsetof(struct shader_model_definition, map_scale) == 0x9C ? 1 : -1];
typedef char verify_rasterizer_shadows_shader_model_base_map_offset[
	offsetof(struct shader_model_definition, base_map) == 0xA4 ? 1 : -1];
typedef char verify_rasterizer_shadows_shader_model_detail_scale_offset[
	offsetof(struct shader_model_definition, detail_map_scale) == 0xD8 ? 1 : -1];
typedef char verify_rasterizer_shadows_shader_model_animation_offset[
	offsetof(struct shader_model_definition, animation) == 0xFC ? 1 : -1];

/* ---------- prototypes */

static void rasterizer_shadow_convolve(
	void);
static void rasterizer_shadow_convolve_pass(
	short source_target,
	short destination_target,
	real offset);

/* ---------- globals */

extern D3DDevice *global_d3d_device;
extern struct pixel_shader_definition pixel_shader;

static boolean shadow_restored = TRUE;

static real_rgb_color local_shadow_color = { 0 };
static real local_object_bounding_radius = 0.0f;
static real_matrix4x3 local_shadow_matrix = { 0 };
static struct rasterizer_model_begin_parameters const *local_parameters = NULL;
static boolean shadow_setup = FALSE;
static boolean shadow_used = FALSE;

/* port: the screen rectangle outside which an object's shadow, projected
onto the level, changes no pixel (halo_draw_bounds: the Metal device clips
the shadow's draws to it, which on a 4K screen saves shading and blending
the whole of each large level triangle under a small shadow).

The draw (_rasterizer_environment_shadow_draw's combiners) multiplies the
screen by 1 - a * t0 * (1 - shadow color), a = (1 - t1) * facing, and
discards where that is nothing: a pixel is left exactly as it was wherever
t0, the shadow map, is 0 or t1, the fade, is 1. The map is sampled at
u = forward.(p - position) / 2r + 0.5 and v = 0.5 - left.(p - position) / 2r
with a border of 0, so it is 0 more than a filter's reach outside u and v of
0 to 1: forward and left within r of the position, which MARGIN widens by 4
of the Xbox map's texels. The fade (globals' linear_corner_fade, a 16x16
max(s, t) ramp, clamped) is sampled at s = d / 4r and t = -2d / r for
d = up.(p - position): it is 1 for d >= 3.875r and d <= -0.484r, inside the
volume's planes at 4r and -0.5r (render_object_shadow_end). So no pixel
outside the box forward, left in [-r, r] * MARGIN, up in [-0.5r, 4r] about
the position changes: what the box covers on the screen bounds the draws.
A fragment outside its projection shades a point of the level outside the
box, whose interpolated u, v and d are those of the point.

Only the part of the box on the far side of the near plane is drawn: the
shadow's vertex shader keeps the frustum's clip z (rasterizer_set_frustum_z's
transform; d3d8_metal.c's viewport undoing leaves z as it was), and the GPU
clips what has z < 0. So the box is cut there first, which matters for the
closest and largest shadow, the player's own, whose box (4r up toward the
light) the camera is often inside: its corners on the far side and the
points where its 12 edges cross the plane span what is drawn. FALSE (no
bounds) when one of those has w <= 0 (cannot happen past a near plane
in front of the camera, but then the projection would bound nothing) or
none is on the far side (nothing of it is drawn; left to the GPU). */
#define SHADOW_BOUNDS_MARGIN (1.0f + 4.0f / 64.0f)

static boolean rasterizer_shadow_screen_bounds(
	short *bounds)
{
	struct render_frustum const *frustum = &global_window_parameters.frustum;
	rectangle2d const *viewport = &global_window_parameters.camera.viewport_bounds;
	real width = (real)(viewport->x1 - viewport->x0);
	real height = (real)(viewport->y1 - viewport->y0);
	real r = local_object_bounding_radius;
	real x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
	/* each corner's clip x, y, z, w, then the points that bound the box
	past the near plane */
	real clip[8][4];
	real points[8 + 12][4];
	short point_count = 0;
	short corner;
	short index;

	if (r <= 0.0f || width <= 0.0f || height <= 0.0f)
		return FALSE;
	for (corner = 0; corner < 8; corner++)
	{
		real a = (corner & 1) ? r * SHADOW_BOUNDS_MARGIN : -r * SHADOW_BOUNDS_MARGIN;
		real b = (corner & 2) ? r * SHADOW_BOUNDS_MARGIN : -r * SHADOW_BOUNDS_MARGIN;
		real c = (corner & 4) ? r * 4.0f : r * -0.5f;
		real point[3];
		real view[3];
		short axis;

		for (axis = 0; axis < 3; axis++)
		{
			point[axis] = local_shadow_matrix.position.n[axis] +
				a * local_shadow_matrix.forward.n[axis] +
				b * local_shadow_matrix.left.n[axis] +
				c * local_shadow_matrix.up.n[axis];
		}
		/* (the world to clip transform of rasterizer_set_frustum_z, which
		the shadow's vertex shader projects with) */
		for (axis = 0; axis < 3; axis++)
		{
			view[axis] = point[0] * frustum->world_to_view.n[0][axis] +
				point[1] * frustum->world_to_view.n[1][axis] +
				point[2] * frustum->world_to_view.n[2][axis] +
				frustum->world_to_view.n[3][axis];
		}
		for (axis = 0; axis < 4; axis++)
		{
			clip[corner][axis] = view[0] * frustum->projection_matrix[0][axis] +
				view[1] * frustum->projection_matrix[1][axis] +
				view[2] * frustum->projection_matrix[2][axis] +
				frustum->projection_matrix[3][axis];
		}
		if (clip[corner][2] >= 0.0f)
		{
			for (axis = 0; axis < 4; axis++)
				points[point_count][axis] = clip[corner][axis];
			point_count++;
		}
	}
	/* (the edges join corners that differ in one bit) */
	for (corner = 0; corner < 8; corner++)
	{
		short bit;

		for (bit = 1; bit < 8; bit <<= 1)
		{
			short other = corner | bit;
			real z0 = clip[corner][2];
			real z1 = clip[other][2];
			real t;
			short axis;

			if (other == corner || (z0 >= 0.0f) == (z1 >= 0.0f))
				continue;
			t = z0 / (z0 - z1);
			for (axis = 0; axis < 4; axis++)
				points[point_count][axis] = clip[corner][axis] + t * (clip[other][axis] - clip[corner][axis]);
			point_count++;
		}
	}
	if (!point_count)
		return FALSE;
	for (index = 0; index < point_count; index++)
	{
		real screen_x, screen_y;

		if (!(points[index][3] > 0.0001f))
			return FALSE;
		screen_x = (real)viewport->x0 + (points[index][0] / points[index][3] + 1.0f) * 0.5f * width;
		screen_y = (real)viewport->y0 + (1.0f - points[index][1] / points[index][3]) * 0.5f * height;
		if (!index || screen_x < x0)
			x0 = screen_x;
		if (!index || screen_x > x1)
			x1 = screen_x;
		if (!index || screen_y < y0)
			y0 = screen_y;
		if (!index || screen_y > y1)
			y1 = screen_y;
	}
	/* (two of the game's units more each way: pixel centres and rounding) */
	x0 = PIN(x0 - 2.0f, (real)viewport->x0, (real)viewport->x1);
	x1 = PIN(x1 + 2.0f, (real)viewport->x0, (real)viewport->x1);
	y0 = PIN(y0 - 2.0f, (real)viewport->y0, (real)viewport->y1);
	y1 = PIN(y1 + 2.0f, (real)viewport->y0, (real)viewport->y1);
	bounds[0] = (short)x0;
	bounds[1] = (short)y0;
	bounds[2] = (short)x1 + (x1 > (real)(short)x1 ? 1 : 0);
	bounds[3] = (short)y1 + (y1 > (real)(short)y1 ? 1 : 0);

	return TRUE;
}

/* ---------- public code */

void _rasterizer_environment_shadows_begin(
	void)
{
	rasterizer_profile_begin(_rasterizer_profile_environment_shadows);

	return;
}

boolean _rasterizer_environment_shadow_begin(
	long object_index,
	real_matrix4x3 const *shadow_matrix,
	real_rgb_color const *shadow_color,
	real object_bounding_radius,
	real *shadow_volume_bounding_radius)
{
	boolean success = TRUE;
	real inverse_radius;
	real vsh_constants__screenproj[20];

	match_assert(
		"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
		147,
		global_d3d_device);

	if (global_window_parameters.rasterizer_target ==
			_rasterizer_target_render_primary &&
		rasterizer_debug_options.draw_environment_shadows)
	{
		match_assert(
			"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
			153,
			shadow_matrix);
		match_assert(
			"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
			154,
			shadow_color);
		match_assert(
			"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
			155,
			shadow_color->red >=0.0f && shadow_color->red <=1.0f);
		match_assert(
			"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
			156,
			shadow_color->green>=0.0f && shadow_color->green<=1.0f);
		match_assert(
			"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
			157,
			shadow_color->blue >=0.0f && shadow_color->blue <=1.0f);
		match_assert(
			"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
			158,
			object_bounding_radius>0.0f);

		IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_CULLMODE, D3DCULL_CCW);
		IDirect3DDevice8_SetRenderState(
			global_d3d_device,
			D3DRS_COLORWRITEENABLE,
			D3DCOLORWRITEENABLE_RED |
			D3DCOLORWRITEENABLE_GREEN |
			D3DCOLORWRITEENABLE_BLUE);
		IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_ALPHABLENDENABLE, FALSE);
		IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_ALPHATESTENABLE, TRUE);
		IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_ALPHAREF, 0x7F);
		IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_ZENABLE, FALSE);
		IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_ZBIAS, 0);

		csmemset(&pixel_shader, 0, sizeof(pixel_shader));
		pixel_shader.texture_modes = 1;
		pixel_shader.combiner_count = 1;
		pixel_shader.final_combiner_inputs_abcd = 0x00000020;
		pixel_shader.final_combiner_inputs_efg = 0x00001800;
		rasterizer_set_pixel_shader(&pixel_shader);

		inverse_radius = 1.0f / object_bounding_radius;

		vsh_constants__screenproj[0] = inverse_radius * shadow_matrix->forward.i;
		vsh_constants__screenproj[1] = inverse_radius * shadow_matrix->forward.j;
		vsh_constants__screenproj[2] = inverse_radius * shadow_matrix->forward.k;
		vsh_constants__screenproj[3] = -inverse_radius * dot_product3d(
			&shadow_matrix->forward,
			(real_vector3d const *)&shadow_matrix->position);
		vsh_constants__screenproj[4] = inverse_radius * shadow_matrix->left.i;
		vsh_constants__screenproj[5] = inverse_radius * shadow_matrix->left.j;
		vsh_constants__screenproj[6] = inverse_radius * shadow_matrix->left.k;
		vsh_constants__screenproj[7] = -inverse_radius * dot_product3d(
			&shadow_matrix->left,
			(real_vector3d const *)&shadow_matrix->position);
		vsh_constants__screenproj[8] = 0.0f;
		vsh_constants__screenproj[9] = 0.0f;
		vsh_constants__screenproj[10] = 0.0f;
		vsh_constants__screenproj[11] = 0.5f;
		vsh_constants__screenproj[12] = 0.0f;
		vsh_constants__screenproj[13] = 0.0f;
		vsh_constants__screenproj[14] = 0.0f;
		vsh_constants__screenproj[15] = 1.0f;
		vsh_constants__screenproj[16] = 0.0f;
		vsh_constants__screenproj[17] = 0.0f;
		vsh_constants__screenproj[18] = 0.0f;
		vsh_constants__screenproj[19] = 0.0f;
		IDirect3DDevice8_SetVertexShaderConstant(
			global_d3d_device,
			-68,
			vsh_constants__screenproj,
			5);

		rasterizer_set_target(
			2,
			0,
			rasterizer_debug_options.shadow_debug_enabled ? 0x88888888 : 0,
			TRUE,
			FALSE);
		rasterizer_set_stencil_mode(0);

		local_shadow_matrix = *shadow_matrix;
		local_shadow_color = *shadow_color;
		local_object_bounding_radius = object_bounding_radius;
		if (shadow_volume_bounding_radius)
		{
			*shadow_volume_bounding_radius = object_bounding_radius;
		}

		local_parameters = 0;
		shadow_setup = FALSE;
		shadow_used = FALSE;
		shadow_restored = FALSE;

		if (rasterizer_debug_options.statistics_mode ==
			_rasterizer_statistics_mode_enabled)
		{
			rasterizer_frame_statistics.shadow_count++;
		}
	}

	return success;
}

void _rasterizer_environment_shadow_model_begin(
	struct rasterizer_model_begin_parameters const *parameters)
{
	match_assert(
		"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
		239,
		global_d3d_device);

	if (global_window_parameters.rasterizer_target ==
			_rasterizer_target_render_primary &&
		rasterizer_debug_options.draw_environment_shadows)
	{
		match_assert(
			"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
			245,
			parameters);

		rasterizer_set_model_skinning(&parameters->skinning);

		local_parameters = parameters;
		shadow_used = TRUE;

		if (rasterizer_debug_options.statistics_mode ==
			_rasterizer_statistics_mode_enabled)
		{
			rasterizer_frame_statistics.model_shadow_count++;
		}
	}

	return;
}

void _rasterizer_environment_shadow_model_end(
	void)
{
	local_parameters = 0;

	return;
}

void _rasterizer_environment_shadow_model_draw(
	struct shader const *shader,
	short bitmap_index,
	struct triangle_buffer const *triangle_buffer,
	struct vertex_buffer const *vertex_buffer)
{
	struct shader_model_definition const *model;

	match_assert(
		"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
		274,
		global_d3d_device);

	if (global_window_parameters.rasterizer_target ==
			_rasterizer_target_render_primary &&
		rasterizer_debug_options.draw_environment_shadows)
	{
		match_assert(
			"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
			280,
			shader);

		if (shader->base.type == _shader_type_model)
		{
			model = (struct shader_model_definition *)shader_get_and_verify_type(
				(struct shader *)shader,
				_shader_type_model);

			match_assert(
				"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
				286,
				vertex_buffer);
			match_assert(
				"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
				287,
				triangle_buffer);
			match_assert(
				"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
				288,
				local_parameters);

			if (TEST_FLAG(model->flags, _shader_model_two_sided_bit))
			{
				IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_CULLMODE, D3DCULL_NONE);
			}
			else
			{
				IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_CULLMODE, D3DCULL_CCW);
			}
			rasterizer_set_vertex_shader_permutation(
				_shadow_model_vertex_shader_index,
				vertex_buffer->type,
				0);

			if (TEST_FLAG(model->flags, _shader_model_not_alpha_tested_bit))
			{
				IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_PSTEXTUREMODES, 0);
			}
			else
			{
				IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_PSTEXTUREMODES, 1);
				rasterizer_set_texture(
					0,
					0,
					1,
					model->base_map.index,
					bitmap_index);
				IDirect3DDevice8_SetTextureStageState(
					global_d3d_device,
					0,
					D3DTSS_ADDRESSU,
					D3DTADDRESS_WRAP);
				IDirect3DDevice8_SetTextureStageState(
					global_d3d_device,
					0,
					D3DTSS_ADDRESSV,
					D3DTADDRESS_WRAP);
				IDirect3DDevice8_SetTextureStageState(
					global_d3d_device,
					0,
					D3DTSS_MAGFILTER,
					D3DTEXF_LINEAR);
				IDirect3DDevice8_SetTextureStageState(
					global_d3d_device,
					0,
					D3DTSS_MINFILTER,
					D3DTEXF_LINEAR);
				IDirect3DDevice8_SetTextureStageState(
					global_d3d_device,
					0,
					D3DTSS_MIPFILTER,
					D3DTEXF_LINEAR);
			}

			{
				real_vector4d vertex_constants[3] =
				{
					{
						model->detail_map_scale,
						model->detail_map_scale * model->detail_map_v_scale,
						1.0f,
						1.0f
					},
					{1.0f, 0.0f, 0.0f, 0.0f},
					{0.0f, 1.0f, 0.0f, 0.0f}
				};

				shader_texture_animation_evaluate(
					&model->animation,
					&local_parameters->animation,
					local_parameters->base_map_scale.i * model->map_scale.i,
					local_parameters->base_map_scale.j * model->map_scale.j,
					0.0f,
					0.0f,
					0.0f,
					global_frame_parameters.game_time_sec,
					&vertex_constants[1],
					&vertex_constants[2]);
				IDirect3DDevice8_SetVertexShaderConstant(
					global_d3d_device,
					-84,
					vertex_constants,
					NUMBEROF(vertex_constants));
			}

			rasterizer_draw_static_triangles_static_vertices(
				triangle_buffer,
				0,
				triangle_buffer->count,
				vertex_buffer);

			if (rasterizer_debug_options.statistics_mode ==
				_rasterizer_statistics_mode_enabled)
			{
				rasterizer_frame_statistics.model_shadow_draw_count++;
				rasterizer_frame_statistics.model_shadow_triangle_count +=
					triangle_buffer->count;
				rasterizer_frame_statistics.model_shadow_vertex_count +=
					rasterizer_frame_statistics_count_static_vertices(
						triangle_buffer,
						vertex_buffer);
			}
		}
	}

	return;
}

void _rasterizer_environment_shadow_end(
	void)
{
	match_assert(
		"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
		563,
		global_d3d_device);

	/* port: (the shadow's clip ends with it) */
	halo_draw_bounds(FALSE, 0, 0, 0, 0);

	if (global_window_parameters.rasterizer_target ==
			_rasterizer_target_render_primary &&
		rasterizer_debug_options.draw_environment_shadows)
	{
		if (!shadow_used)
		{
			error(
				_error_silent,
				"### WARNING empty shadow has been cast");
		}

		if (!shadow_restored)
		{
			rasterizer_set_target(
				global_window_parameters.rasterizer_target,
				FALSE,
				FALSE,
				FALSE,
				TRUE);
			shadow_restored = TRUE;
		}
	}

	return;
}

void _rasterizer_environment_shadow_draw(
	struct shader const *shader,
	short bitmap_index,
	long dynamic_triangle_buffer_index,
	long first_triangle_index,
	long triangle_count,
	struct vertex_buffer const *vertex_buffer)
{
	real vertex_constants[5][4];
	real inverse_radius;
	real inverse_radius_quarter;
	real inverse_radius_double;
	real position_dot_up;

	match_assert(
		"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
		404,
		global_d3d_device);

	if (global_window_parameters.rasterizer_target ==
			_rasterizer_target_render_primary &&
		rasterizer_debug_options.draw_environment_shadows)
	{
		if (!shadow_setup)
		{
			if (rasterizer_debug_options.shadow_convolution_enabled)
			{
				rasterizer_shadow_convolve();
			}

			rasterizer_set_target_as_texture(
				0,
				(rasterizer_debug_options.shadow_convolution_enabled != FALSE) + 2,
				FALSE);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				0,
				D3DTSS_ADDRESSU,
				D3DTADDRESS_BORDER);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				0,
				D3DTSS_ADDRESSV,
				D3DTADDRESS_BORDER);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				0,
				D3DTSS_MAGFILTER,
				D3DTEXF_LINEAR);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				0,
				D3DTSS_MINFILTER,
				D3DTEXF_LINEAR);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				0,
				D3DTSS_MIPFILTER,
				D3DTEXF_LINEAR);

			rasterizer_set_texture_direct(
				1,
				global_rasterizer_data->linear_corner_fade.index,
				0);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				1,
				D3DTSS_ADDRESSU,
				D3DTADDRESS_CLAMP);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				1,
				D3DTSS_ADDRESSV,
				D3DTADDRESS_CLAMP);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				1,
				D3DTSS_MAGFILTER,
				D3DTEXF_LINEAR);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				1,
				D3DTSS_MINFILTER,
				D3DTEXF_LINEAR);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				1,
				D3DTSS_MIPFILTER,
				D3DTEXF_LINEAR);

			rasterizer_set_texture_direct(
				2,
				global_rasterizer_data->vector_normalization.index,
				0);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				2,
				D3DTSS_ADDRESSU,
				D3DTADDRESS_CLAMP);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				2,
				D3DTSS_ADDRESSV,
				D3DTADDRESS_CLAMP);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				2,
				D3DTSS_ADDRESSW,
				D3DTADDRESS_CLAMP);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				2,
				D3DTSS_MAGFILTER,
				D3DTEXF_LINEAR);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				2,
				D3DTSS_MINFILTER,
				D3DTEXF_LINEAR);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				2,
				D3DTSS_MIPFILTER,
				D3DTEXF_LINEAR);

			IDirect3DDevice8_SetRenderState(
				global_d3d_device,
				D3DRS_CULLMODE,
				D3DCULL_CCW);
			IDirect3DDevice8_SetRenderState(
				global_d3d_device,
				D3DRS_COLORWRITEENABLE,
				D3DCOLORWRITEENABLE_ALL);
			IDirect3DDevice8_SetRenderState(
				global_d3d_device,
				D3DRS_ALPHABLENDENABLE,
				TRUE);
			IDirect3DDevice8_SetRenderState(
				global_d3d_device,
				D3DRS_SRCBLEND,
				D3DBLEND_ZERO);
			IDirect3DDevice8_SetRenderState(
				global_d3d_device,
				D3DRS_DESTBLEND,
				D3DBLEND_INVSRCCOLOR);
			IDirect3DDevice8_SetRenderState(
				global_d3d_device,
				D3DRS_BLENDOP,
				D3DBLENDOP_ADD);
			IDirect3DDevice8_SetRenderState(
				global_d3d_device,
				D3DRS_ALPHATESTENABLE,
				TRUE);
			IDirect3DDevice8_SetRenderState(
				global_d3d_device,
				D3DRS_ALPHAREF,
				0);
			IDirect3DDevice8_SetRenderState(
				global_d3d_device,
				D3DRS_ZENABLE,
				TRUE);
			IDirect3DDevice8_SetRenderState(
				global_d3d_device,
				D3DRS_ZFUNC,
				D3DCMP_EQUAL);
			IDirect3DDevice8_SetRenderState(
				global_d3d_device,
				D3DRS_ZWRITEENABLE,
				FALSE);
			IDirect3DDevice8_SetRenderState(
				global_d3d_device,
				D3DRS_ZBIAS,
				0);

			csmemset(&pixel_shader, 0, sizeof(pixel_shader));
			pixel_shader.texture_modes = 0x00000021;
			pixel_shader.combiner_count = 4;
			pixel_shader.constant_0[0] = real_rgb_color_to_pixel32(&local_shadow_color);
			pixel_shader.constant_1[0] = 0x00FFFFFF;
			pixel_shader.rgb_inputs[0] = 0x14200000;
			pixel_shader.rgb_outputs[0] = 0x000000C0;
			pixel_shader.rgb_inputs[1] = 0x290C0821;
			pixel_shader.rgb_outputs[1] = 0x000000CD;
			pixel_shader.rgb_inputs[2] = 0x2C200C2D;
			pixel_shader.rgb_outputs[2] = 0x00000C00;
			pixel_shader.rgb_inputs[3] = 0x2C020000;
			pixel_shader.rgb_outputs[3] = 0x000020D0;
			pixel_shader.final_combiner_inputs_abcd = 0x0000002C;
			pixel_shader.final_combiner_inputs_efg = 0x00000D00;
			if (rasterizer_debug_options.shadow_debug_enabled)
			{
				pixel_shader.final_combiner_inputs_abcd = 0x0000000C;
				IDirect3DDevice8_SetRenderState(
					global_d3d_device,
					D3DRS_ALPHATESTENABLE,
					FALSE);
			}
			rasterizer_set_pixel_shader(&pixel_shader);

			rasterizer_set_vertex_shader_permutation(
				_shadow_vertex_shader_index,
				vertex_buffer->type,
				shader_get_vertex_shader_permutation(shader));

			inverse_radius = 1.0f / local_object_bounding_radius;
			inverse_radius_quarter = 1.0f / (local_object_bounding_radius * 4.0f);
			inverse_radius_double = 1.0f / (local_object_bounding_radius * 0.5f);

			vertex_constants[0][0] = local_shadow_matrix.forward.i * inverse_radius * 0.5f;
			vertex_constants[0][1] = local_shadow_matrix.forward.j * inverse_radius * 0.5f;
			vertex_constants[0][2] = local_shadow_matrix.forward.k * inverse_radius * 0.5f;
			vertex_constants[0][3] =
				(1.0f -
				dot_product3d(
					(real_vector3d const *)&local_shadow_matrix.position,
					&local_shadow_matrix.forward) *
				inverse_radius) * 0.5f;
			vertex_constants[1][0] = local_shadow_matrix.left.i * inverse_radius * -0.5f;
			vertex_constants[1][1] = local_shadow_matrix.left.j * inverse_radius * -0.5f;
			vertex_constants[1][2] = local_shadow_matrix.left.k * inverse_radius * -0.5f;
			vertex_constants[1][3] =
				(dot_product3d(
					(real_vector3d const *)&local_shadow_matrix.position,
					&local_shadow_matrix.left) *
				inverse_radius + 1.0f) * 0.5f;
			vertex_constants[2][0] = local_shadow_matrix.up.i * inverse_radius_quarter;
			vertex_constants[2][1] = local_shadow_matrix.up.j * inverse_radius_quarter;
			vertex_constants[2][2] = local_shadow_matrix.up.k * inverse_radius_quarter;
			position_dot_up = dot_product3d(
				(real_vector3d const *)&local_shadow_matrix.position,
				&local_shadow_matrix.up);
			vertex_constants[2][3] =
				-(position_dot_up * inverse_radius_quarter);
			vertex_constants[3][0] = -(local_shadow_matrix.up.i * inverse_radius_double);
			vertex_constants[3][1] = -(local_shadow_matrix.up.j * inverse_radius_double);
			vertex_constants[3][2] = -(local_shadow_matrix.up.k * inverse_radius_double);
			vertex_constants[3][3] = position_dot_up * inverse_radius_double;
			vertex_constants[4][0] = local_shadow_matrix.up.i;
			vertex_constants[4][1] = local_shadow_matrix.up.j;
			vertex_constants[4][2] = local_shadow_matrix.up.k;
			vertex_constants[4][3] = 0.0f;
			IDirect3DDevice8_SetVertexShaderConstant(
				global_d3d_device,
				-81,
				vertex_constants,
				5);

			if (!shadow_restored)
			{
				rasterizer_set_target(
					global_window_parameters.rasterizer_target,
					0,
					0,
					FALSE,
					TRUE);
				shadow_restored = TRUE;
			}

			/* port: the shadow's draws clipped to where they can change a
			pixel (rasterizer_shadow_screen_bounds) */
			{
				short bounds[4];

				if (!rasterizer_debug_options.shadow_debug_enabled &&
					rasterizer_shadow_screen_bounds(bounds))
				{
					halo_draw_bounds(TRUE, bounds[0], bounds[1], bounds[2], bounds[3]);
				}
			}

			shadow_setup = TRUE;
		}

		rasterizer_set_stencil_mode(RASTERIZER_STENCIL_MODE_REJECT);
		rasterizer_draw_dynamic_triangles_static_vertices(
			dynamic_triangle_buffer_index,
			first_triangle_index,
			triangle_count,
			vertex_buffer);

		if (rasterizer_debug_options.statistics_mode ==
			_rasterizer_statistics_mode_enabled)
		{
			rasterizer_frame_statistics.shadow_draw_count++;
			rasterizer_frame_statistics.shadow_triangle_count += triangle_count;
			rasterizer_frame_statistics.shadow_vertex_count +=
				rasterizer_frame_statistics_count_dynamic_vertices(
					dynamic_triangle_buffer_index,
					first_triangle_index,
					triangle_count);
		}
	}

	return;
}

void _rasterizer_environment_shadows_end(
	void)
{
	halo_draw_bounds(FALSE, 0, 0, 0, 0);
	rasterizer_profile_end(_rasterizer_profile_environment_shadows);

	return;
}

/* ---------- private code */

static void rasterizer_shadow_convolve(
	void)
{
	short stage;
	real vertex_constants[8][4];
	/* port: the shadow maps' texels for each of the Xbox's 128 */
	long scale = halo_shadow_map_scale();
	long distance;

	match_assert(
		"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
		31,
		global_d3d_device);

	if (rasterizer_debug_options.draw_environment_shadows &&
		rasterizer_debug_options.shadow_convolution_enabled)
	{
		for (stage = 0; stage < 4; stage++)
		{
			rasterizer_set_target_as_texture(stage, 2, FALSE);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				stage,
				D3DTSS_ADDRESSU,
				D3DTADDRESS_BORDER);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				stage,
				D3DTSS_ADDRESSV,
				D3DTADDRESS_BORDER);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				stage,
				D3DTSS_MAGFILTER,
				D3DTEXF_LINEAR);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				stage,
				D3DTSS_MINFILTER,
				D3DTEXF_LINEAR);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				stage,
				D3DTSS_MIPFILTER,
				D3DTEXF_POINT);
		}

		IDirect3DDevice8_SetRenderState(
			global_d3d_device,
			D3DRS_CULLMODE,
			D3DCULL_CCW);
		IDirect3DDevice8_SetRenderState(
			global_d3d_device,
			D3DRS_COLORWRITEENABLE,
			D3DCOLORWRITEENABLE_RED |
			D3DCOLORWRITEENABLE_GREEN |
			D3DCOLORWRITEENABLE_BLUE);
		IDirect3DDevice8_SetRenderState(
			global_d3d_device,
			D3DRS_ALPHABLENDENABLE,
			FALSE);
		IDirect3DDevice8_SetRenderState(
			global_d3d_device,
			D3DRS_ALPHATESTENABLE,
			FALSE);
		IDirect3DDevice8_SetRenderState(
			global_d3d_device,
			D3DRS_ZENABLE,
			FALSE);
		IDirect3DDevice8_SetRenderState(
			global_d3d_device,
			D3DRS_ZBIAS,
			0);

		rasterizer_set_vertex_shader_permutation(
			_shadow_convolution_vertex_shader_index,
			8,
			0);

		vertex_constants[0][0] = 1.0f;
		vertex_constants[0][1] = 0.0f;
		vertex_constants[0][2] = 0.0f;
		vertex_constants[0][3] = -0.00390625f;
		vertex_constants[1][0] = 0.0f;
		vertex_constants[1][1] = 1.0f;
		vertex_constants[1][2] = 0.0f;
		vertex_constants[1][3] = -0.00390625f;
		vertex_constants[2][0] = 1.0f;
		vertex_constants[2][1] = 0.0f;
		vertex_constants[2][2] = 0.0f;
		vertex_constants[2][3] = 0.00390625f;
		vertex_constants[3][0] = 0.0f;
		vertex_constants[3][1] = 1.0f;
		vertex_constants[3][2] = 0.0f;
		vertex_constants[3][3] = 0.00390625f;
		vertex_constants[4][0] = 1.0f;
		vertex_constants[4][1] = 0.0f;
		vertex_constants[4][2] = 0.0f;
		vertex_constants[4][3] = -0.00390625f;
		vertex_constants[5][0] = 0.0f;
		vertex_constants[5][1] = 1.0f;
		vertex_constants[5][2] = 0.0f;
		vertex_constants[5][3] = 0.00390625f;
		vertex_constants[6][0] = 1.0f;
		vertex_constants[6][1] = 0.0f;
		vertex_constants[6][2] = 0.0f;
		vertex_constants[6][3] = 0.00390625f;
		vertex_constants[7][0] = 0.0f;
		vertex_constants[7][1] = 1.0f;
		vertex_constants[7][2] = 0.0f;
		vertex_constants[7][3] = -0.00390625f;
		/* port: half a texel of a larger map (rasterizer_shadow_convolve_pass) */
		if (scale > 1)
		{
			for (distance = 0; distance < 8; distance++)
			{
				vertex_constants[distance][3] /= (real)scale;
			}
		}
		IDirect3DDevice8_SetVertexShaderConstant(
			global_d3d_device,
			-81,
			vertex_constants,
			8);

		csmemset(&pixel_shader, 0, sizeof(pixel_shader));
		pixel_shader.texture_modes = 0x00008421;
		pixel_shader.combiner_count = 1;
		pixel_shader.alpha_inputs[0] = 0x08A009A0;
		pixel_shader.alpha_outputs[0] = 0x00030C00;
		pixel_shader.rgb_inputs[0] = 0x0AA00BA0;
		pixel_shader.rgb_outputs[0] = 0x00030C00;
		pixel_shader.final_combiner_inputs_abcd = 0x0C20001C;
		rasterizer_set_pixel_shader(&pixel_shader);

		rasterizer_set_target(3, 0, 0, FALSE, FALSE);

		IDirect3DDevice8_Begin(global_d3d_device, D3DPT_TRIANGLEFAN);
		IDirect3DDevice8_SetVertexData2s(global_d3d_device, 4, 0, 0);
		IDirect3DDevice8_SetVertexData2f(
			global_d3d_device,
			0,
			-1.0078125f,
			1.0078125f);
		IDirect3DDevice8_SetVertexData2s(global_d3d_device, 4, 1, 0);
		IDirect3DDevice8_SetVertexData2f(
			global_d3d_device,
			0,
			0.9921875f,
			1.0078125f);
		IDirect3DDevice8_SetVertexData2s(global_d3d_device, 4, 1, 1);
		IDirect3DDevice8_SetVertexData2f(
			global_d3d_device,
			0,
			0.9921875f,
			-0.9921875f);
		IDirect3DDevice8_SetVertexData2s(global_d3d_device, 4, 0, 1);
		IDirect3DDevice8_SetVertexData2f(
			global_d3d_device,
			0,
			-1.0078125f,
			-0.9921875f);
		IDirect3DDevice8_End(global_d3d_device);

		/* port: the passes that widen the blur of a larger map, back and
		forth between the maps, ending in the secondary */
		for (distance = 1; distance < scale; distance *= 2)
		{
			rasterizer_shadow_convolve_pass(3, 2, (real)distance / (real)(128 * scale));
			rasterizer_shadow_convolve_pass(2, 3, (real)distance / (real)(128 * scale));
		}
	}

	return;
}

/* port: the shadow maps can be drawn larger than the Xbox's 128x128
(display.shadow_resolution, port/linux/src/d3d8_gl.c): scale times as many
texels each way, a power of two. The Xbox's blur takes four taps diagonally
half a texel off each texel, on the corners it shares with its neighbours.
Each tap averages the four texels there, and together they weigh the texel
and its neighbours 1, 2, 1 each way: a tent over three texels, which is a box
two texels wide taken twice. For the shadows to stay as soft, a larger map's
blur must cover as much of the map: a tent scale times as wide, a box of
2 * scale texels taken twice, which weighs the texels 2 * scale - |d| for |d|
up to 2 * scale - 1 texels away. The Xbox's pass, with its taps half a texel
of the larger map off, is the box of two texels taken twice. A further pass
with its taps diagonally distance texels off, on texels' centres, averages
texels 2 * distance apart each way, which doubles a box of 2 * distance
texels; two passes each at distance 1, 2, ... scale / 2 make both boxes
2 * scale texels wide. The quad's offsets of half a texel stay as they are:
they are half of the game's units, which the port undoes in the same units
at any size (port/linux/src/nv2a_vsh.c), so the quad covers the map. */
static void rasterizer_shadow_convolve_pass(
	short source_target,
	short destination_target,
	real offset)
{
	short stage;
	real vertex_constants[8][4];

	csmemset(vertex_constants, 0, sizeof(vertex_constants));
	for (stage = 0; stage < 4; stage++)
	{
		rasterizer_set_target_as_texture(stage, source_target, FALSE);
		vertex_constants[stage * 2][0] = 1.0f;
		vertex_constants[stage * 2 + 1][1] = 1.0f;
	}
	/* the Xbox pass's corners, in its order */
	vertex_constants[0][3] = -offset;
	vertex_constants[1][3] = -offset;
	vertex_constants[2][3] = offset;
	vertex_constants[3][3] = offset;
	vertex_constants[4][3] = -offset;
	vertex_constants[5][3] = offset;
	vertex_constants[6][3] = offset;
	vertex_constants[7][3] = -offset;
	IDirect3DDevice8_SetVertexShaderConstant(
		global_d3d_device,
		-81,
		vertex_constants,
		8);

	rasterizer_set_target(destination_target, 0, 0, FALSE, FALSE);

	IDirect3DDevice8_Begin(global_d3d_device, D3DPT_TRIANGLEFAN);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, 4, 0, 0);
	IDirect3DDevice8_SetVertexData2f(global_d3d_device, 0, -1.0078125f, 1.0078125f);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, 4, 1, 0);
	IDirect3DDevice8_SetVertexData2f(global_d3d_device, 0, 0.9921875f, 1.0078125f);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, 4, 1, 1);
	IDirect3DDevice8_SetVertexData2f(global_d3d_device, 0, 0.9921875f, -0.9921875f);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, 4, 0, 1);
	IDirect3DDevice8_SetVertexData2f(global_d3d_device, 0, -1.0078125f, -0.9921875f);
	IDirect3DDevice8_End(global_d3d_device);

	return;
}
