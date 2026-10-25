/*  Sega 315-5838 / 317-0229 / 317-0230 / 317-0231: fixed word substitution followed by programmable prefix decompression.
    Ported to C for ReST-V from MAME src/mame/sega/315-5838_317-0229_comp.cpp (mamedev/mame, last change 272ed7c2
    2026-10-01), license BSD-3-Clause, copyright-holders: David Haywood, Samuel Neves, Peter Wilhelmsen,
    Morten Shearman Kirkegaard. The tables and the algorithm are unchanged (see the MAME file for how they were
    recovered); the device class became static state. */
#include "sega5838.h"

#include <stdlib.h>
#include <string.h>

#define BIT(x, n) (((x) >> (n)) & 1)

typedef struct {
	uint8_t routing[4];
	uint8_t sboxes[4][8];
	uint8_t affine[4];
} Cipher5838;

static const Cipher5838 CIPHERS[4] = {
	{ // 315-5838
		{ 0, 1, 2, 3 },
		{ { 6, 7, 2, 0, 5, 1, 3, 4 }, { 5, 3, 7, 2, 1, 6, 4, 0 },
		  { 2, 0, 6, 1, 3, 7, 4, 5 }, { 6, 5, 7, 4, 1, 3, 0, 2 } },
		{ 0x14, 0x18, 0x02, 0x15 }
	},
	{ // 317-0229
		{ 3, 2, 0, 1 },
		{ { 6, 0, 5, 4, 7, 2, 1, 3 }, { 5, 6, 1, 3, 7, 0, 4, 2 },
		  { 2, 6, 0, 1, 3, 4, 7, 5 }, { 6, 5, 7, 4, 1, 3, 0, 2 } },
		{ 0x12, 0x11, 0x04, 0x1A }
	},
	{ // 317-0230
		{ 2, 3, 1, 0 },
		{ { 6, 2, 7, 0, 5, 3, 1, 4 }, { 5, 3, 7, 2, 1, 6, 4, 0 },
		  { 2, 1, 3, 5, 6, 0, 4, 7 }, { 6, 1, 3, 5, 7, 0, 2, 4 } },
		{ 0x14, 0x18, 0x02, 0x15 }
	},
	{ // 317-0231
		{ 1, 0, 3, 2 },
		{ { 6, 0, 5, 4, 7, 2, 1, 3 }, { 5, 1, 6, 3, 7, 4, 0, 2 },
		  { 2, 1, 3, 5, 6, 0, 4, 7 }, { 6, 3, 1, 5, 7, 2, 0, 4 } },
		{ 0x12, 0x11, 0x04, 0x1A }
	}
};
static const uint8_t OUTPUT_BITS[4][3] = { { 0, 1, 7 }, { 2, 5, 9 }, { 4, 10, 13 }, { 6, 11, 15 } };
static const uint8_t AFFINE_BITS[4] = { 3, 8, 12, 14 };

static uint16_t (*s_read)(uint32_t addr);
static int s_variant;

/* saved state (in this order) */
static struct {
	uint16_t tree[24];
	uint8_t dictionary[256];
	uint32_t tree_words;
	uint32_t dictionary_bytes;
	uint32_t upload_dictionary;
	uint32_t source;
	uint32_t word;
	uint32_t bits;
	uint32_t output;
	uint32_t abort_;
} s;

/* ReST-V (ADR-207): the substitution of every word as a table, and the prefix code as a 12-bit lookahead table built by
   running the same search for every 12-bit value (rebuilt when the tables change); bits are still taken one word at a
   time as before, so the state stays the same. decathlt decompressed 128 KB in 270 ms on the Vita through the search */
static uint16_t *s_dec_word;   // decipher(x) for the variant s_dec_variant
static int s_dec_variant = -1;
static uint16_t s_dec_code[4096]; // the code at the next 12 bits: length (bits 8-11, 0: none) and dictionary index
static int s_dec_code_ok;
static uint16_t decipher_calc(uint16_t x);

void sega5838_init(int variant, uint16_t (*read)(uint32_t word_addr))
{
	s_variant = variant & 3;
	s_read = read;
	memset(&s, 0, sizeof(s));
	s.output = 0xffff;
	s_dec_code_ok = 0;
}

static uint16_t decipher(uint16_t x)
{
	if (s_dec_variant != s_variant) {
		if (!s_dec_word)
			s_dec_word = (uint16_t *)malloc(65536 * sizeof(uint16_t));
		if (!s_dec_word)
			return decipher_calc(x);
		s_dec_variant = s_variant;
		for (uint32_t v = 0; v < 65536; v++)
			s_dec_word[v] = decipher_calc((uint16_t)v);
	}
	return s_dec_word[x];
}

static uint16_t decipher_calc(uint16_t x)
{
	const uint8_t lanes[4] = {
		(uint8_t)((BIT(x, 5) ^ BIT(x, 12)) | ((BIT(x, 7) ^ BIT(x, 9)) << 1) | (BIT(x, 14) << 2)),
		(uint8_t)(BIT(x, 6) | (BIT(x, 8) << 1) | (BIT(x, 15) << 2)),
		(uint8_t)((BIT(x, 1) ^ BIT(x, 8)) | (BIT(x, 7) << 1) | (BIT(x, 12) << 2)),
		(uint8_t)(BIT(x, 3) | (BIT(x, 4) << 1) | (BIT(x, 10) << 2))
	};
	const uint8_t affine = (uint8_t)(BIT(x, 0) | ((BIT(x, 0) ^ BIT(x, 2)) << 1) | ((BIT(x, 11) ^ BIT(x, 14)) << 2) |
		((BIT(x, 4) ^ BIT(x, 13)) << 3) | 16);
	const Cipher5838 *c = &CIPHERS[s_variant];
	uint16_t result = 0;
	for (int group = 0; group < 4; group++) {
		const uint8_t value = c->sboxes[group][lanes[c->routing[group]]];
		for (int bit = 0; bit < 3; bit++)
			result |= (uint16_t)(BIT(value, bit) << OUTPUT_BITS[group][bit]);
		uint8_t parity = affine & c->affine[group];
		parity ^= parity >> 4;
		parity ^= parity >> 2;
		parity ^= parity >> 1;
		result |= (uint16_t)(BIT(parity, 0) << AFFINE_BITS[group]);
	}
	return result;
}

void sega5838_source_w(uint32_t data, uint32_t mem_mask)
{
	s.source = ((s.source & ~mem_mask) | (data & mem_mask)) & 0x007fffff;
	s.word = 0;
	s.bits = 0;
	s.abort_ = 0;
}

void sega5838_table_w(int offset, uint16_t data)
{
	if (!(offset & 1)) {
		s.upload_dictionary = BIT(data, 7);
		if (s.upload_dictionary)
			s.dictionary_bytes = 0;
		else
			s.tree_words = 0;
	} else if (s.upload_dictionary) {
		if (s.dictionary_bytes < sizeof(s.dictionary)) {
			s.dictionary[s.dictionary_bytes++] = (uint8_t)(data >> 8);
			s.dictionary[s.dictionary_bytes++] = (uint8_t)(data & 0xff);
		}
	} else if (s.tree_words < 24) {
		s.tree[s.tree_words++] = data;
	}
	s_dec_code_ok = 0;
}

// the search below for the code at the top of a 12-bit value: (length << 8) | index, 0 if none matches in 12 bits
static uint16_t code_at(uint32_t v)
{
	for (unsigned length = 1; length <= 12; length++) {
		const uint16_t code = (uint16_t)(v >> (12 - length));
		for (unsigned entry = 0; entry < 24; entry += 2) {
			if (((s.tree[entry] >> 8) & 0xff) != length)
				continue;
			const uint16_t first = (uint16_t)(s.tree[entry + 1] >> (12 - length));
			uint16_t limit = (uint16_t)(1u << length);
			for (unsigned next = entry + 2; next < 24; next += 2)
				if (s.tree[next + 1] != s.tree[entry + 1]) {
					limit = (uint16_t)(s.tree[next + 1] >> (12 - length));
					break;
				}
			if (code >= first && code < limit) {
				const unsigned index = (s.tree[entry] & 0xff) + code - first;
				if (index < sizeof(s.dictionary))
					return (uint16_t)(length << 8 | index);
			}
		}
	}
	return 0;
}

static uint8_t decompress_byte(void)
{
	if (s.abort_ || s.tree_words != 24 || s.dictionary_bytes != sizeof(s.dictionary))
		return 0xff;
	if (s_dec_word) { // (ADR-207) the next 12 bits without taking them, then the code's length taken
		if (!s_dec_code_ok) {
			for (uint32_t v = 0; v < 4096; v++)
				s_dec_code[v] = code_at(v);
			s_dec_code_ok = 1;
		}
		uint32_t buf = s.bits ? (s.word & ((1u << s.bits) - 1)) : 0, nb = s.bits, src = s.source;
		while (nb < 12) {
			buf = (buf << 16) | decipher(s_read ? s_read(src) : 0xffff);
			src = (src + 1) & 0x007fffff;
			nb += 16;
		}
		const uint16_t e = s_dec_code[(buf >> (nb - 12)) & 0xfff];
		for (unsigned need = e ? (e >> 8) : 12; need;) {
			if (!s.bits) {
				s.word = decipher(s_read ? s_read(s.source) : 0xffff);
				s.source = (s.source + 1) & 0x007fffff;
				s.bits = 16;
			}
			const unsigned take = need < s.bits ? need : s.bits;
			s.bits -= take;
			need -= take;
		}
		if (e)
			return s.dictionary[e & 0xff];
		s.abort_ = 1;
		return 0xff;
	}
	uint16_t code = 0;
	for (unsigned length = 1; length <= 12; length++) {
		if (!s.bits) {
			s.word = decipher(s_read ? s_read(s.source) : 0xffff);
			s.source = (s.source + 1) & 0x007fffff;
			s.bits = 16;
		}
		code = (uint16_t)((code << 1) | BIT(s.word, --s.bits));
		for (unsigned entry = 0; entry < 24; entry += 2) {
			if (((s.tree[entry] >> 8) & 0xff) != length)
				continue;
			const uint16_t first = (uint16_t)(s.tree[entry + 1] >> (12 - length));
			uint16_t limit = (uint16_t)(1u << length);
			for (unsigned next = entry + 2; next < 24; next += 2)
				if (s.tree[next + 1] != s.tree[entry + 1]) {
					limit = (uint16_t)(s.tree[next + 1] >> (12 - length));
					break;
				}
			if (code >= first && code < limit) {
				const unsigned index = (s.tree[entry] & 0xff) + code - first;
				if (index < sizeof(s.dictionary))
					return s.dictionary[index];
			}
		}
	}
	/* invalid tables or data must not loop forever or read past the dictionary */
	s.abort_ = 1;
	return 0xff;
}

uint16_t sega5838_data_r(void)
{
	s.output = (uint32_t)decompress_byte() << 8;
	s.output |= decompress_byte();
	return (uint16_t)s.output;
}

// ADR-207: n words as n sega5838_data_r calls, the bits in a local buffer; the chip state afterwards (source, word,
// bits, output) is the one the calls leave
void sega5838_read_words(uint16_t *dst, uint32_t n)
{
	if (!n)
		return;
	if (!s_dec_word)
		dst[0] = sega5838_data_r(), dst++, n--; // (the first word sets up the substitution table)
	if (!n || s.abort_ || s.tree_words != 24 || s.dictionary_bytes != sizeof(s.dictionary) || !s_dec_word) {
		for (uint32_t i = 0; i < n; i++)
			dst[i] = sega5838_data_r();
		return;
	}
	if (!s_dec_code_ok) {
		for (uint32_t v = 0; v < 4096; v++)
			s_dec_code[v] = code_at(v);
		s_dec_code_ok = 1;
	}
	uint64_t acc = s.bits ? (s.word & ((1u << s.bits) - 1)) : 0;
	uint32_t nb = s.bits, src = s.source, w_prev = s.word, w_last = s.word, i = 0;
	for (; i < n; i++) {
		uint32_t v = 0;
		for (int b = 0; b < 2; b++) {
			while (nb < 12) {
				w_prev = w_last;
				w_last = s_dec_word[s_read ? s_read(src) : 0xffff];
				src = (src + 1) & 0x007fffff;
				acc = (acc << 16) | w_last;
				nb += 16;
			}
			const uint16_t e = s_dec_code[(acc >> (nb - 12)) & 0xfff];
			if (!e) { // invalid: put the state back to this byte's start and let the plain path end it
				if (nb >= 16)
					s.word = w_prev, s.bits = nb - 16, s.source = (src - 1) & 0x007fffff;
				else
					s.word = w_last, s.bits = nb, s.source = src;
				if (b) { // (the first byte of this word was good)
					s.output = v << 8;
					s.output |= decompress_byte();
					dst[i++] = (uint16_t)s.output;
				}
				for (; i < n; i++)
					dst[i] = sega5838_data_r();
				return;
			}
			nb -= e >> 8;
			v = (v << 8) | s.dictionary[e & 0xff];
		}
		dst[i] = (uint16_t)v;
	}
	// a word fetched ahead and not started is fetched again next time (the plain path fetches when it needs a bit)
	if (nb >= 16)
		s.word = w_prev, s.bits = nb - 16, s.source = (src - 1) & 0x007fffff;
	else
		s.word = w_last, s.bits = nb, s.source = src;
	s.output = dst[n - 1];
}

int sega5838_state(FILE *fp, int save)
{
	s_dec_code_ok = 0;
	return save ? (fwrite(&s, sizeof(s), 1, fp) == 1 ? 0 : -1) : (fread(&s, sizeof(s), 1, fp) == 1 ? 0 : -1);
}
