/*  Sega 315-5881 decryption / decompression (ST-V A-bus protection, e.g. twcup98).
    Ported to C for ReST-V from MAME src/mame/sega/315-5881_crypt.cpp (mamedev/mame, last change f3e437b9 2026-09-26),
    license BSD-3-Clause, copyright-holders: Andreas Naive, Olivier Galibert, David Haywood.
    The algorithm and tables are unchanged; the device class became static state. */
#include "sega5881.h"

#include <stdio.h>
#include <string.h>

#define BIT(x, n) (((x) >> (n)) & 1)
#define BUFFER_SIZE 2
#define LINE_SIZE 512
#define FLAG_COMPRESSED 0x20000

typedef struct {
	uint8_t table[64];
	int inputs[6];  /* positions of the inputs bits, -1 means no input except from key */
	int outputs[2]; /* positions of the output bits */
} Sbox5881;

static uint16_t (*s_read)(uint32_t addr);
static uint32_t key;
static uint8_t buffer[BUFFER_SIZE];
static uint8_t line_a[LINE_SIZE], line_b[LINE_SIZE];
static uint8_t *line_buffer = line_a, *line_buffer_prev = line_b;
static uint32_t prot_cur_address;
static uint16_t subkey, dec_hist;
static uint32_t dec_header;
static int enc_ready;
static int buffer_pos, line_buffer_pos, line_buffer_size, buffer_bit, buffer_bit2;
static uint8_t buffer2[2];
static uint16_t buffer2a;
static int block_size, block_pos, block_numlines, done_compression;

static const Sbox5881 fn1_sboxes[4][4] = {
	{   // 1st round
		{
			{
				0,3,2,2,1,3,1,2,3,2,1,2,1,2,3,1,3,2,2,0,2,1,3,0,0,3,2,3,2,1,2,0,
				2,3,1,1,2,2,1,1,1,0,2,3,3,0,2,1,1,1,1,1,3,0,3,2,1,0,1,2,0,3,1,3,
			},
			{3,4,5,7,-1,-1},
			{0,4}
		},

		{
			{
				2,2,2,0,3,3,0,1,2,2,3,2,3,0,2,2,1,1,0,3,3,2,0,2,0,1,0,1,2,3,1,1,
				0,1,3,3,1,3,3,1,2,3,2,0,0,0,2,2,0,3,1,3,0,3,2,2,0,3,0,3,1,1,0,2,
			},
			{0,1,2,5,6,7},
			{1,6}
		},

		{
			{
				0,1,3,0,3,1,1,1,1,2,3,1,3,0,2,3,3,2,0,2,1,1,2,1,1,3,1,0,0,2,0,1,
				1,3,1,0,0,3,2,3,2,0,3,3,0,0,0,0,1,2,3,3,2,0,3,2,1,0,0,0,2,2,3,3,
			},
			{0,2,5,6,7,-1},
			{2,3}
		},

		{
			{
				3,2,1,2,1,2,3,2,0,3,2,2,3,1,3,3,0,2,3,0,3,3,2,1,1,1,2,0,2,2,0,1,
				1,3,3,0,0,3,0,3,0,2,1,3,2,1,0,0,0,1,1,2,0,1,0,0,0,1,3,3,2,0,3,3,
			},
			{1,2,3,4,6,7},
			{5,7}
		},
	},
	{   // 2nd round
		{
			{
				3,3,1,2,0,0,2,2,2,1,2,1,3,1,1,3,3,0,0,3,0,3,3,2,1,1,3,2,3,2,1,3,
				2,3,0,1,3,2,0,1,2,1,3,1,2,2,3,3,3,1,2,2,0,3,1,2,2,1,3,0,3,0,1,3,
			},
			{0,1,3,4,5,7},
			{0,4}
		},

		{
			{
				2,0,1,0,0,3,2,0,3,3,1,2,1,3,0,2,0,2,0,0,0,2,3,1,3,1,1,2,3,0,3,0,
				3,0,2,0,0,2,2,1,0,2,3,3,1,3,1,0,1,3,3,0,0,1,3,1,0,2,0,3,2,1,0,1,
			},
			{0,1,3,4,6,-1},
			{1,5}
		},

		{
			{
				2,2,2,3,1,1,0,1,3,3,1,1,2,2,2,0,0,3,2,3,3,0,2,1,2,2,3,0,1,3,0,0,
				3,2,0,3,2,0,1,0,0,1,2,2,3,3,0,2,2,1,3,1,1,1,1,2,0,3,1,0,0,2,3,2,
			},
			{1,2,5,6,7,6},
			{2,7}
		},

		{
			{
				0,1,3,3,3,1,3,3,1,0,2,0,2,0,0,3,1,2,1,3,1,2,3,2,2,0,1,3,0,3,3,3,
				0,0,0,2,1,1,2,3,2,2,3,1,1,2,0,2,0,2,1,3,1,1,3,3,1,1,3,0,2,3,0,0,
			},
			{2,3,4,5,6,7},
			{3,6}
		},
	},
	{   // 3rd round
		{
			{
				0,0,1,0,1,0,0,3,2,0,0,3,0,1,0,2,0,3,0,0,2,0,3,2,2,1,3,2,2,1,1,2,
				0,0,0,3,0,1,1,0,0,2,1,0,3,1,2,2,2,0,3,1,3,0,1,2,2,1,1,1,0,2,3,1,
			},
			{1,2,3,4,5,7},
			{0,5}
		},

		{
			{
				1,2,1,0,3,1,1,2,0,0,2,3,2,3,1,3,2,0,3,2,2,3,1,1,1,1,0,3,2,0,0,1,
				1,0,0,1,3,1,2,3,0,0,2,3,3,0,1,0,0,2,3,0,1,2,0,1,3,3,3,1,2,0,2,1,
			},
			{0,2,4,5,6,7},
			{1,6}
		},

		{
			{
				0,3,0,2,1,2,0,0,1,1,0,0,3,1,1,0,0,3,0,0,2,3,3,2,3,1,2,0,0,2,3,0,
				// unused?
				255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
			},
			{0,2,4,6,7,-1},
			{2,3}
		},

		{
			{
				0,0,1,0,0,1,0,2,3,3,0,3,3,2,3,0,2,2,2,0,3,2,0,3,1,0,0,3,3,0,0,0,
				2,2,1,0,2,0,3,2,0,0,3,1,3,3,0,0,2,1,1,2,1,0,1,1,0,3,1,2,0,2,0,3,
			},
			{0,1,2,3,6,-1},
			{4,7}
		},
	},
	{   // 4th round
		{
			{
				0,3,3,3,3,3,2,0,0,1,2,0,2,2,2,2,1,1,0,2,2,1,3,2,3,2,0,1,2,3,2,1,
				3,2,2,3,1,0,1,0,0,2,0,1,2,1,2,3,1,2,1,1,2,2,1,0,1,3,2,3,2,0,3,1,
			},
			{0,1,3,4,5,6},
			{0,5}
		},

		{
			{
				0,3,0,0,2,0,3,1,1,1,2,2,2,1,3,1,2,2,1,3,2,2,3,3,0,3,1,0,3,2,0,1,
				3,0,2,0,1,0,2,1,3,3,1,2,2,0,2,3,3,2,3,0,1,1,3,3,0,2,1,3,0,2,2,3,
			},
			{0,1,2,3,5,7},
			{1,7}
		},

		{
			{
				0,1,2,3,3,3,3,1,2,0,2,3,2,1,0,1,2,2,1,2,0,3,2,0,1,1,0,1,3,1,3,1,
				3,1,0,0,1,0,0,0,0,1,2,2,1,1,3,3,1,2,3,3,3,2,3,0,2,2,1,3,3,0,2,0,
			},
			{2,3,4,5,6,7},
			{2,3}
		},

		{
			{
				0,2,1,1,3,2,0,3,1,0,1,0,3,2,1,1,2,2,0,3,1,0,1,2,2,2,3,3,0,0,0,0,
				1,2,1,0,2,1,2,2,2,3,2,3,0,1,3,0,0,1,3,0,0,1,1,0,1,0,0,0,0,2,0,1,
			},
			{0,1,2,4,6,7},
			{4,6}
		},
	},
};


static const Sbox5881 fn2_sboxes[4][4] = {
	{   // 1st round
		{
			{
				3,3,0,1,0,1,0,0,0,3,0,0,1,3,1,2,0,3,3,3,2,1,0,1,1,1,2,2,2,3,2,2,
				2,1,3,3,1,3,1,1,0,0,1,2,0,2,2,1,1,2,3,1,2,1,3,1,2,2,0,1,3,0,2,2,
			},
			{1,3,4,5,6,7},
			{0,7}
		},

		{
			{
				0,1,3,0,1,1,2,3,2,0,0,3,2,1,3,1,3,3,0,0,1,0,0,3,0,3,3,2,3,2,0,1,
				3,2,3,2,2,1,3,1,1,1,0,3,3,2,2,1,1,2,0,2,0,1,1,0,1,0,1,1,2,0,3,0,
			},
			{0,3,5,6,5,0},
			{1,2}
		},

		{
			{
				0,2,2,1,0,1,2,1,2,0,1,2,3,3,0,1,3,1,1,2,1,2,1,3,3,2,3,3,2,1,0,1,
				0,1,0,2,0,1,1,3,2,0,3,2,1,1,1,3,2,3,0,2,3,0,2,2,1,3,0,1,1,2,2,2,
			},
			{0,2,3,4,7,-1},
			{3,4}
		},

		{
			{
				2,3,1,3,2,0,1,2,0,0,3,3,3,3,3,1,2,0,2,1,2,3,0,2,0,1,0,3,0,2,1,0,
				2,3,0,1,3,0,3,2,3,1,2,0,3,1,1,2,0,3,0,0,2,0,2,1,2,2,3,2,1,2,3,1,
			},
			{1,2,5,6,-1,-1},
			{5,6}
		},
	},
	{   // 2nd round
		{
			{
				2,3,1,3,1,0,3,3,3,2,3,3,2,0,0,3,2,3,0,3,1,1,2,3,1,1,2,2,0,1,0,0,
				2,1,0,1,2,0,1,2,0,3,1,1,2,3,1,2,0,2,0,1,3,0,1,0,2,2,3,0,3,2,3,0,
			},
			{0,1,4,5,6,7},
			{0,7}
		},

		{
			{
				0,2,2,0,2,2,0,3,2,3,2,1,3,2,3,3,1,1,0,0,3,0,2,1,1,3,3,2,3,2,0,1,
				1,2,3,0,1,0,3,0,3,1,0,2,1,2,0,3,2,3,1,2,2,0,3,2,3,0,0,1,2,3,3,3,
			},
			{0,2,3,6,7,-1},
			{1,5}
		},

		{
			{
				1,0,3,0,0,1,2,1,0,0,1,0,0,0,2,3,2,2,0,2,0,1,3,0,2,0,1,3,2,3,0,1,
				1,2,2,2,1,3,0,3,0,1,1,0,3,2,3,3,2,0,0,3,1,2,1,3,3,2,1,0,2,1,2,3,
			},
			{2,3,4,6,7,2},
			{2,3}
		},

		{
			{
				2,3,1,3,1,1,2,3,3,1,1,0,1,0,2,3,2,1,0,0,2,2,0,1,0,2,2,2,0,2,1,0,
				3,1,2,3,1,3,0,2,1,0,1,0,0,1,2,2,3,2,3,1,3,2,1,1,2,0,2,1,3,3,1,0,
			},
			{1,2,3,4,5,6},
			{4,6}
		},
	},
	{   // 3rd round
		{
			{
				0,3,0,1,3,0,0,2,1,0,1,3,2,2,2,0,3,3,3,0,2,2,0,3,0,0,2,3,0,3,2,1,
				3,3,0,3,0,2,3,3,1,1,1,0,2,2,1,1,3,0,3,1,2,0,2,0,0,0,3,2,1,1,0,0,
			},
			{1,4,5,6,7,5},
			{0,5}
		},

		{
			{
				0,3,0,1,3,0,3,1,3,2,2,2,3,0,3,2,2,1,2,2,0,3,2,2,0,0,2,1,1,3,2,3,
				2,3,3,1,2,0,1,2,2,1,0,0,0,0,2,3,1,2,0,3,1,3,1,2,3,2,1,0,3,0,0,2,
			},
			{0,2,3,4,6,7},
			{1,7}
		},

		{
			{
				2,2,0,3,0,3,1,0,1,1,2,3,2,3,1,0,0,0,3,2,2,0,2,3,1,3,2,0,3,3,1,3,
				// unused?
				255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
			},
			{1,2,4,7,2,-1},
			{2,4}
		},

		{
			{
				0,2,3,1,3,1,1,0,0,1,3,0,2,1,3,3,2,0,2,1,1,2,3,3,0,0,0,2,0,2,3,0,
				3,3,3,3,2,3,3,2,3,0,1,0,2,3,3,2,0,1,3,1,0,1,2,3,3,0,2,0,3,0,3,3,
			},
			{0,1,2,3,5,7},
			{3,6}
		},
	},
	{   // 4th round
		{
			{
				0,1,1,0,0,1,0,2,3,3,0,1,2,3,0,2,1,0,3,3,2,0,3,0,0,2,1,0,1,0,1,3,
				0,3,3,1,2,0,3,0,1,3,2,0,3,3,1,3,0,2,3,3,2,1,1,2,2,1,2,1,2,0,1,1,
			},
			{0,1,2,4,7,-1},
			{0,5}
		},

		{
			{
				2,0,0,2,3,0,2,3,3,1,1,1,2,1,1,0,0,2,1,0,0,3,1,0,0,3,3,0,1,0,1,2,
				0,2,0,2,0,1,2,3,2,1,1,0,3,3,3,3,3,3,1,0,3,0,0,2,0,3,2,0,2,2,0,1,
			},
			{0,1,3,5,6,-1},
			{1,3}
		},

		{
			{
				0,1,1,2,1,3,1,1,0,0,3,1,1,1,2,0,3,2,0,1,1,2,3,3,3,0,3,0,0,2,0,3,
				3,2,0,0,3,2,3,1,2,3,0,3,2,0,1,2,2,2,0,2,0,1,2,2,3,1,2,2,1,1,1,1,
			},
			{0,2,3,4,5,7},
			{2,7}
		},

		{
			{
				0,1,2,0,3,3,0,3,2,1,3,3,0,3,1,1,3,2,3,2,3,0,0,0,3,0,2,2,3,2,2,3,
				2,2,3,1,2,3,1,2,0,3,0,2,3,1,0,0,3,2,1,2,1,2,1,3,1,0,2,3,3,1,3,2,
			},
			{2,3,4,5,6,7},
			{4,6}
		},
	},
};

static const int fn1_game_key_scheduling[38][2] = {
	{1,29},  {1,71},  {2,4},   {2,54},  {3,8},   {4,56},  {4,73},  {5,11},
	{6,51},  {7,92},  {8,89},  {9,9},   {9,39},  {9,58},  {10,90}, {11,6},
	{12,64}, {13,49}, {14,44}, {15,40}, {16,69}, {17,15}, {18,23}, {18,43},
	{19,82}, {20,81}, {21,32}, {22,5},  {23,66}, {24,13}, {24,45}, {25,12},
	{25,35}, {26,61}, {27,10}, {27,59}, {28,25}, {29,86}
};

static const int fn2_game_key_scheduling[32][2] = {
	{0,0},   {1,3},   {2,11},  {3,20},  {4,22},  {5,23},  {6,29},  {7,38},
	{8,39},  {9,55},  {9,86},  {9,87},  {10,50}, {11,57}, {12,59}, {13,61},
	{14,63}, {15,67}, {16,72}, {17,83}, {18,88}, {19,94}, {20,35}, {21,17},
	{22,6},  {23,85}, {24,16}, {25,25}, {26,92}, {27,47}, {28,28}, {29,90}
};

static const int fn1_sequence_key_scheduling[20][2] = {
	{0,52},  {1,34},  {2,17},  {3,36}, {4,84},  {4,88},  {5,57},  {6,48},
	{6,68},  {7,76},  {8,83},  {9,30}, {10,22}, {10,41}, {11,38}, {12,55},
	{13,74}, {14,19}, {14,80}, {15,26}
};

static const int fn2_sequence_key_scheduling[16] = {77,34,8,42,36,27,69,66,13,9,79,31,49,7,24,64};

static const int fn2_middle_result_scheduling[16] = {1,10,44,68,74,78,81,95,2,4,30,40,41,51,53,58};

static const uint8_t trees[9][2][32] = {
	{
		{0x01,0x10,0x0f,0x05,0xc4,0x13,0x87,0x0a,0xcc,0x81,0xce,0x0c,0x86,0x0e,0x84,0xc2,
			0x11,0xc1,0xc3,0xcf,0x15,0xc8,0xcd,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
		{0xc7,0x02,0x03,0x04,0x80,0x06,0x07,0x08,0x09,0xc9,0x0b,0x0d,0x82,0x83,0x85,0xc0,
			0x12,0xc6,0xc5,0x14,0x16,0xca,0xcb,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
	},
	{
		{0x02,0x80,0x05,0x04,0x81,0x10,0x15,0x82,0x09,0x83,0x0b,0x0c,0x0d,0xdc,0x0f,0xde,
			0x1c,0xcf,0xc5,0xdd,0x86,0x16,0x87,0x18,0x19,0x1a,0xda,0xca,0xc9,0x1e,0xce,0xff,},
		{0x01,0x17,0x03,0x0a,0x08,0x06,0x07,0xc2,0xd9,0xc4,0xd8,0xc8,0x0e,0x84,0xcb,0x85,
			0x11,0x12,0x13,0x14,0xcd,0x1b,0xdb,0xc7,0xc0,0xc1,0x1d,0xdf,0xc3,0xc6,0xcc,0xff,},
	},
	{
		{0xc6,0x80,0x03,0x0b,0x05,0x07,0x82,0x08,0x15,0xdc,0xdd,0x0c,0xd9,0xc2,0x14,0x10,
			0x85,0x86,0x18,0x16,0xc5,0xc4,0xc8,0xc9,0xc0,0xcc,0xff,0xff,0xff,0xff,0xff,0xff,},
		{0x01,0x02,0x12,0x04,0x81,0x06,0x83,0xc3,0x09,0x0a,0x84,0x11,0x0d,0x0e,0x0f,0x19,
			0xca,0xc1,0x13,0xd8,0xda,0xdb,0x17,0xde,0xcd,0xcb,0xff,0xff,0xff,0xff,0xff,0xff,},
	},
	{
		{0x01,0x80,0x0d,0x04,0x05,0x15,0x83,0x08,0xd9,0x10,0x0b,0x0c,0x84,0x0e,0xc0,0x14,
			0x12,0xcb,0x13,0xca,0xc8,0xc2,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
		{0xc5,0x02,0x03,0x07,0x81,0x06,0x82,0xcc,0x09,0x0a,0xc9,0x11,0xc4,0x0f,0x85,0xd8,
			0xda,0xdb,0xc3,0xdc,0xdd,0xc1,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
	},
	{
		{0x01,0x80,0x06,0x0c,0x05,0x81,0xd8,0x84,0x09,0xdc,0x0b,0x0f,0x0d,0x0e,0x10,0xdb,
			0x11,0xca,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
		{0xc4,0x02,0x03,0x04,0xcb,0x0a,0x07,0x08,0xd9,0x82,0xc8,0x83,0xc0,0xc1,0xda,0xc2,
			0xc9,0xc3,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
	},
	{
		{0x01,0x02,0x06,0x0a,0x83,0x0b,0x07,0x08,0x09,0x82,0xd8,0x0c,0xd9,0xda,0xff,0xff,
			0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
		{0xc3,0x80,0x03,0x04,0x05,0x81,0xca,0xc8,0xdb,0xc9,0xc0,0xc1,0x0d,0xc2,0xff,0xff,
			0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
	},
	{
		{0x01,0x02,0x03,0x04,0x81,0x07,0x08,0xd8,0xda,0xd9,0xff,0xff,0xff,0xff,0xff,0xff,
			0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
		{0xc2,0x80,0x05,0xc9,0xc8,0x06,0x82,0xc0,0x09,0xc1,0xff,0xff,0xff,0xff,0xff,0xff,
			0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
	},
	{
		{0x01,0x80,0x04,0xc8,0xc0,0xd9,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
			0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
		{0xc1,0x02,0x03,0x81,0x05,0xd8,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
			0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
	},
	{
		{0x01,0xd8,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
			0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
		{0xc0,0x80,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
			0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
	},
};


static int bitswap16(int v, int b15, int b14, int b13, int b12, int b11, int b10, int b9, int b8, int b7, int b6, int b5, int b4,
	int b3, int b2, int b1, int b0)
{
	return (BIT(v, b15) << 15) | (BIT(v, b14) << 14) | (BIT(v, b13) << 13) | (BIT(v, b12) << 12) | (BIT(v, b11) << 11) |
		(BIT(v, b10) << 10) | (BIT(v, b9) << 9) | (BIT(v, b8) << 8) | (BIT(v, b7) << 7) | (BIT(v, b6) << 6) | (BIT(v, b5) << 5) |
		(BIT(v, b4) << 4) | (BIT(v, b3) << 3) | (BIT(v, b2) << 2) | (BIT(v, b1) << 1) | BIT(v, b0);
}

static int feistel_function(int input, const Sbox5881 *sboxes, uint32_t subkeys)
{
	int k, m, aux, result = 0;
	for (m = 0; m < 4; ++m) {
		for (k = 0, aux = 0; k < 6; ++k)
			if (sboxes[m].inputs[k] != -1)
				aux |= BIT(input, sboxes[m].inputs[k]) << k;
		aux = sboxes[m].table[(aux ^ subkeys) & 0x3f];
		for (k = 0; k < 2; ++k)
			result |= BIT(aux, k) << sboxes[m].outputs[k];
		subkeys >>= 6;
	}
	return result;
}

__attribute__((unused)) static uint16_t block_decrypt(uint32_t game_key, uint16_t sequence_key, uint16_t counter, uint16_t data)
{
	int j, aux, aux2, A, B, middle_result;
	uint32_t fn1_subkeys[4] = { 0, 0, 0, 0 }, fn2_subkeys[4] = { 0, 0, 0, 0 };
	for (j = 0; j < 38; ++j)
		if (BIT(game_key, fn1_game_key_scheduling[j][0]) != 0) {
			aux = fn1_game_key_scheduling[j][1] % 24;
			aux2 = fn1_game_key_scheduling[j][1] / 24;
			fn1_subkeys[aux2] ^= (1u << aux);
		}
	for (j = 0; j < 32; ++j)
		if (BIT(game_key, fn2_game_key_scheduling[j][0]) != 0) {
			aux = fn2_game_key_scheduling[j][1] % 24;
			aux2 = fn2_game_key_scheduling[j][1] / 24;
			fn2_subkeys[aux2] ^= (1u << aux);
		}
	for (j = 0; j < 20; ++j)
		if (BIT(sequence_key, fn1_sequence_key_scheduling[j][0]) != 0) {
			aux = fn1_sequence_key_scheduling[j][1] % 24;
			aux2 = fn1_sequence_key_scheduling[j][1] / 24;
			fn1_subkeys[aux2] ^= (1u << aux);
		}
	for (j = 0; j < 16; ++j)
		if (BIT(sequence_key, j) != 0) {
			aux = fn2_sequence_key_scheduling[j] % 24;
			aux2 = fn2_sequence_key_scheduling[j] / 24;
			fn2_subkeys[aux2] ^= (1u << aux);
		}
	aux = bitswap16(counter, 5, 12, 14, 13, 9, 3, 6, 4, 8, 1, 15, 11, 0, 7, 10, 2);
	B = aux >> 8;
	A = (aux & 0xff) ^ feistel_function(B, fn1_sboxes[0], fn1_subkeys[0]);
	B ^= feistel_function(A, fn1_sboxes[1], fn1_subkeys[1]);
	A ^= feistel_function(B, fn1_sboxes[2], fn1_subkeys[2]);
	B ^= feistel_function(A, fn1_sboxes[3], fn1_subkeys[3]);
	middle_result = (B << 8) | A;
	for (j = 0; j < 16; ++j)
		if (BIT(middle_result, j) != 0) {
			aux = fn2_middle_result_scheduling[j] % 24;
			aux2 = fn2_middle_result_scheduling[j] / 24;
			fn2_subkeys[aux2] ^= (1u << aux);
		}
	aux = bitswap16(data, 14, 3, 8, 12, 13, 7, 15, 4, 6, 2, 9, 5, 11, 0, 1, 10);
	B = aux >> 8;
	A = (aux & 0xff) ^ feistel_function(B, fn2_sboxes[0], fn2_subkeys[0]);
	B ^= feistel_function(A, fn2_sboxes[1], fn2_subkeys[1]);
	A ^= feistel_function(B, fn2_sboxes[2], fn2_subkeys[2]);
	B ^= feistel_function(A, fn2_sboxes[3], fn2_subkeys[3]);
	aux = (B << 8) | A;
	aux = bitswap16(aux, 15, 7, 6, 14, 13, 12, 5, 4, 3, 2, 11, 10, 9, 1, 0, 8);
	return (uint16_t)aux;
}

/* ReST-V (ADR-100): the same block_decrypt with its key schedules and bit gathers in tables. The original rebuilt the
   whole subkey schedule and gathered every bit one by one for each word (scene loads of twcup98: 50ms frames).
   Per (game key, sequence key): the fn1 rounds as 4 x 256 byte tables and the fn2 base subkeys; per word: the fn1 lookups,
   the middle result's fn2 subkey bits (byte tables) and the fn2 rounds through per-sbox gather / scatter tables. */
static uint16_t perm_lo[3][256], perm_hi[3][256]; /* 0: counter, 1: data in, 2: data out */
static uint8_t f2_gather[4][4][256];              /* fn2 round, sbox, input byte -> 6 bits */
static uint8_t f2_scatter[4][4][4];               /* fn2 round, sbox, sbox output -> result bits */
static uint32_t mr_lo[256][4], mr_hi[256][4];      /* middle result byte -> fn2 subkey bits */
static uint8_t f1_tab[4][256];                     /* fn1 round tables for the current keys */
static uint32_t f2_base[4];                        /* fn2 subkeys from the game and sequence keys */
static int fast_init_done, fast_keys_valid;
static uint32_t fast_key;
static uint16_t fast_subkey;

static void fast_init(void)
{
	static const int pc[16] = { 5, 12, 14, 13, 9, 3, 6, 4, 8, 1, 15, 11, 0, 7, 10, 2 };
	static const int pi[16] = { 14, 3, 8, 12, 13, 7, 15, 4, 6, 2, 9, 5, 11, 0, 1, 10 };
	static const int po[16] = { 15, 7, 6, 14, 13, 12, 5, 4, 3, 2, 11, 10, 9, 1, 0, 8 };
	const int *pm[3] = { pc, pi, po };
	for (int t = 0; t < 3; t++)
		for (int v = 0; v < 256; v++) {
			uint16_t lo = 0, hi = 0;
			for (int k = 0; k < 16; k++) { /* result bit 15 - k = input bit pm[t][k] */
				const int src = pm[t][k];
				if (src < 8) lo |= (uint16_t)(BIT(v, src) << (15 - k));
				else hi |= (uint16_t)(BIT(v, src - 8) << (15 - k));
			}
			perm_lo[t][v] = lo;
			perm_hi[t][v] = hi;
		}
	for (int r = 0; r < 4; r++)
		for (int m = 0; m < 4; m++) {
			const Sbox5881 *sb = &fn2_sboxes[r][m];
			for (int v = 0; v < 256; v++) {
				int aux = 0;
				for (int k = 0; k < 6; k++)
					if (sb->inputs[k] != -1)
						aux |= BIT(v, sb->inputs[k]) << k;
				f2_gather[r][m][v] = (uint8_t)aux;
			}
			for (int o = 0; o < 4; o++) {
				int res = 0;
				for (int k = 0; k < 2; k++)
					res |= BIT(o, k) << sb->outputs[k];
				f2_scatter[r][m][o] = (uint8_t)res;
			}
		}
	for (int v = 0; v < 256; v++)
		for (int h = 0; h < 2; h++) {
			uint32_t *d = h ? mr_hi[v] : mr_lo[v];
			d[0] = d[1] = d[2] = d[3] = 0;
			for (int j = 0; j < 8; j++)
				if (BIT(v, j)) {
					const int sch = fn2_middle_result_scheduling[j + 8 * h];
					d[sch / 24] ^= 1u << (sch % 24);
				}
		}
	fast_init_done = 1;
}

static void fast_keys(uint32_t game_key, uint16_t sequence_key)
{
	uint32_t fn1_subkeys[4] = { 0, 0, 0, 0 };
	int j;
	for (j = 0; j < 4; j++)
		f2_base[j] = 0;
	for (j = 0; j < 38; ++j)
		if (BIT(game_key, fn1_game_key_scheduling[j][0]) != 0)
			fn1_subkeys[fn1_game_key_scheduling[j][1] / 24] ^= 1u << (fn1_game_key_scheduling[j][1] % 24);
	for (j = 0; j < 32; ++j)
		if (BIT(game_key, fn2_game_key_scheduling[j][0]) != 0)
			f2_base[fn2_game_key_scheduling[j][1] / 24] ^= 1u << (fn2_game_key_scheduling[j][1] % 24);
	for (j = 0; j < 20; ++j)
		if (BIT(sequence_key, fn1_sequence_key_scheduling[j][0]) != 0)
			fn1_subkeys[fn1_sequence_key_scheduling[j][1] / 24] ^= 1u << (fn1_sequence_key_scheduling[j][1] % 24);
	for (j = 0; j < 16; ++j)
		if (BIT(sequence_key, j) != 0)
			f2_base[fn2_sequence_key_scheduling[j] / 24] ^= 1u << (fn2_sequence_key_scheduling[j] % 24);
	for (int r = 0; r < 4; r++)
		for (int v = 0; v < 256; v++)
			f1_tab[r][v] = (uint8_t)feistel_function(v, fn1_sboxes[r], fn1_subkeys[r]);
	fast_key = game_key;
	fast_subkey = sequence_key;
	fast_keys_valid = 1;
}

static inline int f2_round(int r, int input, uint32_t sk)
{
	int result = 0;
	for (int m = 0; m < 4; m++) {
		result |= f2_scatter[r][m][fn2_sboxes[r][m].table[(f2_gather[r][m][input] ^ sk) & 0x3f]];
		sk >>= 6;
	}
	return result;
}

static uint16_t block_decrypt_fast(uint32_t game_key, uint16_t sequence_key, uint16_t counter, uint16_t data)
{
	if (!fast_init_done)
		fast_init();
	if (!fast_keys_valid || fast_key != game_key || fast_subkey != sequence_key)
		fast_keys(game_key, sequence_key);
	int aux = perm_lo[0][counter & 0xff] | perm_hi[0][counter >> 8];
	int B = aux >> 8;
	int A = (aux & 0xff) ^ f1_tab[0][B];
	B ^= f1_tab[1][A];
	A ^= f1_tab[2][B];
	B ^= f1_tab[3][A];
	const uint32_t *ml = mr_lo[A], *mh = mr_hi[B]; /* middle_result = (B << 8) | A */
	const uint32_t k0 = f2_base[0] ^ ml[0] ^ mh[0], k1 = f2_base[1] ^ ml[1] ^ mh[1];
	const uint32_t k2 = f2_base[2] ^ ml[2] ^ mh[2], k3 = f2_base[3] ^ ml[3] ^ mh[3];
	aux = perm_lo[1][data & 0xff] | perm_hi[1][data >> 8];
	B = aux >> 8;
	A = (aux & 0xff) ^ f2_round(0, B, k0);
	B ^= f2_round(1, A, k1);
	A ^= f2_round(2, B, k2);
	B ^= f2_round(3, A, k3);
	aux = (B << 8) | A;
	return (uint16_t)(perm_lo[2][aux & 0xff] | perm_hi[2][aux >> 8]);
}

static uint16_t get_decrypted_16(void)
{
	const uint16_t enc = s_read(prot_cur_address);
	const uint16_t dec = block_decrypt_fast(key, subkey, (uint16_t)prot_cur_address, enc);
#ifdef RSTV_5881_CHECK
	if (dec != block_decrypt(key, subkey, (uint16_t)prot_cur_address, enc)) {
		static int c;
		if (c++ < 10) printf("5881 fast mismatch addr %08x enc %04x\n", prot_cur_address, enc);
	}
#endif
	const uint16_t res = (dec & 3) | (dec_hist & 0xfffc);
	dec_hist = dec;
	prot_cur_address++;
	return res;
}

static void enc_start(void)
{
	block_pos = 0;
	done_compression = 0;
	buffer_pos = BUFFER_SIZE;
	if (buffer_bit2 < 14) {
		/* remaining bits in the decompression buffer (twcup98 needs this) */
		dec_header = (uint32_t)(buffer2a & 0x0003) << 16;
	} else {
		dec_hist = 0;
		dec_header = (uint32_t)get_decrypted_16() << 16;
	}
	dec_header |= get_decrypted_16();
	block_numlines = ((dec_header & 0x000000ff) >> 0) + 1;
	const int blocky = ((dec_header & 0x0001ff00) >> 8) + 1;
	block_size = block_numlines * blocky;
	if (dec_header & FLAG_COMPRESSED) {
		line_buffer_size = blocky;
		line_buffer_pos = line_buffer_size;
		buffer_bit = 7;
		buffer_bit2 = 15;
	}
	enc_ready = 1;
}

static void enc_fill(void)
{
	for (int i = 0; i != BUFFER_SIZE; i += 2) {
		const uint16_t val = get_decrypted_16();
		buffer[i] = (uint8_t)val;
		buffer[i + 1] = (uint8_t)(val >> 8);
		block_pos += 2;
		if (!(dec_header & FLAG_COMPRESSED) && block_pos == block_size)
			enc_start();
	}
	buffer_pos = 0;
}

static int get_compressed_bit(void)
{
	if (buffer_bit2 == 15) {
		buffer_bit2 = 0;
		buffer2a = get_decrypted_16();
		buffer2[0] = (uint8_t)buffer2a;
		buffer2[1] = (uint8_t)(buffer2a >> 8);
		buffer_pos = 0;
	} else {
		buffer_bit2++;
	}
	const int res = (buffer2[(buffer_pos & 1) ^ 1] >> buffer_bit) & 1;
	buffer_bit--;
	if (buffer_bit == -1) {
		buffer_bit = 7;
		buffer_pos++;
	}
	return res;
}

static void line_fill(void)
{
	uint8_t *lp = line_buffer, *lc = line_buffer_prev;
	line_buffer = lc; /* swap: the line being built becomes the current one */
	line_buffer_prev = lp;
	line_buffer_pos = 0;
	for (int i = 0; i != line_buffer_size;) {
		const int slot = i ? i < line_buffer_size - 7 ? 1 : (i & 7) + 1 : 0;
		uint32_t tmp = 0;
		while (!(tmp & 0x80))
			tmp = get_compressed_bit() ? trees[slot][1][tmp] : trees[slot][0][tmp];
		if (tmp != 0xff) {
			const int count = (tmp & 7) + 1;
			if (tmp & 0x40) {
				static const int offsets[4] = { 0, 1, 0, -1 };
				const int offset = offsets[(tmp & 0x18) >> 3];
				for (int j = 0; j != count; j++) {
					lc[i ^ 1] = lp[((i + offset) % line_buffer_size) ^ 1];
					i++;
				}
			} else {
				uint8_t byte = 0;
				for (int b = 0; b < 8; b++)
					byte = (uint8_t)((byte << 1) | get_compressed_bit());
				for (int j = 0; j != count; j++)
					lc[(i++) ^ 1] = byte;
			}
		}
	}
	block_pos++;
	if (block_numlines == block_pos)
		done_compression = 1;
}

/* ---- interface ---- */

void sega5881_init(uint32_t game_key, uint16_t (*read)(uint32_t addr))
{
	key = game_key;
	s_read = read;
	memset(buffer, 0, sizeof(buffer));
	memset(line_a, 0, sizeof(line_a));
	memset(line_b, 0, sizeof(line_b));
	line_buffer = line_a;
	line_buffer_prev = line_b;
	prot_cur_address = 0;
	subkey = 0;
	dec_hist = 0;
	dec_header = 0;
	enc_ready = 0;
	buffer_pos = 0;
	line_buffer_pos = 0;
	line_buffer_size = 0;
	buffer_bit = 0;
	buffer_bit2 = 0;
}

uint16_t sega5881_do_decrypt(void)
{
	const uint8_t *base;
	if (!enc_ready)
		enc_start();
	if (dec_header & FLAG_COMPRESSED) {
		if (line_buffer_pos == line_buffer_size) {
			if (done_compression == 1)
				enc_start();
			line_fill();
		}
		base = line_buffer + line_buffer_pos;
		line_buffer_pos += 2;
	} else {
		if (buffer_pos == BUFFER_SIZE)
			enc_fill();
		base = buffer + buffer_pos;
		buffer_pos += 2;
	}
	return (uint16_t)((base[0] << 8) | base[1]);
}

void sega5881_set_addr_low(uint16_t data)
{
	prot_cur_address = (prot_cur_address & 0xffff0000) | data;
	enc_ready = 0;
}

void sega5881_set_addr_high(uint16_t data)
{
	prot_cur_address = (prot_cur_address & 0x0000ffff) | ((uint32_t)data << 16);
	enc_ready = 0;
	buffer_bit = 7;
	buffer_bit2 = 15;
}

void sega5881_set_subkey(uint16_t data)
{
	subkey = data;
	enc_ready = 0;
}

#define ST_(x) do { if (save) fwrite(&(x), sizeof(x), 1, fp); else if (fread(&(x), sizeof(x), 1, fp) != 1) return -1; } while (0)
int sega5881_state(FILE *fp, int save)
{
	int which = line_buffer == line_b;
	ST_(key); ST_(buffer); ST_(line_a); ST_(line_b); ST_(which);
	line_buffer = which ? line_b : line_a;
	line_buffer_prev = which ? line_a : line_b;
	ST_(prot_cur_address); ST_(subkey); ST_(dec_hist); ST_(dec_header); ST_(enc_ready);
	ST_(buffer_pos); ST_(line_buffer_pos); ST_(line_buffer_size); ST_(buffer_bit); ST_(buffer_bit2);
	ST_(buffer2); ST_(buffer2a);
	ST_(block_size); ST_(block_pos); ST_(block_numlines); ST_(done_compression);
	return 0;
}
#undef ST_
