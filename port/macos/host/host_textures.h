/*
HOST_TEXTURES.H

DXT textures (Direct3D's DXT1, DXT3 and DXT5: Metal's BC1, BC2 and BC3) for
a GPU without BC formats, the Apple TV's A12 among them. The guest uploads
the blocks as they are in the map and the host (host_metal.m) converts them
here, natively rather than in the translated guest, into

- BGRA8: the texels the guest's own decoding gives, exactly
  (port/linux/src/xbox_textures.c, dxt_decode_level); or
- ASTC 4x4 (LDR, 16 bytes a block: a quarter of BGRA8's bytes), block by
  block, by Khronos' ASTC specification. A block of one colour, a DXT1
  block of four colours, of three and no transparency or of transparency
  and one colour, and a DXT3 or DXT5 block of one alpha, maps onto ASTC
  exactly but for the interpolated colours' rounding (within 2 255ths). A
  DXT1 block of transparency and more colours is split into two or three
  partitions where they fit, whose endpoints are rounded to fewer bits;
  a DXT3 or DXT5 block whose alpha varies keeps four levels of it. Those
  can differ visibly: tools/texture_formats.py measures by how much on a
  map's textures. Not the default (display.texture_format).

Plain C, no Metal: tools/test_texture_formats.py and tools/texture_formats.py
build it on its own.
*/

#ifndef __HALO_MACOS_HOST_TEXTURES_H
#define __HALO_MACOS_HOST_TEXTURES_H

#include <stdint.h>

enum host_texture_bc
{
	_host_texture_bc1,
	_host_texture_bc2,
	_host_texture_bc3,
};

/* the bytes of a block (8 for BC1, 16 for BC2 and BC3) */
unsigned int host_textures_bc_block_bytes(int kind);

/* one block's texels as the guest decodes them: BGRA words (the guest's
32-bit ARGB), rows of 4 */
void host_textures_bc_decode_block(int kind, const unsigned char *block, uint32_t texels[16]);

/* a level (depth slices of rows of blocks, bytes_per_row and
bytes_per_image apart) into width * height * depth BGRA words, rows of
width */
void host_textures_bc_to_bgra(int kind, const void *blocks, unsigned int bytes_per_row, unsigned int bytes_per_image,
	unsigned int width, unsigned int height, unsigned int depth, uint32_t *bgra);

/* how near a block's ASTC is to the guest's texels: the largest
difference of a channel of a texel, in 255ths */
enum host_texture_astc_error
{
	/* 0-2: the interpolation's rounding */
	_astc_exact,
	/* 3-4: endpoints rounded to fewer bits (no more than the 5 bits of
	DXT's own red and blue endpoints lose) */
	_astc_rounded,
	/* more */
	_astc_approximated,
	NUMBER_OF_HOST_TEXTURE_ASTC_ERRORS
};

/* one block into an ASTC 4x4 block; the largest difference of a channel
of a texel from the guest's decoding, 0-255 */
int host_textures_astc_block(int kind, const unsigned char *block, unsigned char astc[16]);

/* an ASTC 4x4 block's texels as a decoder gives them, rounded to 8 bits,
BGRA words: 1 for the kinds of block host_textures_astc_block writes, else 0 */
int host_textures_astc_decode(const unsigned char astc[16], uint32_t texels[16]);

/* a level's blocks (depth slices of rows of blocks, bytes_per_row and
bytes_per_image apart) into ASTC blocks (rows of blocks_x * 16 bytes, slices
of blocks_y of those). With counts, each block's error is measured: counts
adds up how many blocks were of each, and the largest is returned; without
(the game's conversions) a block whose encoding is the only one considered
is not decoded again to measure it, and the result is 0. */
int host_textures_bc_to_astc(int kind, const void *blocks, unsigned int bytes_per_row, unsigned int bytes_per_image,
	unsigned int blocks_x, unsigned int blocks_y, unsigned int depth, unsigned char *astc,
	unsigned long counts[NUMBER_OF_HOST_TEXTURE_ASTC_ERRORS]);

#endif
