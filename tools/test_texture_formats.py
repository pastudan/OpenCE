"""The host's DXT conversions (port/macos/host/host_textures.c), built on
their own with cc:

- host_textures_bc_to_bgra gives the texels the guest's own decoding does
  (port/linux/src/xbox_textures.c's dxt_color_block and dxt_decode_level,
  compiled from its text here), for random blocks of all three kinds and
  for sizes that are not multiples of 4;
- the ASTC blocks decode (host_textures_astc_decode) to within the error
  reported, and the exact cases are exact: a block of one colour, a DXT1
  block of four colours, of three without transparency, of transparency and
  one colour, and a DXT3/DXT5 block of one alpha are within 2/255 of the
  guest's texels on every channel; synthetic smooth textures keep a PSNR
  over 40 dB.

The ASTC bit layout itself was checked against ARM's astcenc decoder on
every DXT block of a10 and b30 (port/tvos/PERFORMANCE.md, "Texture formats
(A12)"); astcenc is not vendored, so that check is not repeated here.
"""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parent.parent
HOST = ROOT / "port/macos/host"


def guest_decoder():
    """the guest's DXT decoding, cut from xbox_textures.c"""
    source = (ROOT / "port/linux/src/xbox_textures.c").read_text()
    start = source.index("static unsigned long color565(")
    end = source.index("#endif", source.index("static void dxt_decode_level("))
    helpers = source[source.index("static unsigned long expand5("):source.index("static unsigned char clamp_byte(")]
    return helpers + source[start:end]


HARNESS = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host_textures.h"
typedef int BOOL;
#define TRUE 1
#define FALSE 0
enum { _texel_dxt1 = 1, _texel_dxt3, _texel_dxt5 };
GUEST

static unsigned long seed = 12345;
static unsigned rnd(void) { seed = seed * 6364136223846793005ul + 1442695040888963407ul; return (unsigned)(seed >> 33); }

static int channel(uint32_t texel, int c) { return (texel >> (c * 8)) & 255; }

static int max_error(const uint32_t *a, const uint32_t *b, int n, double *squares)
{
	int worst = 0, i, c;
	for (i = 0; i < n; i++)
		for (c = 0; c < 4; c++) {
			int d = abs(channel(a[i], c) - channel(b[i], c));
			if (d > worst) worst = d;
			if (squares) *squares += d * d;
		}
	return worst;
}

/* a texture's blocks into BGRA by the guest and by the host: the same */
static void same_as_guest(int kind, unsigned w, unsigned h)
{
	unsigned bx = (w + 3) / 4, by = (h + 3) / 4, bytes = kind ? 16 : 8, i;
	unsigned char *blocks = malloc(bx * by * bytes);
	unsigned long *guest = malloc(w * h * sizeof(unsigned long));
	uint32_t *host = malloc(w * h * 4);
	for (i = 0; i < bx * by * bytes; i++) blocks[i] = (unsigned char)rnd();
	/* (some DXT1 blocks of three colours, some of four, some equal) */
	dxt_decode_level(kind == 0 ? _texel_dxt1 : kind == 1 ? _texel_dxt3 : _texel_dxt5, blocks, w, h, 1, guest);
	host_textures_bc_to_bgra(kind, blocks, bx * bytes, bx * by * bytes, w, h, 1, host);
	for (i = 0; i < w * h; i++)
		assert((uint32_t)guest[i] == host[i]);
	free(blocks); free(guest); free(host);
}

static uint16_t rgb565(int r, int g, int b) { return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }

static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }

/* one block into ASTC: decodes within the error it reports, and returns it */
static int astc_error(int kind, const unsigned char *block)
{
	unsigned char astc[16];
	uint32_t ref[16], got[16];
	int reported = host_textures_astc_block(kind, block, astc);
	host_textures_bc_decode_block(kind, block, ref);
	assert(host_textures_astc_decode(astc, got));
	assert(max_error(ref, got, 16, NULL) == reported);
	return reported;
}

int main(void)
{
	unsigned i, t;
	unsigned char block[16];
	int kind;

	for (kind = 0; kind < 3; kind++) {
		same_as_guest(kind, 64, 32);
		same_as_guest(kind, 4, 4);
		same_as_guest(kind, 1, 1);
		same_as_guest(kind, 2, 8);
		same_as_guest(kind, 13, 7);
	}

	/* DXT1, four colours (c0 > c1) and three without transparency: exact */
	for (i = 0; i < 20000; i++) {
		unsigned c0 = rnd() & 0xffff, c1 = rnd() & 0xffff, bits = rnd();
		if (c0 == c1) continue;
		if (i & 1) { if (c0 < c1) { unsigned s = c0; c0 = c1; c1 = s; } }
		else {
			if (c0 > c1) { unsigned s = c0; c0 = c1; c1 = s; }
			/* (no index 3: no transparency) */
			for (t = 0; t < 16; t++) if (((bits >> (2 * t)) & 3) == 3) bits &= ~(1u << (2 * t));
		}
		put16(block, c0); put16(block + 2, c1);
		block[4] = (unsigned char)bits; block[5] = (unsigned char)(bits >> 8);
		block[6] = (unsigned char)(bits >> 16); block[7] = (unsigned char)(bits >> 24);
		assert(astc_error(0, block) <= 2);
	}
	/* DXT1, transparency and one colour; and all of one colour: exact */
	for (i = 0; i < 5000; i++) {
		unsigned c0 = rnd() & 0xffff, c1, bits = 0, index = rnd() % 3;
		c1 = c0 | (rnd() & 0xffff);
		put16(block, c0); put16(block + 2, c1);
		for (t = 0; t < 16; t++) bits |= ((rnd() & 1) ? 3u : index) << (2 * t);
		block[4] = (unsigned char)bits; block[5] = (unsigned char)(bits >> 8);
		block[6] = (unsigned char)(bits >> 16); block[7] = (unsigned char)(bits >> 24);
		assert(astc_error(0, block) <= 2);
	}
	/* DXT3 and DXT5 of one alpha: exact */
	for (i = 0; i < 10000; i++) {
		for (t = 0; t < 16; t++) block[t] = (unsigned char)rnd();
		if (i & 1) {
			unsigned a = rnd() & 15;
			for (t = 0; t < 8; t++) block[t] = (unsigned char)(a | (a << 4));
			assert(astc_error(1, block) <= 2);
		} else {
			block[0] = block[1];
			for (t = 2; t < 8; t++) block[t] = 0;
			assert(astc_error(2, block) <= 2);
		}
	}
	/* random blocks of every kind: decodable, error as reported */
	for (i = 0; i < 30000; i++) {
		for (t = 0; t < 16; t++) block[t] = (unsigned char)rnd();
		(void)astc_error((int)(i % 3), block);
	}
	/* a smooth opaque gradient as DXT1 (endpoints along it): PSNR > 40 dB */
	{
		double squares = 0;
		unsigned n = 0, bx, by;
		for (by = 0; by < 32; by++)
			for (bx = 0; bx < 32; bx++) {
				unsigned char astc[16];
				uint32_t ref[16], got[16];
				unsigned bits = 0;
				put16(block, rgb565(bx * 8 + 7, by * 8 + 7, 128));
				put16(block + 2, rgb565(bx * 8, by * 8, 96));
				for (t = 0; t < 16; t++) bits |= (unsigned)((t * 7 + bx + by) % 4) << (2 * t);
				block[4] = (unsigned char)bits; block[5] = (unsigned char)(bits >> 8);
				block[6] = (unsigned char)(bits >> 16); block[7] = (unsigned char)(bits >> 24);
				host_textures_astc_block(0, block, astc);
				host_textures_bc_decode_block(0, block, ref);
				host_textures_astc_decode(astc, got);
				max_error(ref, got, 16, &squares);
				n += 64;
			}
		assert(10 * log10(255.0 * 255.0 / (squares / n)) > 40.0);
	}
	/* a level converted whole: the same blocks as one by one, the counts */
	{
		unsigned char blocks[8 * 8 * 8], astc[8 * 8 * 16], one[16];
		unsigned long counts[NUMBER_OF_HOST_TEXTURE_ASTC_ERRORS] = { 0 };
		for (i = 0; i < sizeof(blocks); i++) blocks[i] = (unsigned char)rnd();
		host_textures_bc_to_astc(0, blocks, 8 * 8, 8 * 8 * 8, 8, 8, 1, astc, counts);
		assert(counts[0] + counts[1] + counts[2] == 64);
		for (i = 0; i < 64; i++) {
			host_textures_astc_block(0, blocks + i * 8, one);
			assert(!memcmp(one, astc + i * 16, 16));
		}
		/* (unmeasured, as the game converts: the same blocks) */
		memset(astc, 0, sizeof(astc));
		host_textures_bc_to_astc(0, blocks, 8 * 8, 8 * 8 * 8, 8, 8, 1, astc, NULL);
		for (i = 0; i < 64; i++) {
			host_textures_astc_block(0, blocks + i * 8, one);
			assert(!memcmp(one, astc + i * 16, 16));
		}
	}
	puts("ok");
	return 0;
}
'''


def test_host_conversions(tmp_path):
    code = tmp_path / "textures.c"
    code.write_text(HARNESS.replace("GUEST", guest_decoder()))
    executable = tmp_path / "textures"
    subprocess.run(["cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Wno-unused-function", "-I", str(HOST), str(code),
                    str(HOST / "host_textures.c"), "-o", str(executable), "-lm"], check=True)
    assert subprocess.run([str(executable)], check=True, capture_output=True, text=True).stdout.strip() == "ok"


def test_guest_hands_dxt_to_the_host_where_no_bc():
    source = (ROOT / "port/linux/src/xbox_textures.c").read_text()
    upload = source[source.index("static void upload(", source.index("#ifdef HALO_METAL")):]
    upload = upload[:upload.index("\n}\n")]
    # a GPU with BC formats (the Mac) still gets the blocks as they are
    assert "compressed_upload = description->compressed && host_metal_supports_bc()" in upload
    assert re.search(r"if \(description->compressed && !compressed_upload && bc_conversion\(\) >= 0\)", upload)
    assert "host_metal_texture_bc(" in upload
    config = (ROOT / "port/linux/src/port_config.c").read_text()
    assert '"display.texture_format", _config_string, "\\"exact\\""' in config


def test_texture_cache_keeps_a_budget_and_never_sweeps():
    """xbox_textures.c's residency: no periodic mass drop (the old
    every-600-frames sweep of every texture unused for 1800), a byte budget
    trimmed a few textures a frame, and a dropped texture forgotten only from
    its own recent_textures slot"""
    source = (ROOT / "port/linux/src/xbox_textures.c").read_text()
    assert "texture_frame % 600" not in source and "TEXTURE_IDLE_FRAMES" not in source
    assert "texture_drop_serial" not in source
    assert "#define TEXTURE_EVICTIONS_PER_FRAME 4" in source
    trim = source[source.index("static void texture_residency_trim(void)"):]
    trim = trim[:trim.index("\n}\n")]
    assert "dropped < TEXTURE_EVICTIONS_PER_FRAME" in trim and "texture_residency.bytes > budget" in trim
    assert "TEXTURE_RESIDENT_FRAMES" in trim
    delete = source[source.index("static void texture_entry_delete("):]
    delete = delete[:delete.index("\n}\n")]
    assert "recent_textures[recent].entry == entry" in delete and "memset(recent_textures" not in delete
    assert "xgpu_gl_state_invalidate" not in delete
    begin = source[source.index("void xgpu_texture_cache_begin_frame(void)"):]
    assert "texture_residency_trim();" in begin
    config = (ROOT / "port/linux/src/port_config.c").read_text()
    assert '{ "debug.texture_budget_mb", _config_integer, "0", "HALO_TEXTURE_BUDGET_MB"' in config
