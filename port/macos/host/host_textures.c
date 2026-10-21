/*
HOST_TEXTURES.C

DXT blocks converted for a GPU without BC formats (host_textures.h).

The decoding is the guest's arithmetic (port/linux/src/xbox_textures.c:
dxt_color_block, dxt_decode_level), so that a texture decoded here has the
texels the guest would have given it; tools/test_texture_formats.py compares
the two.

The ASTC blocks follow the Khronos Data Format Specification's ASTC chapter
(block modes, integer sequence encoding, endpoint modes 8, 10 and 12, the
partition hash), written here for the few kinds of block a DXT block needs:
a 4x4 grid of weights, one weight a texel, one to three partitions, one or
two planes. host_textures_astc_decode decodes those kinds again, which is
how each block's error is measured.
*/

#include "host_textures.h"

#include <pthread.h>
#include <string.h>

static uint32_t expand5(uint32_t v) { return (v << 3) | (v >> 2); }
static uint32_t expand6(uint32_t v) { return (v << 2) | (v >> 4); }

static uint32_t color565(uint32_t value)
{
	return 0xff000000u | (expand5(value >> 11) << 16) | (expand6((value >> 5) & 0x3f) << 8) | expand5(value & 0x1f);
}

static uint32_t mix(uint32_t a, uint32_t b, uint32_t weight_a, uint32_t weight_b, uint32_t divisor)
{
	uint32_t result = 0;
	int shift;

	for (shift = 0; shift < 24; shift += 8)
		result |= ((((a >> shift) & 0xff) * weight_a + ((b >> shift) & 0xff) * weight_b) / divisor) << shift;
	return result | 0xff000000u;
}

unsigned int host_textures_bc_block_bytes(int kind)
{
	return kind == _host_texture_bc1 ? 8 : 16;
}

/* ---------- decoding */

/* the colour half's palette (DXT3's and DXT5's always of four) and each
texel's index into it */
static void bc_colors(int kind, const unsigned char *color, uint32_t palette[4], unsigned char indices[16])
{
	uint32_t c0 = color[0] | ((uint32_t)color[1] << 8);
	uint32_t c1 = color[2] | ((uint32_t)color[3] << 8);
	uint32_t bits = color[4] | ((uint32_t)color[5] << 8) | ((uint32_t)color[6] << 16) | ((uint32_t)color[7] << 24);
	int index;

	palette[0] = color565(c0);
	palette[1] = color565(c1);
	if (c0 > c1 || kind != _host_texture_bc1)
	{
		palette[2] = mix(palette[0], palette[1], 2, 1, 3);
		palette[3] = mix(palette[0], palette[1], 1, 2, 3);
	}
	else
	{
		palette[2] = mix(palette[0], palette[1], 1, 1, 2);
		palette[3] = 0;
	}
	for (index = 0; index < 16; index++)
		indices[index] = (unsigned char)((bits >> (index * 2)) & 3);
}

void host_textures_bc_decode_block(int kind, const unsigned char *block, uint32_t texels[16])
{
	uint32_t palette[4];
	unsigned char indices[16];
	int index;

	bc_colors(kind, kind == _host_texture_bc1 ? block : block + 8, palette, indices);
	for (index = 0; index < 16; index++)
		texels[index] = palette[indices[index]];
	if (kind == _host_texture_bc2)
	{
		for (index = 0; index < 16; index++)
			texels[index] = (texels[index] & 0x00ffffffu) | ((((block[index / 2] >> ((index & 1) * 4)) & 0xfu) * 0x11u) << 24);
	}
	else if (kind == _host_texture_bc3)
	{
		uint32_t a0 = block[0], a1 = block[1], values[8];
		uint64_t alpha_bits = 0;
		int byte;

		for (byte = 0; byte < 6; byte++)
			alpha_bits |= (uint64_t)block[2 + byte] << (byte * 8);
		values[0] = a0;
		values[1] = a1;
		if (a0 > a1)
		{
			for (index = 2; index < 8; index++)
				values[index] = ((8 - index) * a0 + (index - 1) * a1) / 7;
		}
		else
		{
			for (index = 2; index < 6; index++)
				values[index] = ((6 - index) * a0 + (index - 1) * a1) / 5;
			values[6] = 0;
			values[7] = 255;
		}
		for (index = 0; index < 16; index++)
			texels[index] = (texels[index] & 0x00ffffffu) | (values[(alpha_bits >> (index * 3)) & 7] << 24);
	}
}

void host_textures_bc_to_bgra(int kind, const void *blocks, unsigned int bytes_per_row, unsigned int bytes_per_image,
	unsigned int width, unsigned int height, unsigned int depth, uint32_t *bgra)
{
	unsigned int block_bytes = host_textures_bc_block_bytes(kind);
	unsigned int blocks_x = (width + 3) / 4, blocks_y = (height + 3) / 4;
	unsigned int z, bx, by, x, y;

	for (z = 0; z < depth; z++)
	{
		for (by = 0; by < blocks_y; by++)
		{
			const unsigned char *row = (const unsigned char *)blocks + (uint64_t)z * bytes_per_image +
				(uint64_t)by * bytes_per_row;

			for (bx = 0; bx < blocks_x; bx++)
			{
				uint32_t texels[16];

				host_textures_bc_decode_block(kind, row + bx * block_bytes, texels);
				for (y = 0; y < 4 && by * 4 + y < height; y++)
				{
					uint32_t *out = bgra + ((uint64_t)z * height + by * 4 + y) * width + bx * 4;

					for (x = 0; x < 4 && bx * 4 + x < width; x++)
						out[x] = texels[y * 4 + x];
				}
			}
		}
	}
}

/* ---------- ASTC: ranges and the integer sequence encoding */

enum
{
	QUANT_2, QUANT_3, QUANT_4, QUANT_5, QUANT_6, QUANT_8, QUANT_10, QUANT_12, QUANT_16, QUANT_20, QUANT_24,
	QUANT_32, QUANT_40, QUANT_48, QUANT_64, QUANT_80, QUANT_96, QUANT_128, QUANT_160, QUANT_192, QUANT_256,
	NUMBER_OF_QUANTS
};

/* each range's bits, and whether it has a trit or a quint above them */
static const unsigned char quant_bits[NUMBER_OF_QUANTS] =
	{ 1, 0, 2, 0, 1, 3, 1, 2, 4, 2, 3, 5, 3, 4, 6, 4, 5, 7, 5, 6, 8 };
static const unsigned char quant_trits[NUMBER_OF_QUANTS] =
	{ 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0 };
static const unsigned char quant_quints[NUMBER_OF_QUANTS] =
	{ 0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0 };

static unsigned int quant_levels(int quant)
{
	return (1u << quant_bits[quant]) * (quant_trits[quant] ? 3u : 1u) * (quant_quints[quant] ? 5u : 1u);
}

static unsigned int ise_bits(unsigned int count, int quant)
{
	return count * quant_bits[quant] + (quant_trits[quant] ? (8 * count + 4) / 5 : 0) +
		(quant_quints[quant] ? (7 * count + 2) / 3 : 0);
}

/* 128 bits, bit 0 the first byte's lowest */
struct bits128
{
	uint64_t word[2];
};

static void bits_put(struct bits128 *bits, unsigned int offset, unsigned int count, uint32_t value)
{
	uint64_t v = (uint64_t)value & ((1ull << count) - 1);

	if (!count)
		return;
	if (offset < 64)
	{
		bits->word[0] |= v << offset;
		if (offset && offset + count > 64)
			bits->word[1] |= v >> (64 - offset);
	}
	else
	{
		bits->word[1] |= v << (offset - 64);
	}
}

static uint32_t bits_get(const struct bits128 *bits, unsigned int offset, unsigned int count)
{
	uint64_t v;

	if (!count)
		return 0;
	if (offset >= 64)
	{
		v = bits->word[1] >> (offset - 64);
	}
	else
	{
		v = bits->word[0] >> offset;
		if (offset && offset + count > 64)
			v |= bits->word[1] << (64 - offset);
	}
	return (uint32_t)(v & ((1ull << count) - 1));
}

static uint64_t reverse64(uint64_t v)
{
	v = ((v >> 1) & 0x5555555555555555ull) | ((v & 0x5555555555555555ull) << 1);
	v = ((v >> 2) & 0x3333333333333333ull) | ((v & 0x3333333333333333ull) << 2);
	v = ((v >> 4) & 0x0f0f0f0f0f0f0f0full) | ((v & 0x0f0f0f0f0f0f0f0full) << 4);
	v = ((v >> 8) & 0x00ff00ff00ff00ffull) | ((v & 0x00ff00ff00ff00ffull) << 8);
	v = ((v >> 16) & 0x0000ffff0000ffffull) | ((v & 0x0000ffff0000ffffull) << 16);
	return (v >> 32) | (v << 32);
}

static struct bits128 reverse128(struct bits128 bits)
{
	struct bits128 reversed;

	reversed.word[0] = reverse64(bits.word[1]);
	reversed.word[1] = reverse64(bits.word[0]);
	return reversed;
}

#define BIT(v, n) (((v) >> (n)) & 1u)
#define BITS(v, high, low) (((v) >> (low)) & ((1u << ((high) - (low) + 1)) - 1))

/* the five trits an 8-bit T holds, and the three quints of a 7-bit Q (the
specification's decoding) */
static void trits_of(unsigned int T, unsigned char t[5])
{
	unsigned int C;

	if (BITS(T, 4, 2) == 7)
	{
		C = (BITS(T, 7, 5) << 2) | BITS(T, 1, 0);
		t[4] = 2;
		t[3] = 2;
	}
	else
	{
		C = BITS(T, 4, 0);
		if (BITS(T, 6, 5) == 3)
		{
			t[4] = 2;
			t[3] = (unsigned char)BIT(T, 7);
		}
		else
		{
			t[4] = (unsigned char)BIT(T, 7);
			t[3] = (unsigned char)BITS(T, 6, 5);
		}
	}
	if (BITS(C, 1, 0) == 3)
	{
		t[2] = 2;
		t[1] = (unsigned char)BIT(C, 4);
		t[0] = (unsigned char)((BIT(C, 3) << 1) | (BIT(C, 2) & !BIT(C, 3)));
	}
	else if (BITS(C, 3, 2) == 3)
	{
		t[2] = 2;
		t[1] = 2;
		t[0] = (unsigned char)BITS(C, 1, 0);
	}
	else
	{
		t[2] = (unsigned char)BIT(C, 4);
		t[1] = (unsigned char)BITS(C, 3, 2);
		t[0] = (unsigned char)((BIT(C, 1) << 1) | (BIT(C, 0) & !BIT(C, 1)));
	}
}

static void quints_of(unsigned int Q, unsigned char q[3])
{
	unsigned int C;

	if (BITS(Q, 2, 1) == 3 && BITS(Q, 6, 5) == 0)
	{
		q[2] = (unsigned char)((BIT(Q, 0) << 2) | ((BIT(Q, 4) & !BIT(Q, 0)) << 1) | (BIT(Q, 3) & !BIT(Q, 0)));
		q[1] = 4;
		q[0] = 4;
		return;
	}
	if (BITS(Q, 2, 1) == 3)
	{
		q[2] = 4;
		C = (BITS(Q, 4, 3) << 3) | ((~BITS(Q, 6, 5) & 3) << 1) | BIT(Q, 0);
	}
	else
	{
		q[2] = (unsigned char)BITS(Q, 6, 5);
		C = BITS(Q, 4, 0);
	}
	if (BITS(C, 2, 0) == 5)
	{
		q[1] = 4;
		q[0] = (unsigned char)BITS(C, 4, 3);
	}
	else
	{
		q[1] = (unsigned char)BITS(C, 4, 3);
		q[0] = (unsigned char)BITS(C, 2, 0);
	}
}

/* T for each five trits (t0 + 3 t1 + 9 t2 + 27 t3 + 81 t4), Q for each
three quints (q0 + 5 q1 + 25 q2), and the trits and quints of each */
static unsigned char trit_pack[243], quint_pack[125];
static unsigned char trit_unpack[256][5], quint_unpack[128][3];

/* (where each trit's and quint's bits of T and Q go: after each value) */
static const unsigned char trit_bit_counts[5] = { 2, 2, 1, 2, 1 }, trit_bit_shifts[5] = { 0, 2, 4, 5, 7 };
static const unsigned char quint_bit_counts[3] = { 3, 2, 2 }, quint_bit_shifts[3] = { 0, 3, 5 };

static void ise_encode(int quant, unsigned int count, const unsigned char *values, struct bits128 *bits,
	unsigned int offset)
{
	unsigned int m = quant_bits[quant], mask = (1u << m) - 1, i, j;

	if (quant_trits[quant] || quant_quints[quant])
	{
		unsigned int group = quant_trits[quant] ? 5 : 3, base = quant_trits[quant] ? 3 : 5;
		const unsigned char *counts = quant_trits[quant] ? trit_bit_counts : quint_bit_counts;
		const unsigned char *shifts = quant_trits[quant] ? trit_bit_shifts : quint_bit_shifts;

		for (i = 0; i < count; i += group)
		{
			unsigned int index = 0, scale = 1, packed;

			for (j = 0; j < group; j++, scale *= base)
			{
				if (i + j < count)
					index += (values[i + j] >> m) * scale;
			}
			packed = quant_trits[quant] ? trit_pack[index] : quint_pack[index];
			for (j = 0; j < group && i + j < count; j++)
			{
				bits_put(bits, offset, m, values[i + j] & mask);
				offset += m;
				bits_put(bits, offset, counts[j], (packed >> shifts[j]) & ((1u << counts[j]) - 1));
				offset += counts[j];
			}
		}
		return;
	}
	for (i = 0; i < count; i++)
	{
		bits_put(bits, offset, m, values[i]);
		offset += m;
	}
}

static void ise_decode(int quant, unsigned int count, const struct bits128 *bits, unsigned int offset,
	unsigned char *values)
{
	unsigned int m = quant_bits[quant], i, j;

	if (quant_trits[quant] || quant_quints[quant])
	{
		unsigned int group = quant_trits[quant] ? 5 : 3;
		const unsigned char *counts = quant_trits[quant] ? trit_bit_counts : quint_bit_counts;
		const unsigned char *shifts = quant_trits[quant] ? trit_bit_shifts : quint_bit_shifts;

		for (i = 0; i < count; i += group)
		{
			unsigned int packed = 0, low[5] = { 0 };

			for (j = 0; j < group && i + j < count; j++)
			{
				low[j] = bits_get(bits, offset, m);
				offset += m;
				packed |= bits_get(bits, offset, counts[j]) << shifts[j];
				offset += counts[j];
			}
			for (j = 0; j < group && i + j < count; j++)
			{
				unsigned int high = quant_trits[quant] ? trit_unpack[packed][j] : quint_unpack[packed & 0x7f][j];

				values[i + j] = (unsigned char)((high << m) | low[j]);
			}
		}
		return;
	}
	for (i = 0; i < count; i++)
	{
		values[i] = (unsigned char)bits_get(bits, offset, m);
		offset += m;
	}
}

/* ---------- ASTC: unquantization */

/* an n-bit value replicated to fill bits */
static unsigned int replicate(unsigned int value, unsigned int n, unsigned int bits)
{
	unsigned int result = 0, bit;

	for (bit = 0; bit < bits; bit++)
		result |= ((value >> (n - 1 - bit % n)) & 1) << (bits - 1 - bit);
	return result;
}

/* a colour value of a range, 0-255 */
static unsigned int color_unquantize(int quant, unsigned int value)
{
	unsigned int m = quant_bits[quant], low = value & ((1u << m) - 1), D = value >> m, A, B, C, T;
	unsigned int b = BIT(low, 1), c = BIT(low, 2), d = BIT(low, 3), e = BIT(low, 4), f = BIT(low, 5);

	if (!quant_trits[quant] && !quant_quints[quant])
		return replicate(low, m, 8);
	A = (low & 1) ? 0x1ff : 0;
	switch (quant)
	{
	case QUANT_6: B = 0; C = 204; break;
	case QUANT_10: B = 0; C = 113; break;
	case QUANT_12: B = (b << 8) | (b << 4) | (b << 2) | (b << 1); C = 93; break;
	case QUANT_20: B = (b << 8) | (b << 3) | (b << 2); C = 54; break;
	case QUANT_24: B = (c << 8) | (b << 7) | (c << 3) | (b << 2) | (c << 1) | b; C = 44; break;
	case QUANT_40: B = (c << 8) | (b << 7) | (c << 2) | (b << 1) | c; C = 26; break;
	case QUANT_48: B = (d << 8) | (c << 7) | (b << 6) | (d << 2) | (c << 1) | b; C = 22; break;
	case QUANT_80: B = (d << 8) | (c << 7) | (b << 6) | (d << 1) | c; C = 11; break;
	case QUANT_96: B = (e << 8) | (d << 7) | (c << 6) | (b << 5) | (e << 1) | d; C = 10; break;
	case QUANT_160: B = (e << 8) | (d << 7) | (c << 6) | (b << 5) | e; C = 5; break;
	case QUANT_192: B = (f << 8) | (e << 7) | (d << 6) | (c << 5) | (b << 4) | f; C = 4; break;
	/* (QUANT_3 and QUANT_5 are never colour ranges) */
	default: return 0;
	}
	T = D * C + B;
	T ^= A;
	return (A & 0x80) | (T >> 2);
}

/* a weight of a range (QUANT_2 to QUANT_32), 0-64 */
static unsigned int weight_unquantize(int quant, unsigned int value)
{
	unsigned int m = quant_bits[quant], low = value & ((1u << m) - 1), D = value >> m, result;
	unsigned int b = BIT(low, 1), c = BIT(low, 2);

	if (!quant_trits[quant] && !quant_quints[quant])
	{
		result = replicate(low, m, 6);
	}
	else if (!m)
	{
		static const unsigned char trit[3] = { 0, 32, 63 }, quint[5] = { 0, 16, 32, 47, 63 };

		result = quant_trits[quant] ? trit[D] : quint[D];
	}
	else
	{
		unsigned int A = (low & 1) ? 0x7f : 0, B, C, T;

		switch (quant)
		{
		case QUANT_6: B = 0; C = 50; break;
		case QUANT_10: B = 0; C = 28; break;
		case QUANT_12: B = (b << 6) | (b << 2) | b; C = 23; break;
		case QUANT_20: B = (b << 6) | (b << 1); C = 13; break;
		case QUANT_24: B = (c << 6) | (b << 5) | (c << 1) | b; C = 11; break;
		default: return 0;
		}
		T = D * C + B;
		T ^= A;
		result = (A & 0x20) | (T >> 2);
	}
	return result > 32 ? result + 1 : result;
}

static unsigned char color_values[NUMBER_OF_QUANTS][256];
/* the value of a range nearest each 0-255 */
static unsigned char color_nearest[NUMBER_OF_QUANTS][256];
static unsigned char weight_values[QUANT_32 + 1][32];

/* ---------- ASTC: partitions */

static uint32_t hash52(uint32_t p)
{
	p ^= p >> 15;
	p -= p << 17;
	p += p << 7;
	p += p << 4;
	p ^= p >> 5;
	p += p << 16;
	p ^= p >> 7;
	p ^= p >> 3;
	p ^= p << 6;
	p ^= p >> 17;
	return p;
}

/* the partition of a texel of a 4x4 block (the specification's hash; a
block of fewer than 31 texels doubles its coordinates) */
static unsigned int select_partition(unsigned int seed, unsigned int x, unsigned int y, unsigned int count)
{
	uint32_t r;
	unsigned int s[12], sh1, sh2, sh3, index;
	int a, b, c, d;
	unsigned int z = 0;

	x <<= 1;
	y <<= 1;
	seed += (count - 1) * 1024;
	r = hash52(seed);
	s[0] = r & 0xf;
	s[1] = (r >> 4) & 0xf;
	s[2] = (r >> 8) & 0xf;
	s[3] = (r >> 12) & 0xf;
	s[4] = (r >> 16) & 0xf;
	s[5] = (r >> 20) & 0xf;
	s[6] = (r >> 24) & 0xf;
	s[7] = (r >> 28) & 0xf;
	s[8] = (r >> 18) & 0xf;
	s[9] = (r >> 22) & 0xf;
	s[10] = (r >> 26) & 0xf;
	s[11] = ((r >> 30) | (r << 2)) & 0xf;
	for (index = 0; index < 12; index++)
		s[index] *= s[index];
	if (seed & 1)
	{
		sh1 = (seed & 2) ? 4 : 5;
		sh2 = count == 3 ? 6 : 5;
	}
	else
	{
		sh1 = count == 3 ? 6 : 5;
		sh2 = (seed & 2) ? 4 : 5;
	}
	sh3 = (seed & 0x10) ? sh1 : sh2;
	for (index = 0; index < 8; index += 2)
	{
		s[index] >>= sh1;
		s[index + 1] >>= sh2;
	}
	for (index = 8; index < 12; index++)
		s[index] >>= sh3;
	a = (int)((s[0] * x + s[1] * y + s[10] * z + (r >> 14)) & 0x3f);
	b = (int)((s[2] * x + s[3] * y + s[11] * z + (r >> 10)) & 0x3f);
	c = (int)((s[4] * x + s[5] * y + s[8] * z + (r >> 6)) & 0x3f);
	d = (int)((s[6] * x + s[7] * y + s[9] * z + (r >> 2)) & 0x3f);
	if (count <= 3)
		d = 0;
	if (count <= 2)
		c = 0;
	if (a >= b && a >= c && a >= d)
		return 0;
	if (b >= c && b >= d)
		return 1;
	if (c >= d)
		return 2;
	return 3;
}

/* each seed's texels of each partition, two and three partitions */
static uint16_t partition_masks[2][1024][3];

/* The two-partition splits, each way round: the texels of partition A (the
one with a DXT1 block's transparent texels), its seed and which partition
it is. One seed of each split (the first). Bit c of contains[t] is whether
candidate c's A has texel t. */
#define SPLITS_MAXIMUM 2048
#define SPLIT_WORDS (SPLITS_MAXIMUM / 64)
static unsigned int split_count, split_words;
static uint16_t split_mask[SPLITS_MAXIMUM], split_seed[SPLITS_MAXIMUM];
static unsigned char split_a[SPLITS_MAXIMUM];
static uint64_t split_contains[16][SPLIT_WORDS];

static pthread_once_t tables_once = PTHREAD_ONCE_INIT;

static void astc_tables_make(void)
{
	static unsigned char seen[65536 / 8];
	unsigned int index, quant, value, seed, count, texel;

	memset(trit_pack, 0xff, sizeof(trit_pack));
	for (index = 255; index < 256; index--)
	{
		trits_of(index, trit_unpack[index]);
		trit_pack[trit_unpack[index][0] + 3 * trit_unpack[index][1] + 9 * trit_unpack[index][2] +
			27 * trit_unpack[index][3] + 81 * trit_unpack[index][4]] = (unsigned char)index;
	}
	for (index = 127; index < 128; index--)
	{
		quints_of(index, quint_unpack[index]);
		quint_pack[quint_unpack[index][0] + 5 * quint_unpack[index][1] + 25 * quint_unpack[index][2]] =
			(unsigned char)index;
	}
	for (quant = QUANT_6; quant < NUMBER_OF_QUANTS; quant++)
	{
		unsigned int levels = quant_levels((int)quant);

		for (value = 0; value < levels; value++)
			color_values[quant][value] = (unsigned char)color_unquantize((int)quant, value);
		for (index = 0; index < 256; index++)
		{
			unsigned int best = 0, best_distance = 1000;

			for (value = 0; value < levels; value++)
			{
				unsigned int distance = color_values[quant][value] > index ? color_values[quant][value] - index :
					index - color_values[quant][value];

				if (distance < best_distance)
				{
					best = value;
					best_distance = distance;
				}
			}
			color_nearest[quant][index] = (unsigned char)best;
		}
	}
	for (quant = QUANT_2; quant <= QUANT_32; quant++)
	{
		for (value = 0; value < quant_levels((int)quant); value++)
			weight_values[quant][value] = (unsigned char)weight_unquantize((int)quant, value);
	}
	for (count = 2; count <= 3; count++)
	{
		for (seed = 0; seed < 1024; seed++)
		{
			memset(partition_masks[count - 2][seed], 0, sizeof(partition_masks[0][0]));
			for (texel = 0; texel < 16; texel++)
				partition_masks[count - 2][seed][select_partition(seed, texel & 3, texel >> 2, count)] |=
					(uint16_t)(1u << texel);
		}
	}
	memset(seen, 0, sizeof(seen));
	memset(split_contains, 0, sizeof(split_contains));
	split_count = 0;
	for (seed = 0; seed < 1024; seed++)
	{
		unsigned int mask = partition_masks[0][seed][0], side;

		if (!mask || mask == 0xffff || (seen[mask >> 3] & (1u << (mask & 7))))
			continue;
		for (side = 0; side < 2; side++)
		{
			unsigned int a = side ? (~mask & 0xffff) : mask;

			seen[a >> 3] |= (unsigned char)(1u << (a & 7));
			split_mask[split_count] = (uint16_t)a;
			split_seed[split_count] = (uint16_t)seed;
			split_a[split_count] = (unsigned char)side;
			for (texel = 0; texel < 16; texel++)
			{
				if (a & (1u << texel))
					split_contains[texel][split_count / 64] |= 1ull << (split_count % 64);
			}
			split_count++;
		}
	}
	split_words = (split_count + 63) / 64;
}

/* (once, whichever thread converts first) */
static void astc_tables(void)
{
	pthread_once(&tables_once, astc_tables_make);
}

/* ---------- ASTC: blocks */

/* what a block holds, before its values are quantized: the partitions
(and the texels of each), each one's endpoint mode and values (0-255, in
the mode's order) and each texel's weights (0-64, plane 1 and 2: alpha) */
struct astc_description
{
	unsigned int partitions, seed, dual;
	int weight_quant;
	unsigned int modes[3];
	unsigned int values[3][8];
	unsigned char partition_of[16];
	unsigned char weights[16][2];
};

static unsigned int mode_values(unsigned int mode)
{
	return 2 * (mode >> 2) + 2;
}

/* the block mode of a 4x4 grid of weights of a range (QUANT_2 to QUANT_32),
one plane or two */
static unsigned int block_mode_4x4(int weight_quant, unsigned int dual)
{
	unsigned int r = 2 + (unsigned int)weight_quant % 6, high = (unsigned int)weight_quant / 6;

	return (r >> 1) | ((r & 1) << 4) | (2u << 5) | (high << 9) | (dual << 10);
}

static unsigned int nearest_weight(int quant, unsigned int weight)
{
	unsigned int value, best = 0, best_distance = 1000;

	for (value = 0; value < quant_levels(quant); value++)
	{
		unsigned int level = weight_values[quant][value];
		unsigned int distance = level > weight ? level - weight : weight - level;

		if (distance < best_distance)
		{
			best = value;
			best_distance = distance;
		}
	}
	return best;
}

/* the colour range a block's endpoints get: the largest that fits */
static int color_quant_of(unsigned int count, int color_bits)
{
	int quant;

	for (quant = QUANT_256; quant >= QUANT_6; quant--)
	{
		if ((int)ise_bits(count, quant) <= color_bits)
			return quant;
	}
	return -1;
}

static int astc_pack(const struct astc_description *description, unsigned char out[16])
{
	unsigned int planes = description->dual ? 2 : 1, weight_count = 16 * planes;
	unsigned int weight_bits = ise_bits(weight_count, description->weight_quant);
	unsigned int header = description->partitions == 1 ? 17 : 29, extra = 0, count = 0, partition, index;
	unsigned int lowest_class = 3, matched = 1, invert[3] = { 0, 0, 0 };
	unsigned char colors[18], weights[32];
	struct bits128 block = { { 0, 0 } }, stream = { { 0, 0 } };
	int color_quant;

	for (partition = 0; partition < description->partitions; partition++)
	{
		count += mode_values(description->modes[partition]);
		if ((description->modes[partition] >> 2) < lowest_class)
			lowest_class = description->modes[partition] >> 2;
		matched &= description->modes[partition] == description->modes[0];
	}
	if (description->partitions > 1 && !matched)
		extra = 3 * description->partitions - 4;
	if (count > 18 || weight_bits < 24 || weight_bits > 96)
		return 0;
	color_quant = color_quant_of(count, 128 - (int)header - (int)weight_bits - (int)extra - (description->dual ? 2 : 0));
	if (color_quant < 0)
		return 0;
	count = 0;
	for (partition = 0; partition < description->partitions; partition++)
	{
		unsigned int mode = description->modes[partition], n = mode_values(mode), value;

		for (value = 0; value < n; value++)
			colors[count + value] = color_nearest[color_quant][description->values[partition][value] & 0xff];
		/* (RGB and RGBA direct swap their endpoints, contracting blue, when the
		first is the brighter: the endpoints go the other way round, and the
		weights with them) */
		if (mode == 8 || mode == 12)
		{
			const unsigned char *unquantized = color_values[color_quant];
			unsigned int s0 = unquantized[colors[count]] + unquantized[colors[count + 2]] + unquantized[colors[count + 4]];
			unsigned int s1 = unquantized[colors[count + 1]] + unquantized[colors[count + 3]] + unquantized[colors[count + 5]];

			if (s1 < s0)
			{
				for (value = 0; value < n; value += 2)
				{
					unsigned char swap = colors[count + value];

					colors[count + value] = colors[count + value + 1];
					colors[count + value + 1] = swap;
				}
				invert[partition] = 1;
			}
		}
		count += n;
	}
	for (index = 0; index < 16; index++)
	{
		unsigned int plane;

		for (plane = 0; plane < planes; plane++)
		{
			unsigned int weight = description->weights[index][plane];

			if (invert[description->partition_of[index]])
				weight = 64 - weight;
			weights[index * planes + plane] = (unsigned char)nearest_weight(description->weight_quant, weight);
		}
	}
	bits_put(&block, 0, 11, block_mode_4x4(description->weight_quant, description->dual));
	bits_put(&block, 11, 2, description->partitions - 1);
	if (description->partitions == 1)
	{
		bits_put(&block, 13, 4, description->modes[0]);
	}
	else
	{
		bits_put(&block, 13, 10, description->seed);
		if (matched)
		{
			bits_put(&block, 23, 6, description->modes[0] << 2);
		}
		else
		{
			unsigned int encoded, at = 2;

			if (lowest_class == 3)
				lowest_class = 2;
			encoded = lowest_class + 1;
			for (partition = 0; partition < description->partitions; partition++)
			{
				if ((description->modes[partition] >> 2) - lowest_class > 1)
					return 0;
				encoded |= ((description->modes[partition] >> 2) - lowest_class) << at++;
			}
			for (partition = 0; partition < description->partitions; partition++, at += 2)
				encoded |= (description->modes[partition] & 3) << at;
			bits_put(&block, 23, 6, encoded & 0x3f);
			bits_put(&block, 128 - weight_bits - extra, extra, encoded >> 6);
		}
	}
	if (description->dual)
		bits_put(&block, 128 - weight_bits - extra - 2, 2, 3);
	ise_encode(color_quant, count, colors, &block, header);
	ise_encode(description->weight_quant, weight_count, weights, &stream, 0);
	stream = reverse128(stream);
	block.word[0] |= stream.word[0];
	block.word[1] |= stream.word[1];
	for (index = 0; index < 16; index++)
		out[index] = (unsigned char)(block.word[index / 8] >> ((index % 8) * 8));
	return 1;
}

/* an endpoint mode's endpoints (RGBA, 0-255); 0 for one this does not know */
static int endpoints(unsigned int mode, const unsigned int *v, unsigned int e0[4], unsigned int e1[4])
{
	unsigned int channel;

	switch (mode)
	{
	case 0:
		e0[0] = e0[1] = e0[2] = v[0];
		e1[0] = e1[1] = e1[2] = v[1];
		e0[3] = e1[3] = 255;
		return 1;
	case 4:
		e0[0] = e0[1] = e0[2] = v[0];
		e1[0] = e1[1] = e1[2] = v[1];
		e0[3] = v[2];
		e1[3] = v[3];
		return 1;
	case 6:
	case 10:
		for (channel = 0; channel < 3; channel++)
		{
			e0[channel] = (v[channel] * v[3]) >> 8;
			e1[channel] = v[channel];
		}
		e0[3] = mode == 10 ? v[4] : 255;
		e1[3] = mode == 10 ? v[5] : 255;
		return 1;
	case 8:
	case 12:
	{
		unsigned int s0 = v[0] + v[2] + v[4], s1 = v[1] + v[3] + v[5];

		if (s1 >= s0)
		{
			for (channel = 0; channel < 3; channel++)
			{
				e0[channel] = v[channel * 2];
				e1[channel] = v[channel * 2 + 1];
			}
			e0[3] = mode == 12 ? v[6] : 255;
			e1[3] = mode == 12 ? v[7] : 255;
		}
		else
		{
			/* (blue contracted, and the endpoints the other way round) */
			e0[0] = (v[1] + v[5]) >> 1;
			e0[1] = (v[3] + v[5]) >> 1;
			e0[2] = v[5];
			e1[0] = (v[0] + v[4]) >> 1;
			e1[1] = (v[2] + v[4]) >> 1;
			e1[2] = v[4];
			e0[3] = mode == 12 ? v[7] : 255;
			e1[3] = mode == 12 ? v[6] : 255;
		}
		return 1;
	}
	default:
		return 0;
	}
}

int host_textures_astc_decode(const unsigned char astc[16], uint32_t texels[16])
{
	struct bits128 block = { { 0, 0 } }, stream;
	unsigned int mode, partitions, seed = 0, modes[3], extra = 0, count = 0, partition, index, planes;
	unsigned int weight_bits, weight_count, component = 4, values[18], e[3][2][4];
	unsigned char colors[18], weights[32];
	int weight_quant, color_quant;

	astc_tables();
	for (index = 0; index < 16; index++)
		block.word[index / 8] |= (uint64_t)astc[index] << ((index % 8) * 8);
	mode = bits_get(&block, 0, 11);
	if ((mode & 0x1ff) == 0x1fc)
	{
		/* (a constant colour: 16-bit RGBA) */
		uint32_t texel = 0;

		if (mode & 0x200)
			return 0;
		for (index = 0; index < 4; index++)
		{
			unsigned int v = bits_get(&block, 64 + index * 16, 16);

			texel |= (uint32_t)((v * 255 + 32767) / 65535) << (index == 3 ? 24 : 16 - index * 8);
		}
		for (index = 0; index < 16; index++)
			texels[index] = texel;
		return 1;
	}
	/* (only the 4x4 grids, the modes this writes) */
	if (!(mode & 3) || ((mode >> 2) & 3) || ((mode >> 5) & 3) != 2 || ((mode >> 7) & 3))
		return 0;
	weight_quant = (int)(((mode >> 4) & 1) | ((mode & 3) << 1)) - 2 + 6 * (int)((mode >> 9) & 1);
	planes = (mode >> 10) & 1 ? 2 : 1;
	if (weight_quant > QUANT_32)
		return 0;
	weight_count = 16 * planes;
	weight_bits = ise_bits(weight_count, weight_quant);
	partitions = bits_get(&block, 11, 2) + 1;
	if (weight_bits < 24 || weight_bits > 96 || partitions > 3 || (planes == 2 && partitions == 4))
		return 0;
	if (partitions == 1)
	{
		modes[0] = bits_get(&block, 13, 4);
	}
	else
	{
		unsigned int encoded = bits_get(&block, 23, 6), base = encoded & 3;

		seed = bits_get(&block, 13, 10);
		if (!base)
		{
			for (partition = 0; partition < partitions; partition++)
				modes[partition] = encoded >> 2;
		}
		else
		{
			extra = 3 * partitions - 4;
			encoded |= bits_get(&block, 128 - weight_bits - extra, extra) << 6;
			for (partition = 0; partition < partitions; partition++)
			{
				modes[partition] = ((((encoded >> (2 + partition)) & 1) + base - 1) << 2) |
					((encoded >> (2 + partitions + 2 * partition)) & 3);
			}
		}
	}
	for (partition = 0; partition < partitions; partition++)
		count += mode_values(modes[partition]);
	if (count > 18)
		return 0;
	color_quant = color_quant_of(count, 128 - (partitions == 1 ? 17 : 29) - (int)weight_bits - (int)extra -
		(planes == 2 ? 2 : 0));
	if (color_quant < 0)
		return 0;
	if (planes == 2)
		component = bits_get(&block, 128 - weight_bits - extra - 2, 2);
	ise_decode(color_quant, count, &block, partitions == 1 ? 17 : 29, colors);
	for (index = 0; index < count; index++)
		values[index] = color_values[color_quant][colors[index]];
	count = 0;
	for (partition = 0; partition < partitions; partition++)
	{
		if (!endpoints(modes[partition], values + count, e[partition][0], e[partition][1]))
			return 0;
		count += mode_values(modes[partition]);
	}
	stream = reverse128(block);
	ise_decode(weight_quant, weight_count, &stream, 0, weights);
	for (index = 0; index < 16; index++)
	{
		unsigned int part = 0;
		unsigned int channel;
		uint32_t texel = 0;

		if (partitions > 1)
			part = partition_masks[partitions - 2][seed][1] & (1u << index) ? 1 :
				partition_masks[partitions - 2][seed][2] & (1u << index) ? 2 : 0;
		for (channel = 0; channel < 4; channel++)
		{
			/* (channels red, green, blue, alpha: alpha is the texel's top byte) */
			unsigned int weight = weight_values[weight_quant][weights[index * planes +
				(planes == 2 && channel == component ? 1 : 0)]];
			unsigned int c0 = e[part][0][channel] * 257, c1 = e[part][1][channel] * 257;
			unsigned int v = (c0 * (64 - weight) + c1 * weight + 32) >> 6;

			texel |= (uint32_t)((v * 255 + 32767) / 65535) << (channel == 3 ? 24 : 16 - channel * 8);
		}
		texels[index] = texel;
	}
	return 1;
}

/* ---------- DXT blocks into ASTC */

static unsigned int channel_of(uint32_t texel, unsigned int channel)
{
	return (texel >> (channel == 3 ? 24 : 16 - channel * 8)) & 0xff;
}

/* the largest difference of a channel of a texel, and the squares' sum */
static unsigned int block_error(const uint32_t reference[16], const uint32_t texels[16], uint32_t *squares)
{
	unsigned int index, channel, worst = 0;
	uint32_t sum = 0;

	for (index = 0; index < 16; index++)
	{
		for (channel = 0; channel < 4; channel++)
		{
			int difference = (int)channel_of(reference[index], channel) - (int)channel_of(texels[index], channel);
			unsigned int magnitude = (unsigned int)(difference < 0 ? -difference : difference);

			if (magnitude > worst)
				worst = magnitude;
			sum += magnitude * magnitude;
		}
	}
	if (squares)
		*squares = sum;
	return worst;
}

/* a candidate packed and measured: it replaces the best so far if nearer */
struct astc_best
{
	int measure;
	unsigned int error;
	uint32_t squares;
	unsigned char block[16];
};

static void best_start(struct astc_best *best, int measure)
{
	best->measure = measure;
	best->error = 1000;
	best->squares = 0xffffffffu;
	/* (a constant black block, should no candidate pack: none fails to) */
	memset(best->block, 0, 16);
	best->block[0] = 0xfc;
	best->block[1] = 0xfd;
	memset(best->block + 2, 0xff, 6);
}

/* (only: the one candidate there is, which needs measuring only when the
error is asked for) */
static void consider(const struct astc_description *description, const uint32_t reference[16], struct astc_best *best,
	int only)
{
	unsigned char block[16];
	uint32_t texels[16], squares;
	unsigned int error;

	if (only && !best->measure)
	{
		if (astc_pack(description, best->block))
			best->error = 0;
		return;
	}
	if (!astc_pack(description, block) || !host_textures_astc_decode(block, texels))
		return;
	error = block_error(reference, texels, &squares);
	if (error < best->error || (error == best->error && squares < best->squares))
	{
		best->error = error;
		best->squares = squares;
		memcpy(best->block, block, 16);
	}
}

static void set_rgb(unsigned int *values, uint32_t first, uint32_t second)
{
	unsigned int channel;

	for (channel = 0; channel < 3; channel++)
	{
		values[channel * 2] = channel_of(first, channel);
		values[channel * 2 + 1] = channel_of(second, channel);
	}
}

/* the weights a DXT colour index gets on its endpoints' line: four colours
(1/3 and 2/3 as 21/64 and 43/64) and three (1/2) */
static const unsigned char four_weights[4] = { 0, 64, 21, 43 }, three_weights[4] = { 0, 64, 32, 0 };

/* a DXT1 block of transparency and colours: partition A has the
transparent texels and those of one colour (index colour, or none: 3), on
the line from transparent black to it (endpoint mode 10, scale 0); B the
others, on the block's line */
static void bc1_split(const uint32_t palette[4], const unsigned char indices[16], unsigned int split,
	unsigned int colour, struct astc_description *description)
{
	unsigned int a = split_a[split], index;
	uint32_t x = palette[colour < 3 ? colour : 0];

	memset(description, 0, sizeof(*description));
	description->partitions = 2;
	description->seed = split_seed[split];
	description->weight_quant = QUANT_3;
	description->modes[a] = 10;
	description->values[a][0] = channel_of(x, 0);
	description->values[a][1] = channel_of(x, 1);
	description->values[a][2] = channel_of(x, 2);
	description->values[a][3] = 0;
	description->values[a][4] = 0;
	description->values[a][5] = 255;
	description->modes[1 - a] = 8;
	set_rgb(description->values[1 - a], palette[0], palette[1]);
	for (index = 0; index < 16; index++)
	{
		unsigned int in_a = (split_mask[split] >> index) & 1;

		description->partition_of[index] = (unsigned char)(in_a ? a : 1 - a);
		description->weights[index][0] = in_a ? (indices[index] == 3 ? 0 : 64) : three_weights[indices[index]];
	}
}

/* the first split whose A has every texel of must and none of mustnt,
given the splits with every texel of must (candidates); or -1 */
static int split_find(const uint64_t candidates[SPLIT_WORDS], unsigned int mustnt)
{
	uint64_t words[SPLIT_WORDS];
	unsigned int word, texel;

	memcpy(words, candidates, sizeof(words));
	for (texel = 0; texel < 16; texel++)
	{
		if (mustnt & (1u << texel))
		{
			for (word = 0; word < split_words; word++)
				words[word] &= ~split_contains[texel][word];
		}
	}
	for (word = 0; word < split_words; word++)
	{
		if (words[word])
			return (int)(word * 64 + (unsigned int)__builtin_ctzll(words[word]));
	}
	return -1;
}

static unsigned int bc1_to_astc(const unsigned char *block, unsigned char astc[16], int measure)
{
	uint32_t reference[16], palette[4];
	unsigned char indices[16];
	unsigned int masks[4] = { 0, 0, 0, 0 }, index, colours = 0, colour = 3;
	unsigned int four = (block[0] | (block[1] << 8)) > (block[2] | (block[3] << 8));
	struct astc_description description;
	struct astc_best best;

	bc_colors(_host_texture_bc1, block, palette, indices);
	for (index = 0; index < 16; index++)
	{
		reference[index] = palette[indices[index]];
		masks[indices[index]] |= 1u << index;
	}
	for (index = 0; index < 3; index++)
	{
		if (masks[index])
		{
			colours++;
			colour = index;
		}
	}
	best_start(&best, measure);
	memset(&description, 0, sizeof(description));
	description.partitions = 1;
	if (four || !masks[3])
	{
		/* one line: the block's own */
		description.weight_quant = four ? QUANT_4 : QUANT_3;
		description.modes[0] = 8;
		set_rgb(description.values[0], palette[0], palette[1]);
		for (index = 0; index < 16; index++)
			description.weights[index][0] = (four ? four_weights : three_weights)[indices[index]];
		consider(&description, reference, &best, 1);
	}
	else if (colours <= 1)
	{
		/* transparent black to the one colour */
		uint32_t x = palette[colour < 3 ? colour : 0];

		description.weight_quant = QUANT_3;
		description.modes[0] = 12;
		set_rgb(description.values[0], 0, x);
		description.values[0][6] = 0;
		description.values[0][7] = 255;
		for (index = 0; index < 16; index++)
			description.weights[index][0] = indices[index] == 3 ? 0 : 64;
		consider(&description, reference, &best, 1);
	}
	else
	{
		uint64_t candidates[SPLIT_WORDS];
		unsigned int word, opaque = masks[0] | masks[1] | masks[2], set;
		int split = -1;

		/* the splits whose A has every transparent texel */
		for (word = 0; word < split_words; word++)
			candidates[word] = ~0ull;
		for (index = 0; index < 16; index++)
		{
			if (masks[3] & (1u << index))
			{
				for (word = 0; word < split_words; word++)
					candidates[word] &= split_contains[index][word];
			}
		}
		if (split_count % 64)
			candidates[split_words - 1] &= (1ull << (split_count % 64)) - 1;
		/* exact: A has the transparent texels and those of one colour at most */
		for (index = 0; index < 3 && split < 0; index++)
		{
			colour = (index + 2) % 3;
			if (masks[colour])
				split = split_find(candidates, opaque & ~masks[colour]);
		}
		if (split >= 0)
		{
			bc1_split(palette, indices, (unsigned int)split, colour, &description);
			consider(&description, reference, &best, 1);
		}
		else
		{
			/* else approximately: A's colours become one, their middle ... */
			static const unsigned char sets[4] = { 5, 6, 3, 7 };

			for (set = 0; set < 4; set++)
			{
				unsigned int mustnt = opaque, channel, k;
				unsigned int low[3] = { 255, 255, 255 }, high[3] = { 0, 0, 0 };

				for (k = 0; k < 3; k++)
				{
					if (sets[set] & (1u << k))
					{
						mustnt &= ~masks[k];
						for (channel = 0; channel < 3; channel++)
						{
							unsigned int v = channel_of(palette[k], channel);

							low[channel] = v < low[channel] ? v : low[channel];
							high[channel] = v > high[channel] ? v : high[channel];
						}
					}
				}
				split = split_find(candidates, mustnt);
				if (split < 0)
					continue;
				bc1_split(palette, indices, (unsigned int)split, 0, &description);
				for (channel = 0; channel < 3; channel++)
					description.values[split_a[split]][channel] = (low[channel] + high[channel] + 1) / 2;
				consider(&description, reference, &best, 0);
			}
			/* ... or the transparent texels keep alpha 0 (a plane of its own)
			but a colour of the block's line: the darkest */
			{
				unsigned int dark0 = 0, dark1 = 0, dark2 = 0, channel, w;

				for (channel = 0; channel < 3; channel++)
				{
					dark0 = channel_of(palette[0], channel) > dark0 ? channel_of(palette[0], channel) : dark0;
					dark1 = channel_of(palette[1], channel) > dark1 ? channel_of(palette[1], channel) : dark1;
					dark2 = channel_of(palette[2], channel) > dark2 ? channel_of(palette[2], channel) : dark2;
				}
				w = dark0 <= dark1 && dark0 <= dark2 ? 0 : dark1 <= dark2 ? 64 : 32;
				memset(&description, 0, sizeof(description));
				description.partitions = 1;
				description.dual = 1;
				description.weight_quant = QUANT_3;
				description.modes[0] = 12;
				set_rgb(description.values[0], palette[0], palette[1]);
				description.values[0][6] = 0;
				description.values[0][7] = 255;
				for (index = 0; index < 16; index++)
				{
					description.weights[index][0] = (unsigned char)(indices[index] == 3 ? w : three_weights[indices[index]]);
					description.weights[index][1] = indices[index] == 3 ? 0 : 64;
				}
				consider(&description, reference, &best, 0);
			}
		}
	}
	memcpy(astc, best.block, 16);
	return best.error;
}

/* a DXT3 or DXT5 block: its colours on their line, its alpha with them
when it is one value, else a plane of its own of four levels */
static unsigned int bc23_to_astc(int kind, const unsigned char *block, unsigned char astc[16], int measure)
{
	uint32_t reference[16], palette[4];
	unsigned char indices[16];
	unsigned int index, low = 255, high = 0;
	struct astc_description description;
	struct astc_best best;

	host_textures_bc_decode_block(kind, block, reference);
	bc_colors(kind, block + 8, palette, indices);
	for (index = 0; index < 16; index++)
	{
		unsigned int alpha = reference[index] >> 24;

		low = alpha < low ? alpha : low;
		high = alpha > high ? alpha : high;
	}
	best_start(&best, measure);
	memset(&description, 0, sizeof(description));
	description.partitions = 1;
	description.modes[0] = 12;
	description.weight_quant = QUANT_4;
	set_rgb(description.values[0], palette[0], palette[1]);
	description.values[0][6] = low;
	description.values[0][7] = high;
	for (index = 0; index < 16; index++)
		description.weights[index][0] = four_weights[indices[index]];
	if (low == high)
	{
		consider(&description, reference, &best, 1);
	}
	else
	{
		unsigned int attempt;

		description.dual = 1;
		/* (the alpha's range as it is, and a little inside it: four levels of
		a range narrowed by a sixth reach its middle values better) */
		for (attempt = 0; attempt < 3; attempt++)
		{
			unsigned int inset = attempt * (high - low) / 12, a0 = low + inset, a1 = high - inset;

			if (attempt && (a1 <= a0 || !inset))
				break;
			description.values[0][6] = a0;
			description.values[0][7] = a1;
			for (index = 0; index < 16; index++)
			{
				int alpha = (int)(reference[index] >> 24), weight;

				weight = ((alpha - (int)a0) * 64 + (int)(a1 - a0) / 2) / (int)(a1 - a0);
				description.weights[index][1] = (unsigned char)(weight < 0 ? 0 : weight > 64 ? 64 : weight);
			}
			consider(&description, reference, &best, 0);
		}
	}
	memcpy(astc, best.block, 16);
	return best.error;
}

int host_textures_astc_block(int kind, const unsigned char *block, unsigned char astc[16])
{
	astc_tables();
	return (int)(kind == _host_texture_bc1 ? bc1_to_astc(block, astc, 1) : bc23_to_astc(kind, block, astc, 1));
}

int host_textures_bc_to_astc(int kind, const void *blocks, unsigned int bytes_per_row, unsigned int bytes_per_image,
	unsigned int blocks_x, unsigned int blocks_y, unsigned int depth, unsigned char *astc,
	unsigned long counts[NUMBER_OF_HOST_TEXTURE_ASTC_ERRORS])
{
	unsigned int block_bytes = host_textures_bc_block_bytes(kind), x, y, z;
	int worst = 0;

	astc_tables();
	for (z = 0; z < depth; z++)
	{
		for (y = 0; y < blocks_y; y++)
		{
			const unsigned char *row = (const unsigned char *)blocks + (uint64_t)z * bytes_per_image +
				(uint64_t)y * bytes_per_row;

			for (x = 0; x < blocks_x; x++)
			{
				const unsigned char *block = row + x * block_bytes;
				int error = (int)(kind == _host_texture_bc1 ? bc1_to_astc(block, astc, counts != NULL) :
					bc23_to_astc(kind, block, astc, counts != NULL));

				if (counts)
					counts[error <= 2 ? _astc_exact : error <= 4 ? _astc_rounded : _astc_approximated]++;
				worst = error > worst ? error : worst;
				astc += 16;
			}
		}
	}
	return worst;
}
