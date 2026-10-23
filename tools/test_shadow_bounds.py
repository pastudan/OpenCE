"""The Metal device clips an object's shadow, projected onto the level, to
the screen rectangle outside which it changes no pixel
(source/rasterizer/xbox/rasterizer_xbox_shadows.c's
rasterizer_shadow_screen_bounds, port/macos/guest/d3d8_metal.c's
halo_draw_bounds). These check the wiring and the arithmetic its comment
rests on; frozen-frame identity on the Mac is in port/tvos/PERFORMANCE.md."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent.parent
SHADOWS = (ROOT / "source/rasterizer/xbox/rasterizer_xbox_shadows.c").read_text()
METAL = (ROOT / "port/macos/guest/d3d8_metal.c").read_text()
GL = (ROOT / "port/linux/src/d3d8_gl.c").read_text()
FIXUPS = (ROOT / "port/linux/include/halo_linux_source_fixups.h").read_text()
RENDER_OBJECTS = (ROOT / "source/render/render_objects.c").read_text()


def function_body(text, name):
    """the definition's body (not a declaration's: the parameters' close is
    followed by the body on the next line)"""
    match = re.search(re.escape(name) + r"\([^;{]*\)\n\{", text)
    start = match.end() - 2
    depth, index = 0, start + 1
    while True:
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if not depth:
                return text[start:index + 1]
        index += 1


def test_every_renderer_has_the_hook():
    assert "void halo_draw_bounds(unsigned char active, short x0, short y0, short x1, short y1);" in FIXUPS
    assert "void halo_draw_bounds(unsigned char active, short x0, short y0, short x1, short y1)" in METAL
    assert "void halo_draw_bounds(unsigned char active, short x0, short y0, short x1, short y1)" in GL


def test_the_bounds_are_set_once_a_shadow_is_set_up_and_cleared_after():
    draw = function_body(SHADOWS, "void _rasterizer_environment_shadow_draw")
    setup = draw[:draw.index("shadow_setup = TRUE;")]
    # (after the target is restored: the clip is in the screen's units)
    assert setup.index("rasterizer_set_target(") < setup.index("halo_draw_bounds(TRUE")
    assert "!rasterizer_debug_options.shadow_debug_enabled" in setup
    end = function_body(SHADOWS, "void _rasterizer_environment_shadow_end")
    assert "halo_draw_bounds(FALSE, 0, 0, 0, 0);" in end
    shadows_end = function_body(SHADOWS, "void _rasterizer_environment_shadows_end")
    assert "halo_draw_bounds(FALSE, 0, 0, 0, 0);" in shadows_end


def test_the_box_is_the_shadow_volume_s():
    bounds = function_body(SHADOWS, "static boolean rasterizer_shadow_screen_bounds")
    # forward and left within r (widened), up from -0.5r to 4r: the volume's
    # planes in render_object_shadow_end
    assert "r * 4.0f : r * -0.5f" in bounds
    assert "shadow_volume_planes[0].d -= data->shadow_bounding_radius * 0.5f;" in RENDER_OBJECTS
    assert "shadow_volume_planes[1].d -= data->shadow_bounding_radius * 4.f;" in RENDER_OBJECTS
    margin = float(eval(re.search(r"#define SHADOW_BOUNDS_MARGIN \((.*)\)", SHADOWS).group(1).replace("f", "")))
    # the map's 128 texels span 2r: a bilinear tap reaches half a texel, so
    # it is 0 beyond r * (1 + 1/128); the margin is 4 texels
    assert margin >= 1.0 + 1.0 / 128.0
    # the box is cut at the near plane (clip z >= 0, what the GPU draws);
    # behind the camera the projection bounds nothing: no clip
    assert "if (clip[corner][2] >= 0.0f)" in bounds
    assert "t = z0 / (z0 - z1);" in bounds
    assert "if (!(points[index][3] > 0.0001f))\n\t\t\treturn FALSE;" in bounds
    assert "if (!point_count)\n\t\treturn FALSE;" in bounds


def test_the_fade_is_one_beyond_the_box_along_up():
    """linear_corner_fade is 16x16, max(s, t) * 0x11 (read from the map: the
    trace in PERFORMANCE.md), clamped; the shadow's vertex shader samples it
    at s = d / 4r, t = -2d / r for d = up.(p - position) (its constants -81's
    third and fourth rows). The draw changes nothing where it is 1."""
    texels = [[max(x, y) * 0x11 for x in range(16)] for y in range(16)]

    def fade(s, t):  # bilinear, clamp to edge, texel centres at (i + 0.5) / 16
        def tap(c):
            c = min(max(c * 16 - 0.5, 0.0), 15.0)
            i = min(int(c), 14)
            return i, c - i
        (x, fx), (y, fy) = tap(s), tap(t)
        top = texels[y][x] * (1 - fx) + texels[y][x + 1] * fx
        bottom = texels[y + 1][x] * (1 - fx) + texels[y + 1][x + 1] * fx
        return top * (1 - fy) + bottom * fy

    for d in (4.0, 4.5, 10.0, -0.5, -0.6, -3.0):  # in units of r
        assert fade(d / 4.0, -2.0 * d) == 255.0, d
    # (the vertex constants the fade's coordinates come from)
    assert "inverse_radius_quarter = 1.0f / (local_object_bounding_radius * 4.0f);" in SHADOWS
    assert "inverse_radius_double = 1.0f / (local_object_bounding_radius * 0.5f);" in SHADOWS
    assert "vertex_constants[3][0] = -(local_shadow_matrix.up.i * inverse_radius_double);" in SHADOWS


def test_the_box_is_handed_over_and_each_draw_is_cut_to_it():
    assert "void halo_draw_bounds_box(float const planes[6][4]);" in FIXUPS
    assert "void halo_draw_bounds_box(float const planes[6][4])" in GL
    box = function_body(SHADOWS, "static void rasterizer_shadow_bounds_box")
    assert "{ r * 4.0f, r * 0.5f } };" in box and "r * SHADOW_BOUNDS_MARGIN" in box
    assert "halo_draw_bounds_box(planes);" in box
    triangles = function_body(METAL, "static BOOL draw_bounds_triangles")
    # the shadow's own projection (rasterizer_set_frustum_z's c[-96]), the
    # near plane, and slack for the vertex program's half-unit shift
    assert "XGPU_VERTEX_CONSTANT_BIAS - 96" in triangles
    assert "near_plane[4] = { 0.0f, 0.0f, 1.0f, 0.0f }" in triangles
    assert "0.5f * target_scale[1]" in triangles
    # anything it cannot read draws as it was
    assert "position->type != D3DVSDT_FLOAT3" in triangles
    draw = function_body(METAL, "void WINAPI D3DDevice_DrawIndexedVertices")
    assert "primitive_type_value == D3DPT_TRIANGLELIST" in draw
    assert draw.index("draw_bounds_triangles") < draw.index("host_metal_draw")
    # cleared with the rectangle
    assert "draw_bounds_box.active = FALSE;" in function_body(METAL, "void halo_draw_bounds")


def test_polygon_cut_keeps_the_inside():
    """polygon_cut's arithmetic, in Python: a triangle across x >= 0"""
    def cut(polygon, plane):
        out = []
        for index, a in enumerate(polygon):
            b = polygon[(index + 1) % len(polygon)]
            da = sum(x * p for x, p in zip(a, plane))
            db = sum(x * p for x, p in zip(b, plane))
            if da >= 0:
                out.append(a)
            if (da >= 0) != (db >= 0):
                t = da / (da - db)
                out.append([x + t * (y - x) for x, y in zip(a, b)])
        return out
    triangle = [[-1.0, 0.0, 0.0, 1.0], [1.0, 0.0, 0.0, 1.0], [1.0, 1.0, 0.0, 1.0]]
    kept = cut(triangle, [1.0, 0.0, 0.0, 0.0])
    assert min(p[0] for p in kept) == 0.0 and len(kept) == 4
    assert cut(triangle, [-1.0, 0.0, 0.0, -2.0]) == []
    assert "static int polygon_cut(" in METAL


def test_the_device_clips_the_scissor_and_can_be_switched_off():
    raster = function_body(METAL, "static void apply_raster_state")
    assert "if (draw_bounds.active)" in raster
    assert raster.index("if (draw_bounds.active)") < raster.index("draw->scissor[2] =")
    enabled = function_body(METAL, "static BOOL draw_bounds_enabled")
    assert 'getenv("HALO_SHADOW_BOUNDS")' in enabled
    assert '"toggle:"' in enabled and "device.frame" in enabled
