/* tinyd: see tinyd.h.  Fixed Huffman + stored blocks, with a preset window. */
#include "tinyd.h"
#include <string.h>

static const uint16_t len_base[29] = {
	3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,
	131,163,195,227,258
};
static const uint8_t len_extra[29] = {
	0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const uint16_t dist_base[30] = {
	1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,
	2049,3073,4097,6145,8193,12289,16385,24577
};
static const uint8_t dist_extra[30] = {
	0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

/* One-level lookup tables, built once.  Literal/length needs 9 bits (the
 * longest fixed code), distances 5.  A slower bit-at-a-time canonical decoder
 * would be a third of this code and two to three times the cycles, and cycles
 * are the currency here. */
#define FAST_BITS 9
#define FAST_SIZE (1u << FAST_BITS)
static uint16_t lit_sym[FAST_SIZE];
static uint8_t lit_bits[FAST_SIZE];
static uint16_t dist_sym[FAST_SIZE];
static uint8_t dist_bits[FAST_SIZE];
static int tables_built;

static void build_table(const uint8_t *lens, unsigned nsym, unsigned fastbits,
			uint16_t *sym, uint8_t *bits, int is_fixed)
{
	unsigned count[16] = {0}, offs[16], i;
	uint16_t next[16];
	unsigned code = 0;
	(void)is_fixed;

	for (i = 0; i < nsym; i++)
		count[lens[i]]++;
	count[0] = 0;
	/* Shift first, then store: the first code of length i is
	 * (first code of length i-1 + count of length i-1) shifted once.  Storing
	 * before the shift hands every length an offset one step too early, which
	 * silently overlays the 8-bit codes on the 7-bit range. */
	for (i = 1; i < 16; i++) {
		code = (code + count[i - 1]) << 1;
		offs[i] = code;
	}
	for (i = 0; i < 16; i++)
		next[i] = offs[i];
	/* Canonical codes are assigned in symbol order; the table is written as
	 * all bit patterns that share the code, reversed into LSB-first order. */
	for (i = 0; i < nsym; i++) {
		unsigned len = lens[i], j;
		if (!len)
			continue;
		code = next[len]++;
		{
			unsigned rev = 0, c = code;
			for (j = 0; j < len; j++) {
				rev = (rev << 1) | (c & 1);
				c >>= 1;
			}
			for (j = rev; j < FAST_SIZE; j += (1u << len)) {
				sym[j] = (uint16_t)i;
				bits[j] = (uint8_t)len;
			}
		}
	}
	(void)fastbits;
}

static void build_tables(void)
{
	uint8_t llens[288], dlens[32];
	unsigned i;

	if (tables_built)
		return;
	/* RFC 1951 3.2.5, generated rather than typed: 0-143 are 8 bits, 144-255 are
	 * 9, 256-279 are 7 and 280-287 are 8.  (A hand-written copy of this table
	 * was 64 entries short and decoded nothing.) */
	for (i = 0; i < 144; i++) llens[i] = 8;
	for (; i < 256; i++)      llens[i] = 9;
	for (; i < 280; i++)      llens[i] = 7;
	for (; i < 288; i++)      llens[i] = 8;
	memset(lit_sym, 0, sizeof lit_sym);
	memset(lit_bits, 0, sizeof lit_bits);
	memset(dist_sym, 0, sizeof dist_sym);
	memset(dist_bits, 0, sizeof dist_bits);
	build_table(llens, 288, FAST_BITS, lit_sym, lit_bits, 1);
	for (i = 0; i < 32; i++) dlens[i] = 5;
	build_table(dlens, 32, 5, dist_sym, dist_bits, 0);
	tables_built = 1;
}

/* LSB-first bit reader over in[0..in_len). */
struct bits {
	const uint8_t *p;
	size_t len, pos;
	uint32_t acc;
	unsigned n;
	int eof;
	/* A consume past the end of the input.  Without it the accumulator's
	 * stale bits look like a valid code stream and a *truncated* stream
	 * decodes to rubbish with a success return -- measured: all 1405
	 * prefixes of a valid stream were accepted, the first ones producing a
	 * byte or two of nothing.  n is unsigned, so the underflow used to wrap
	 * to a huge value and the "eof && n == 0" test never fired. */
	int overrun;
};

static void br_init(struct bits *b, const uint8_t *p, size_t len)
{
	b->p = p; b->len = len; b->pos = 0; b->acc = 0; b->n = 0; b->eof = 0;
	b->overrun = 0;
}

static void br_fill(struct bits *b)
{
	while (b->n <= 24) {
		if (b->pos >= b->len) {
			b->eof = 1;
			return;
		}
		b->acc |= (uint32_t)b->p[b->pos++] << b->n;
		b->n += 8;
	}
}

static unsigned br_peek(struct bits *b, unsigned n)
{
	br_fill(b);
	return b->acc & ((1u << n) - 1);
}

static void br_drop(struct bits *b, unsigned n)
{
	if (n > b->n) {
		b->overrun = 1;
		b->n = 0;
		b->acc = 0;
		return;
	}
	b->acc >>= n;
	b->n -= n;
}

/* Decode one symbol from a one-level table; returns the bit length used. */
static int decode_sym(struct bits *b, const uint16_t *sym, const uint8_t *bits,
		      unsigned nsym, unsigned *out)
{
	unsigned idx, len, s;

	br_fill(b);
	idx = b->acc & (FAST_SIZE - 1);
	len = bits[idx];
	if (!len)
		return -1;		/* not a code in this table */
	s = sym[idx];
	if (s >= nsym)
		return -1;
	br_drop(b, len);
	*out = s;
	return 0;
}

int tinyd_inflate(const uint8_t *in, size_t in_len,
		  uint8_t *win, size_t dict_len, size_t out_max, size_t *out_len)
{
	struct bits b;
	size_t pos = dict_len;		/* write cursor inside win */
	int final = 0;

	build_tables();
	br_init(&b, in, in_len);

	while (!final) {
		unsigned btype;

		if (b.overrun || (b.eof && b.n == 0))
			return TINYD_ERR_TRUNCATED;
		final = (int)br_peek(&b, 1);
		br_drop(&b, 1);
		btype = br_peek(&b, 2);
		br_drop(&b, 2);

		if (btype == 0) {			/* stored */
			unsigned len, nlen, consumed, skip;
			/* Align to the next byte boundary, then give back whatever whole
			 * bytes the accumulator is still holding: dropping them would
			 * lose input, not padding. */
			consumed = (unsigned)(b.pos * 8) - b.n;
			skip = (8 - (consumed & 7)) & 7;
			br_drop(&b, skip);
			b.pos -= b.n / 8;
			b.n = 0; b.acc = 0;
			if (b.pos + 4 > b.len)
				return TINYD_ERR_TRUNCATED;
			len = b.p[b.pos] | ((unsigned)b.p[b.pos + 1] << 8);
			nlen = b.p[b.pos + 2] | ((unsigned)b.p[b.pos + 3] << 8);
			b.pos += 4;
			if ((len ^ 0xffffu) != nlen)
				return TINYD_ERR_FORMAT;
			if (b.pos + len > b.len)
				return TINYD_ERR_TRUNCATED;
			if (pos + len > dict_len + out_max)
				return TINYD_ERR_OVERFLOW;
			memcpy(win + pos, b.p + b.pos, len);
			pos += len;
			b.pos += len;
			continue;
		}
		if (btype != 1)
			return TINYD_ERR_FORMAT;	/* 2 = dynamic, 3 = reserved */

		for (;;) {				/* fixed Huffman block */
			unsigned sym, len, dist, extra;

			/* A code longer than the bits that are left is caught by
			 * the drop inside decode_sym; a code that fits is real,
			 * because the table only offers prefix-free codes. */
			if (b.overrun)
				return TINYD_ERR_TRUNCATED;
			if (decode_sym(&b, lit_sym, lit_bits, 288, &sym))
				return TINYD_ERR_FORMAT;
			if (sym < 256) {
				if (pos >= dict_len + out_max)
					return TINYD_ERR_OVERFLOW;
				win[pos++] = (uint8_t)sym;
				continue;
			}
			if (sym == 256)
				break;			/* end of block */
			sym -= 257;
			if (sym >= 29)
				return TINYD_ERR_FORMAT;
			len = len_base[sym];
			extra = len_extra[sym];
			if (extra) {
				len += br_peek(&b, extra);
				br_drop(&b, extra);
			}
			if (decode_sym(&b, dist_sym, dist_bits, 32, &dist))
				return TINYD_ERR_FORMAT;
			if (dist >= 30)
				return TINYD_ERR_FORMAT;
			{
				unsigned d = dist_base[dist];
				extra = dist_extra[dist];
				if (extra) {
					d += br_peek(&b, extra);
					br_drop(&b, extra);
				}
				if (d > pos)
					return TINYD_ERR_DISTANCE;
				if (pos + len > dict_len + out_max)
					return TINYD_ERR_OVERFLOW;
				/* byte-wise, because a match may overlap itself */
				while (len--) {
					win[pos] = win[pos - d];
					pos++;
				}
			}
		}
	}
	/* The last block can end on a code that ran past the input: with the
	 * accumulator empty, index 0 of the fixed literal table *is* the
	 * end-of-block code, so the inner loop breaks out of a block whose bits
	 * were never there, and "final" reads as 1 from nothing.  That is the
	 * one path that reaches here with overrun set -- 327 of 1405 truncated
	 * prefixes of a valid stream took it, all with output identical to what
	 * zlib decodes from the same prefix. */
	if (b.overrun)
		return TINYD_ERR_TRUNCATED;

	if (out_len)
		*out_len = pos - dict_len;
	return TINYD_OK;
}

