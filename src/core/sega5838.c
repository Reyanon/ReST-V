/*  Sega 315-5838 / 317-0229 / 317-0230 / 317-0231: fixed word substitution followed by programmable prefix decompression.
    Ported to C for ReST-V from MAME src/mame/sega/315-5838_317-0229_comp.cpp (mamedev/mame, last change 272ed7c2
    2026-10-01), license BSD-3-Clause, copyright-holders: David Haywood, Samuel Neves, Peter Wilhelmsen,
    Morten Shearman Kirkegaard. The tables and the algorithm are unchanged (see the MAME file for how they were
    recovered); the device class became static state. */
#include "sega5838.h"

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

void sega5838_init(int variant, uint16_t (*read)(uint32_t word_addr))
{
	s_variant = variant & 3;
	s_read = read;
	memset(&s, 0, sizeof(s));
	s.output = 0xffff;
}

static uint16_t decipher(uint16_t x)
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
}

static uint8_t decompress_byte(void)
{
	if (s.abort_ || s.tree_words != 24 || s.dictionary_bytes != sizeof(s.dictionary))
		return 0xff;
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

int sega5838_state(FILE *fp, int save)
{
	return save ? (fwrite(&s, sizeof(s), 1, fp) == 1 ? 0 : -1) : (fread(&s, sizeof(s), 1, fp) == 1 ? 0 : -1);
}
