/*
GUEST_GL.C

The OpenGL renderer's anti-aliasing passes (xgpu_post.c), which the macOS
build's device does not run while it draws with Metal
(port/macos/README.md): d3d8_gl.c refers to them, behind its GL state, which
the macOS build never has.
*/

#include "xgpu.h"

BOOL xgpu_post_prepare(BOOL smaa)
{
	(void)smaa;
	return FALSE;
}

BOOL xgpu_post_anti_alias(BOOL smaa, GLuint framebuffer, unsigned long width, unsigned long height,
	const GLint corners[4])
{
	(void)smaa;
	(void)framebuffer;
	(void)width;
	(void)height;
	(void)corners;
	return FALSE;
}
