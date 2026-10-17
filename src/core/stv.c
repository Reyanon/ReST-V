/*  ST-V ROM set loader for ReST-V.
    Set layouts follow MAME src/mame/sega/stv.cpp; the blob model (HEADER/BYTE/WORD) and the
    T1/T2 storage conventions follow Kronos ctrl/src/stv.c (GPL-2.0-or-later).

      HEADER_BLOB = ROM_LOAD16_BYTE      -> byte j goes to offset + 2*j
      BYTE_BLOB   = ROM_LOAD / RELOAD    -> plain bytes (T1)
      WORD_BLOB   = ROM_LOAD16_WORD_SWAP -> T2 byte writes (word swap)

    This file is part of ReST-V, based on Yabause/Kronos. GPL-2.0-or-later.
*/
#include "stv.h"

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "cs0.h"
#include "eeprom.h"
#include "junzip.h"
#include "memory.h"
#include "sh2core.h"
#include "sh2int.h"
#include "zlib.h"
#include "sega5881.h"
#include "stvio.h"

typedef enum { BLOB_END, HEADER_BLOB, BYTE_BLOB, WORD_BLOB } BlobType;

typedef struct {
	BlobType type;
	const char *file;
	u32 offset, length, crc;
} Blob;

typedef struct {
	StvGameInfo info;
	void (*init)(void);
	Blob blobs[24];
	u32 key5881; // 315-5881 key (MAME ROM_PARAMETER ":315_5881:key"); 0 = no A-bus protection chip
	const char *nv; // default EEPROM in the game's zip (MAME "eeprom" region: settings the BIOS checks, e.g. players); "-" = blank
	u32 nv_crc;
	int io; // STV_IO_* board input variant
	u32 hint_m[4], hint_s[4]; // ADR-106: MAME pcflush addresses; turned into idle-loop heads when the loop is seen
} StvGame;

static void sanjeon_decrypt(void);

RstvHleHook rstv_hle[RSTV_MAX_HLE];
int rstv_hle_n;
// thuntk C library (scene loading: LZ decoder 0601EFE0 calls memcpy / memmove byte loops, overlay start clears its BSS)
// (library routines, the LZ decoder and the overlay BSS loop are found by their code in any game, ADR-045)
// thuntk perspective vertex routine (puzzle mini games, ADR-055)
static const RstvHleHook thuntk_hle[] = { { 0x0602FEF8, RSTV_HLE_TKVTX }, { 0, 0 } };
// sasissu / sanjeon SGL vertex transform loop (ADR-047): verified exact but no gain (calls are mostly one pass before the
// clip path, the dispatcher round trip costs what the loop saved). Kept off: { 0x0606ECB6, RSTV_HLE_SGLVTX }
static const RstvHleHook sasissu_hle[] = { { 0x0606ECB6, RSTV_HLE_SGLVTX }, { 0x060726FA, RSTV_HLE_SGLVTX },
	{ 0x0600B33A, RSTV_HLE_SJINTERP }, { 0x0607284E, RSTV_HLE_SGLPOLY }, { 0x0606DC9A, RSTV_HLE_SGLSPR },
	{ 0x0606AEA8, RSTV_HLE_SGLONSCR },
	{ 0, 0 } }; // ADR-056 interpolation, ADR-060 polygons, ADR-069 sprite, ADR-074 on-screen test
// ADR-100: twcup98 runs the same SGL vertex loop on both CPUs (master 0605CD86, slave 060631DE: 80% of the slave's time)
static const RstvHleHook twcup98_hle[] = { { 0x0605CD86, RSTV_HLE_SGLVTX }, { 0x060631DE, RSTV_HLE_SGLVTX },
	{ 0x06063332, RSTV_HLE_SGLPOLY }, { 0x0605C04E, RSTV_HLE_SGLPOLY }, { 0, 0 } };
	// and the SGL polygon loop (slave 06063332 = sanjeon 0607284E; master 0605C04E another build, ADR-101)
static void twcup98_init(void);

static const StvGame s_games[] = {
	{ { "sandor", NULL, "Puzzle & Action: Sando-R (J 951114 V1.000)", { 0x602a0f8, 0x6013fb8, 0x602ac38, 0x6011332 }, 0x602abcc, 0, NULL, 0, 256 }, NULL, {
		{ HEADER_BLOB, "sando-r.13",  0x0000001, 0x0100000, 0xfe63a239 },
		{ BYTE_BLOB,   "sando-r.13",  0x0200000, 0x0100000, 0xfe63a239 },
		{ BYTE_BLOB,   "sando-r.13",  0x0300000, 0x0100000, 0xfe63a239 },
		{ WORD_BLOB,   "mpr18635.8",  0x1c00000, 0x0400000, 0x441e1368 },
		{ WORD_BLOB,   "mpr18635.8",  0x0400000, 0x0400000, 0x441e1368 },
		{ WORD_BLOB,   "mpr18636.9",  0x2000000, 0x0400000, 0xfff1dd80 },
		{ WORD_BLOB,   "mpr18636.9",  0x0800000, 0x0400000, 0xfff1dd80 },
		{ WORD_BLOB,   "mpr18637.10", 0x2400000, 0x0400000, 0x83aced0f },
		{ WORD_BLOB,   "mpr18637.10", 0x0c00000, 0x0400000, 0x83aced0f },
		{ WORD_BLOB,   "mpr18638.11", 0x2800000, 0x0400000, 0xcaab531b },
		{ WORD_BLOB,   "mpr18638.11", 0x1000000, 0x0400000, 0xcaab531b },
		{ BLOB_END } } },
	{ { "thunt", "sandor", "Puzzle & Action: Treasure Hunt (JUET 970901 V2.00E)", { 0x602A024, 0x6013EE4, 0x602AB64, 0x6011332 }, 0x602AAF8, 0, NULL, 0, 256 }, NULL, {
		{ HEADER_BLOB, "th-ic7_2.stv", 0x0200000, 0x0080000, 0xc4e993de },
		{ HEADER_BLOB, "th-ic7_1.stv", 0x0200001, 0x0080000, 0x1355cc18 },
		{ WORD_BLOB,   "th-e-2.ic2",   0x0400000, 0x0400000, 0x47315694 },
		{ WORD_BLOB,   "th-e-3.ic3",   0x0800000, 0x0400000, 0xc9290b44 },
		{ WORD_BLOB,   "th-e-4.ic4",   0x0c00000, 0x0400000, 0xc672e40b },
		{ WORD_BLOB,   "th-e-5.ic5",   0x1000000, 0x0400000, 0x3914b805 },
		{ BLOB_END } } },
	{ { "thuntk", "sandor", "Puzzle & Action: BoMulEul Chajara (JUET 970125 V2.00K)", { 0x602A024, 0x6013EE4, 0x602AB64, 0x6011332 }, 0x602AAF8, 0, thuntk_hle, 0, 256 }, NULL, {
		{ HEADER_BLOB, "2.ic13_2",      0x0200000, 0x0080000, 0x6cae2926 },
		{ HEADER_BLOB, "1.ic13_1",      0x0200001, 0x0080000, 0x460727c8 },
		{ BYTE_BLOB,   "bom210-10.ic2", 0x1c00000, 0x0400000, 0xf59d0827 },
		{ BYTE_BLOB,   "bom210-10.ic2", 0x0400000, 0x0400000, 0xf59d0827 },
		{ BYTE_BLOB,   "bom210-11.ic3", 0x2000000, 0x0400000, 0x44e5a13e },
		{ BYTE_BLOB,   "bom210-11.ic3", 0x0800000, 0x0400000, 0x44e5a13e },
		{ BYTE_BLOB,   "bom210-12.ic4", 0x2400000, 0x0400000, 0xdeabc701 },
		{ BYTE_BLOB,   "bom210-12.ic4", 0x0c00000, 0x0400000, 0xdeabc701 },
		{ BYTE_BLOB,   "bom210-13.ic5", 0x2800000, 0x0400000, 0x5ece1d5c },
		{ BYTE_BLOB,   "bom210-13.ic5", 0x1000000, 0x0400000, 0x5ece1d5c },
		{ BLOB_END } } },
	{ { "sasissu", NULL, "Taisen Tanto-R Sashissu!! (J 980216 V1.000)", { 0x6070c28, 0 }, 0x60710be, 0x6009584, sasissu_hle,
	    RSTV_HACK_RBG_HALF }, NULL, {
		{ HEADER_BLOB, "epr20542.13", 0x0000001, 0x0100000, 0x0e632db5 },
		{ BYTE_BLOB,   "epr20542.13", 0x0200000, 0x0100000, 0x0e632db5 },
		{ BYTE_BLOB,   "epr20542.13", 0x0300000, 0x0100000, 0x0e632db5 },
		{ WORD_BLOB,   "mpr20544.2",  0x0400000, 0x0400000, 0x661fff5e },
		{ WORD_BLOB,   "mpr20545.3",  0x0800000, 0x0400000, 0x8e3a37be },
		{ WORD_BLOB,   "mpr20546.4",  0x0c00000, 0x0400000, 0x72020886 },
		{ WORD_BLOB,   "mpr20547.5",  0x1000000, 0x0400000, 0x8362e397 },
		{ WORD_BLOB,   "mpr20548.6",  0x1400000, 0x0400000, 0xe37534d9 },
		{ WORD_BLOB,   "mpr20543.1",  0x1800000, 0x0400000, 0x1f688cdf },
		{ BLOB_END } } },
	{ { "sanjeon", "sasissu", "DaeJeon! SanJeon SuJeon (AJTUE 990412 V1.000)", { 0x6070c28, 0 }, 0x60710be, 0x6009584, sasissu_hle,
	    RSTV_HACK_RBG_HALF }, sanjeon_decrypt, {
		{ HEADER_BLOB, "ic11", 0x0000001, 0x0200000, 0x9abae8d4 },
		{ BYTE_BLOB,   "ic13", 0x0400000, 0x0200000, 0xf72c1d13 },
		{ BYTE_BLOB,   "ic14", 0x0600000, 0x0200000, 0xbcd72105 },
		{ BYTE_BLOB,   "ic15", 0x0800000, 0x0200000, 0x8c9c8352 },
		{ BYTE_BLOB,   "ic16", 0x0a00000, 0x0200000, 0x07e11512 },
		{ BYTE_BLOB,   "ic17", 0x0c00000, 0x0200000, 0x46b7b344 },
		{ BYTE_BLOB,   "ic18", 0x0e00000, 0x0200000, 0xd48404e1 },
		{ BYTE_BLOB,   "ic19", 0x1000000, 0x0200000, 0x33d23bb9 },
		{ BYTE_BLOB,   "ic20", 0x1200000, 0x0200000, 0xf8cc1038 },
		{ BYTE_BLOB,   "ic21", 0x1400000, 0x0200000, 0x74ceb649 },
		{ BYTE_BLOB,   "ic22", 0x1600000, 0x0200000, 0x85f31277 },
		{ BYTE_BLOB,   "ic12", 0x1800000, 0x0400000, 0xd5ebc84e },
		{ BLOB_END } } },
	// MAME: stv_5881, init_twcup98 (315-5881 key 05200913), pcflush master 0x605edde / slave 0x6062bca
	// ADR-100: master idle head moved to the loop's branch target 0x605edd8 (MAME's 0x605edde is the jsr return point:
	// the loop "jsr @(0x3C8,gbr) callback; test flag; bt 605edd8" ran 23000 times a frame on the pitch)
	{ { "twcup98", NULL, "Tecmo World Cup '98 (JUET 980410 V1.000)", { 0x605edd8, 0 }, 0x6062bca, 0, twcup98_hle }, twcup98_init, {
		{ HEADER_BLOB, "epr20819.24", 0x0000001, 0x0100000, 0xd930dfc8 },
		{ BYTE_BLOB,   "epr20819.24", 0x0200000, 0x0100000, 0xd930dfc8 },
		{ BYTE_BLOB,   "epr20819.24", 0x0300000, 0x0100000, 0xd930dfc8 },
		{ WORD_BLOB,   "mpr20821.12", 0x0400000, 0x0400000, 0x2d930d23 },
		{ WORD_BLOB,   "mpr20822.13", 0x0800000, 0x0400000, 0x8b33a5e2 },
		{ WORD_BLOB,   "mpr20823.14", 0x0c00000, 0x0400000, 0x6e6d4e95 },
		{ WORD_BLOB,   "mpr20824.15", 0x1000000, 0x0400000, 0x4cf18a25 },
		{ BLOB_END } } },
	// ADR-106: every other ST-V set in MAME (layouts only, no idle loops / HLE yet)
#include "stv_games.h"
};

// ---- A-bus protection: 315-5881 at 0x04FFFFF0-0x04FFFFFF (MAME stv_state::common_prot_r/w) ----
int stv_prot_enabled;
static u32 s_abus[4], s_abus_protenable, s_abus_protkey;

static u16 prot_rom_read(u32 addr) // MAME crypt_read_callback: word at 0x02000000 + 2*addr, byte-swapped
{
	// ADR-106: the cart is 0x3000000 bytes; the old mask 0x2FFFFFE dropped bit 24 (sss / astrass data above 16 MB)
	const u32 off = (2 * addr) & 0x3FFFFFE;
	if (off >= 0x3000000)
		return 0xFFFF;
	const u16 dat = T1ReadWord(CartridgeArea->rom, off);
	return (u16)(((dat & 0xff00) >> 8) | ((dat & 0x00ff) << 8));
}

static void prot_init(u32 key)
{
	sega5881_init(key, prot_rom_read);
	memset(s_abus, 0, sizeof(s_abus));
	s_abus_protenable = s_abus_protkey = 0;
	stv_prot_enabled = 1;
}

static void twcup98_init(void)
{
	prot_init(0x05200913);
}

u32 stv_prot_read_long(u32 addr)
{
	const u32 off = (addr >> 2) & 3;
	if (s_abus_protenable & 0x00010000) {
		if (off == 3) {
			u16 res = sega5881_do_decrypt(), res2 = sega5881_do_decrypt();
			res = (u16)(((res & 0xff00) >> 8) | ((res & 0x00ff) << 8));
			res2 = (u16)(((res2 & 0xff00) >> 8) | ((res2 & 0x00ff) << 8));
#if !defined(__vita__)
			{	// diag (harness): RSTV_PROTLOG=file, sequence of values read from the protection chip port (compare with MAME)
				static FILE *pl; static int init;
				if (!init) { init = 1; const char *e = getenv("RSTV_PROTLOG"); if (e) pl = fopen(e, "w"); }
				if (pl) fprintf(pl, "%08x\n", (u32)res2 | ((u32)res << 16));
			}
#endif
			return (u32)res2 | ((u32)res << 16);
		}
		return s_abus[off];
	}
	return s_abus[off] ? s_abus[off] : T1ReadLong(CartridgeArea->rom, 0x2FFFFF0 + off * 4);
}

void stv_prot_write(u32 addr, u32 data, u32 mask)
{
	const u32 off = (addr >> 2) & 3;
#if !defined(__vita__)
	{ static FILE *pl; static int init; // diag (harness): RSTV_PROTWLOG
	  if (!init) { init = 1; const char *e = getenv("RSTV_PROTWLOG"); if (e) pl = fopen(e, "w"); }
	  if (pl) fprintf(pl, "W %08x %08x %08x\n", (unsigned)(addr & ~3u) | 0x04000000u, (unsigned)data, (unsigned)mask); }
#endif
	s_abus[off] = (s_abus[off] & ~mask) | (data & mask);
	if (off == 0) {
		s_abus_protenable = (s_abus_protenable & ~mask) | (data & mask);
	} else if (off == 2) {
		if (mask & 0xFFFF0000) sega5881_set_addr_low((u16)(data >> 16));
		if (mask & 0x0000FFFF) sega5881_set_addr_high((u16)(data & 0xffff));
	} else if (off == 3) {
		s_abus_protkey = (s_abus_protkey & ~mask) | (data & mask);
		sega5881_set_subkey((u16)(s_abus_protkey >> 16));
	}
}

#define NUM_GAMES ((int)(sizeof(s_games) / sizeof(s_games[0])))

// BIOS candidates in preference order (all target sets are Japanese or multi-region)
static const struct { const char *file; u32 crc; } s_bios[] = {
	{ "epr-23603.ic8",  0xf688ae60 }, // jp  (MAME default)
	{ "epr-20091.ic8",  0x59ed40f4 }, // jp1
	{ "epr-19730.ic8",  0xd0e0889d }, // jp2
	{ "epr-17951a.ic8", 0x2672f9d8 }, // jp3
	{ "epr-17740a.ic8", 0x3e23c81f }, // jp4
	{ "epr-17740.ic8",  0x5c5aa63d }, // jp5
	// ADR-106: games without a Japan release refuse the Japanese BIOS ("disallowed in this country")
	{ "epr-17952a.ic8", 0xd1be2adf }, // us   (index 6)
	{ "epr-17954a.ic8", 0xf7722da3 }, // euro (index 7)
	{ "epr-19854.ic8",  0xe09d1f60 }, // tw   (index 8)
	{ "epr-17953a.ic8", 0xa4c47570 }, // tw1  (index 9)
};
#define NUM_BIOS ((int)(sizeof(s_bios) / sizeof(s_bios[0])))

const StvGameInfo *stv_current_game;
unsigned rstv_game_hacks;

int STVGameCount(void) { return NUM_GAMES; }
const StvGameInfo *STVGameInfo(int index) { return (index >= 0 && index < NUM_GAMES) ? &s_games[index].info : NULL; }

static const StvGame *find_game(const char *romset)
{
	for (int i = 0; i < NUM_GAMES; i++)
		if (strcmp(s_games[i].info.romset, romset) == 0)
			return &s_games[i];
	return NULL;
}

const StvGameInfo *STVFindGame(const char *romset)
{
	const StvGame *g = find_game(romset);
	return g ? &g->info : NULL;
}

#include "rstv_zip.h"

// ---- loading ---------------------------------------------------------------

// MAME title region code "(JUET 980410 ...)": the BIOS order for it (Japan first when the game has J or no code)
static int bios_order(const char *name, int *order)
{
	const char *p = name ? strchr(name, '(') : NULL;
	char code[16] = "";
	int n = 0, k = 0;
	if (p)
		while (p[1 + k] >= 'A' && p[1 + k] <= 'Z' && k < 15) {
			code[k] = p[1 + k];
			k++;
		}
	code[k] = 0;
	if (!k || p[1 + k] != ' ' || strchr(code, 'J')) {
		for (int i = 0; i < 6; i++)
			order[n++] = i;
	} else {
		if (strchr(code, 'U')) order[n++] = 6;
		if (strchr(code, 'E')) order[n++] = 7;
		if (strchr(code, 'T')) { order[n++] = 8; order[n++] = 9; }
		for (int i = 0; i < 6; i++)
			order[n++] = i;
	}
	return n;
}

static int load_bios_from(const char *path, const int *order, int norder)
{
	Zip z;
	if (zip_open(&z, path) != 0)
		return -1;
	for (int oi = 0; oi < norder; oi++) {
		const int i = order[oi];
		const ZipEntry *e = zip_find(&z, s_bios[i].file, s_bios[i].crc);
		if (!e)
			continue;
		u32 size = 0;
		u8 *data = zip_read(&z, e, &size);
		if (!data || size != 0x80000) {
			free(data);
			continue;
		}
		// ROM_LOAD16_WORD_SWAP into the T2 (host word order) BIOS area = plain byte copy
		for (u32 j = 0; j < size; j++)
			T1WriteByte(BiosRom, j, data[j]);
		free(data);
		zip_close(&z);
		return 0;
	}
	zip_close(&z);
	return -1;
}

// stvbios.zip first; then the game's own zip and its parent (MAME non-merged sets carry the BIOS files, ADR-106)
static int load_bios(const char *rom_dir, const StvGame *g, char *err, int errlen)
{
	int order[NUM_BIOS + 6];
	const int norder = bios_order(g->info.name, order);
	char path[1024];
	snprintf(path, sizeof(path), "%s/stvbios.zip", rom_dir);
	if (load_bios_from(path, order, norder) == 0)
		return 0;
	snprintf(path, sizeof(path), "%s/%s.zip", rom_dir, g->info.romset);
	if (load_bios_from(path, order, norder) == 0)
		return 0;
	if (g->info.parent) {
		snprintf(path, sizeof(path), "%s/%s.zip", rom_dir, g->info.parent);
		if (load_bios_from(path, order, norder) == 0)
			return 0;
	}
	snprintf(err, errlen, "no usable BIOS in stvbios.zip or %s.zip", g->info.romset);
	return -1;
}

void (*rstv_load_progress)(int permille); // ADR-095: frontend loading bar (NULL = none)
int STVLoad(const char *rom_dir, const char *romset, char *err, int errlen)
{
	const StvGame *g = find_game(romset);
	if (!g) {
		snprintf(err, errlen, "unsupported set %s", romset);
		return -1;
	}
	if (!CartridgeArea || !CartridgeArea->rom) {
		snprintf(err, errlen, "cartridge not initialised");
		return -1;
	}
	if (rstv_load_progress)
		rstv_load_progress(0);
	if (load_bios(rom_dir, g, err, errlen) != 0)
		return -1;
	if (rstv_load_progress)
		rstv_load_progress(100);

	memset(CartridgeArea->rom, 0xFF, STV_CART_SIZE); // ROMREGION_ERASEFF

	char path[1024];
	Zip own, parent;
	snprintf(path, sizeof(path), "%s/%s.zip", rom_dir, g->info.romset);
	if (zip_open(&own, path) != 0) {
		snprintf(err, errlen, "cannot open %s", path);
		return -1;
	}
	memset(&parent, 0, sizeof(parent));
	if (g->info.parent) {
		snprintf(path, sizeof(path), "%s/%s.zip", rom_dir, g->info.parent);
		zip_open(&parent, path); // optional
	}

	int rc = 0;
	const char *cached_file = NULL;
	u8 *data = NULL;
	u32 size = 0;
	int nblob = 0;
	for (const Blob *b = g->blobs; b->type != BLOB_END; b++)
		nblob++;
	for (const Blob *b = g->blobs; b->type != BLOB_END; b++) {
		if (rstv_load_progress)
			rstv_load_progress(100 + (int)(b - g->blobs) * 850 / (nblob ? nblob : 1));
		if (!cached_file || strcmp(cached_file, b->file) != 0) {
			free(data);
			data = NULL;
			Zip *src = &own;
			const ZipEntry *e = zip_find(&own, b->file, b->crc);
			if (!e) {
				src = &parent;
				e = zip_find(&parent, b->file, b->crc);
			}
			if (!e || !(data = zip_read(src, e, &size))) {
				snprintf(err, errlen, "%s: missing %s", romset, b->file);
				rc = -1;
				break;
			}
			cached_file = b->file;
		}
		u32 len = b->length < size ? b->length : size;
		u8 *rom = CartridgeArea->rom;
		switch (b->type) {
		case HEADER_BLOB:
			for (u32 j = 0; j < len; j++)
				T1WriteByte(rom, b->offset + 2 * j, data[j]);
			break;
		case BYTE_BLOB:
			for (u32 j = 0; j < len; j++)
				T1WriteByte(rom, b->offset + j, data[j]);
			break;
		case WORD_BLOB:
			for (u32 j = 0; j < len; j++)
				T2WriteByte(rom, b->offset + j, data[j]);
			break;
		default:
			break;
		}
	}
	free(data);
	u8 nv[0x80];
	int have_nv = 0;
	if (rc == 0 && g->nv && !strcmp(g->nv, "-")) { // ADR-106: no default EEPROM in MAME = blank chip (all 0xFF)
		memset(nv, 0xFF, sizeof(nv));
		have_nv = 1;
	} else if (rc == 0 && g->nv) { // ADR-106: missing or short default EEPROM falls back to the BIOS default
		Zip *src = &own;
		const ZipEntry *e = zip_find(&own, g->nv, g->nv_crc);
		if (!e) {
			src = &parent;
			e = zip_find(&parent, g->nv, g->nv_crc);
		}
		u8 *d = e ? zip_read(src, e, &size) : NULL;
		if (d && size >= sizeof(nv)) {
			memcpy(nv, d, sizeof(nv));
			have_nv = 1;
		}
		free(d);
	}
	zip_close(&own);
	zip_close(&parent);
	if (rc != 0)
		return rc;

	stv_prot_enabled = 0;
	if (g->init)
		g->init();
	else if (g->key5881)
		prot_init(g->key5881);
	eeprom_start(have_nv ? nv : NULL);
	StvIoSetType(g->io);
	stv_current_game = &g->info;
	{
		// ADR-093: the sandor engine's slave must not run 2048 cycles ahead inside the wake-up write (the whack-a-mole
		// mini game lost the player sprite); 0 keeps the ADR-020 default
		extern int rstv_icap_cycles;
		rstv_icap_cycles = g->info.icap_cycles ? (int)g->info.icap_cycles : 2048;
	}
	STVSetIdleHacks(1);
	return 0;
}

// ADR-106: idle hints for the generated sets (MAME pcflush). MAME checks the hint at any instruction; our idle PCs must be
// loop heads (block starts / branch targets), so when code near a hint runs we look for the branch that closes its loop
u32 rstv_idle_hint[2][4];
int rstv_idle_hint_n[2];
static u8 s_hint_done[2][4];
int rstv_idle_auto; // bit 0 (ADR-108): SGL slSynch on the master, bit 1 (ADR-113): SGL slave queue wait; cleared once found
                    // bits 2/3 (ADR-122): "bra . / nop" on the master / slave, stays on
#include "sh2core.h"

// ADR-108: SGL slSynch waits for the V-blank flag while calling a user callback:
//   loop: mov.l @(cb,gbr),r0 / jsr @r0 / nop / mov.b @(flag,gbr),r0 / cmp/pz r0 / bt loop
// (bakubaku, ffreveng, skychal, patocar ...). It is idle when the callback is empty (rts) or only counts (n++)
static int rstv_sgl_synch_at(u32 addr)
{
	u16 op[7];
	op[0] = MappedMemoryReadWordNocache(addr);
	// most branch targets stop here (the interpreter asks on every taken branch)
	if ((op[0] & 0xFF00) != 0xC600 && (op[0] & 0xFF00) != 0xD100)
		return 0;
	for (int i = 1; i < 7; i++)
		op[i] = MappedMemoryReadWordNocache(addr + i * 2);
	u32 cb;
	if ((op[0] & 0xFF00) == 0xC600) {
		if (op[1] != 0x400B || op[2] != 0x0009 || (op[3] & 0xFF00) != 0xC400 || op[4] != 0x4011 || op[5] != 0x89F9)
			return 0;
		cb = MappedMemoryReadLongNocache(MSH2->regs.GBR + (op[0] & 0xFF) * 4);
	} else {
		// variant (nameclub): mov.l @(d,pc),r1 / mov.l @r1,r0 / jsr @r0 / nop / mov.b @(flag,gbr),r0 / cmp/pz r0 / bt loop
		if (op[1] != 0x6012 || op[2] != 0x400B || op[3] != 0x0009 || (op[4] & 0xFF00) != 0xC400 || op[5] != 0x4011 ||
			op[6] != 0x89F8)
			return 0;
		const u32 lit = (addr & ~3u) + 4 + (op[0] & 0xFF) * 4;
		cb = MappedMemoryReadLongNocache(MappedMemoryReadLongNocache(lit));
	}
	u16 c[5];
	for (int i = 0; i < 5; i++)
		c[i] = MappedMemoryReadWordNocache(cb + i * 2);
	if (c[0] == 0x000B && c[1] == 0x0009)
		return 1; // rts / nop
	// mov.l @(d,pc),r4 / mov.l @r4,r3 / add #1,r3 / rts / mov.l r3,@r4
	return (c[0] & 0xFF00) == 0xD400 && c[1] == 0x6342 && c[2] == 0x7301 && c[3] == 0x000B && c[4] == 0x2432;
}
// addr = a block start / taken branch target on CPU w. Returns 1 if it became an idle PC
int rstv_idle_hint_at(int w, u32 addr)
{
	if ((rstv_idle_auto >> (w + 2)) & 1) {
		// ADR-122: bra . / nop only waits for an interrupt (magzun's master: the game runs in its interrupt handlers)
		if (MappedMemoryReadWordNocache(addr) == 0xAFFE && MappedMemoryReadWordNocache(addr + 2) == 0x0009) {
			for (int k = 0; k < rstv_idle_n[w]; k++)
				if (rstv_idle_pc[w][k] == addr)
					return 1;
#if !defined(__vita__)
			if (getenv("RSTV_HINTLOG"))
				fprintf(stderr, "idle bra-self %s %08x\n", w ? "S" : "M", (unsigned)addr);
#endif
			if (rstv_idle_n[w] < RSTV_MAX_IDLE_PC) {
				rstv_idle_pc[w][rstv_idle_n[w]++] = addr;
				return 1;
			}
		}
	}
	for (int k = 0; k < rstv_idle_hint_n[w]; k++) {
		const u32 x = rstv_idle_hint[w][k];
		if (s_hint_done[w][k] || addr > x || x - addr > 0x80)
			continue;
		// first backward branch at or after the hint (calls and their delay slots are stepped over; forward conditional
		// branches leave the loop or skip work inside it — vfremix's slave polls FTCSR with them before its closing bra)
		u32 pc = x, target = 0;
		for (int n = 0; n < 48 && !target; n++, pc += 2) {
			const u16 op = MappedMemoryReadWordNocache(pc);
			if ((op & 0xFF00) == 0x8900 || (op & 0xFF00) == 0x8B00 || (op & 0xFF00) == 0x8D00 || (op & 0xFF00) == 0x8F00) {
				const u32 t = pc + 4 + (u32)((s32)(s8)(op & 0xFF) * 2);
				if (t > pc)
					continue;
				target = t;
			}
			else if ((op & 0xF000) == 0xA000)
				target = pc + 4 + (u32)((((s32)(op & 0xFFF)) << 20 >> 20) * 2);
			else if (op == 0x000B || op == 0x002B || (op & 0xF0FF) == 0x402B || (op & 0xF0FF) == 0x0023)
				break; // rts / rte / jmp / braf: not a loop
		}
		if (!target || target > x)
			continue;
		if (target != addr)
			continue;
		s_hint_done[w][k] = 1;
#if !defined(__vita__)
		if (getenv("RSTV_HINTLOG"))
			fprintf(stderr, "idle hint %s %08x -> loop head %08x\n", w ? "S" : "M", (unsigned)x, (unsigned)addr);
#endif
		if (rstv_idle_n[w] < RSTV_MAX_IDLE_PC)
			rstv_idle_pc[w][rstv_idle_n[w]++] = addr;
		return 1;
	}
	if (w && (rstv_idle_auto & 2)) {
		// ADR-113: the SGL slave waits for its command queue with a delay between looks:
		//   mov #0x40,r0 / dt r0 / bf -2 / bra check  (finlarch, mausuke ...): the check is the loop head
		const u16 o0 = MappedMemoryReadWordNocache(addr);
		if (o0 == 0xE040 && MappedMemoryReadWordNocache(addr + 2) == 0x4010 && MappedMemoryReadWordNocache(addr + 4) == 0x8BFD) {
			const u16 o3 = MappedMemoryReadWordNocache(addr + 6);
			if ((o3 & 0xF000) == 0xA000) {
				const u32 head = addr + 6 + 4 + (u32)((((s32)(o3 & 0xFFF)) << 20 >> 20) * 2);
				if (head < addr && addr - head < 0x80) {
					rstv_idle_auto &= ~2;
#if !defined(__vita__)
					if (getenv("RSTV_HINTLOG"))
						fprintf(stderr, "idle sgl slave queue S %08x\n", (unsigned)head);
#endif
					if (rstv_idle_n[1] < RSTV_MAX_IDLE_PC)
						rstv_idle_pc[1][rstv_idle_n[1]++] = head;
				}
			}
		}
		return 0; // the head is reached by a later branch
	}
	if (!w && (rstv_idle_auto & 1) && rstv_sgl_synch_at(addr)) {
		rstv_idle_auto &= ~1;
#if !defined(__vita__)
		if (getenv("RSTV_HINTLOG"))
			fprintf(stderr, "idle sgl slSynch M %08x\n",(unsigned)addr);
#endif
		if (rstv_idle_n[0] < RSTV_MAX_IDLE_PC)
			rstv_idle_pc[0][rstv_idle_n[0]++] = addr;
		return 1;
	}
	return 0;
}

// Idle-loop PCs (MAME install_stvbios_speedups + per-game pcflush). on = 0 disables (debug)
void STVSetIdleHacks(int on)
{
	const StvGameInfo *g = stv_current_game;
	rstv_idle_n[0] = rstv_idle_n[1] = 0;
	rstv_idle_hint_n[0] = rstv_idle_hint_n[1] = 0;
	rstv_idle_auto = 0;
	memset(s_hint_done, 0, sizeof(s_hint_done));
	rstv_once_fn = 0;
	rstv_hle_n = 0;
	rstv_game_hacks = g ? g->hacks : 0;
	if (!on || !g)
		return;
	{ // ADR-106: generated sets carry MAME pcflush hints (manual sets list their loop heads directly)
		const StvGame *sg = (const StvGame *)((const char *)g - offsetof(StvGame, info));
		for (int k = 0; k < 4; k++) {
			if (sg->hint_m[k])
				rstv_idle_hint[0][rstv_idle_hint_n[0]++] = sg->hint_m[k];
			if (sg->hint_s[k])
				rstv_idle_hint[1][rstv_idle_hint_n[1]++] = sg->hint_s[k];
		}
	}
	rstv_idle_auto = 3 | 12;
	rstv_once_fn = g->once_fn;
	for (const RstvHleHook *h = g->hle; h && h->pc && rstv_hle_n < RSTV_MAX_HLE; h++)
		rstv_hle[rstv_hle_n++] = *h;
	// MAME pcflush 0x60154b2 / 0x6013aee, moved to the loop heads (branch targets) because the
	// interpreter checks idle PCs on taken branches only
	static const u32 bios_idle[] = { 0x60154a8, 0x6013ae8 };
	for (int c = 0; c < 2; c++)
		for (int i = 0; i < 2; i++)
			rstv_idle_pc[c][rstv_idle_n[c]++] = bios_idle[i];
	for (int i = 0; i < 4; i++)
		if (g->idle_master[i])
			rstv_idle_pc[0][rstv_idle_n[0]++] = g->idle_master[i];
	if (g->idle_slave)
		rstv_idle_pc[1][rstv_idle_n[1]++] = g->idle_slave;
}

// ---- per-game init ---------------------------------------------------------

static u8 bitswap8(u8 v, int b7, int b6, int b5, int b4, int b3, int b2, int b1, int b0)
{
	return (u8)((((v >> b7) & 1) << 7) | (((v >> b6) & 1) << 6) | (((v >> b5) & 1) << 5) |
		(((v >> b4) & 1) << 4) | (((v >> b3) & 1) << 3) | (((v >> b2) & 1) << 2) |
		(((v >> b1) & 1) << 1) | ((v >> b0) & 1));
}

// MAME stv_state::init_sanjeon (operates on the byte-addressed cart region)
static void sanjeon_decrypt(void)
{
	u8 *src = CartridgeArea->rom;
	for (u32 x = 0; x < STV_CART_SIZE; x++) {
		u8 v = T1ReadByte(src, x) ^ 0xff;
		v = bitswap8(v, 7, 2, 5, 1, 3, 6, 4, 0);
		v = bitswap8(v, 4, 6, 5, 7, 3, 2, 1, 0);
		v = bitswap8(v, 7, 6, 5, 4, 2, 3, 1, 0);
		v = bitswap8(v, 7, 0, 5, 4, 3, 2, 1, 6);
		v = bitswap8(v, 3, 6, 5, 4, 7, 2, 1, 0);
		T1WriteByte(src, x, v);
	}
}

#include "yabause.h"
#include <time.h>
#include "smpc.h"
#include "stvio.h"
#include "eeprom.h"
#include "sega5881.h"
// ReST-V save state: the ST-V parts Yabause does not know about
static int stv_state(FILE *fp, int save)
{
#define ST_(x) do { if (save) fwrite(&(x), sizeof(x), 1, fp); else if (fread(&(x), sizeof(x), 1, fp) != 1) return -1; } while (0)
	ST_(stv_prot_enabled);
	ST_(s_abus); ST_(s_abus_protenable); ST_(s_abus_protkey);
#undef ST_
	if (StvIoState(fp, save) || eeprom_state(fp, save) || sega5881_state(fp, save))
		return -1;
	return 0;
}

// Saves made before version 2: the frame count from the last clock the game read (SMPC OREG1-7, BCD local time)
// minus the saved base time, so the clock goes on from where it was (within a second)
static void stv_frames_from_rtc(void)
{
	const u8 *o = SmpcRegs->OREG;
#define BCD(x) ((((x) >> 4) & 15) * 10 + ((x) & 15))
	struct tm t;
	memset(&t, 0, sizeof(t));
	t.tm_year = BCD(o[1]) * 100 + BCD(o[2]) - 1900;
	t.tm_mon = (o[3] & 15) - 1;
	t.tm_mday = BCD(o[4]);
	t.tm_hour = BCD(o[5]);
	t.tm_min = BCD(o[6]);
	t.tm_sec = BCD(o[7]);
	t.tm_isdst = -1;
#undef BCD
	if (t.tm_year < 90 || t.tm_mon < 0 || t.tm_mon > 11 || t.tm_mday < 1 || !SmpcInternalVars)
		return;
	const time_t now = mktime(&t);
	const long long d = (long long)now - (long long)SmpcInternalVars->basetime;
	if (now != (time_t)-1 && d >= 0 && d < 100LL * 24 * 3600)
		yabsys.frame_count = (u32)(d * 60000 / 1001);
}

// version 2 (ADR-104): + the emulated frame count. The SMPC clock is basetime + frames (clocksync); Yabause set the
// count to 0 on every load, so a loaded game saw its clock jump back to power-on (twcup98 players running wild after
// loading a quick save). The clock must follow emulated time only, never the time the emulator was paused or reloaded
int STVSaveState(FILE *fp)
{
	const int offset = StateWriteHeader(fp, "STV ", 2);
	stv_state(fp, 1);
	u32 fc = yabsys.frame_count;
	fwrite(&fc, sizeof(fc), 1, fp);
	return StateFinishHeader(fp, offset);
}

int STVLoadState(FILE *fp, int version, int size)
{
	if (stv_state(fp, 0))
		return -1;
	if (version >= 2) {
		u32 fc;
		if (fread(&fc, sizeof(fc), 1, fp) != 1)
			return -1;
		yabsys.frame_count = fc;
	} else
		stv_frames_from_rtc();
	return size;
}

// ReST-V save state entry points: between frames only (the sound thread and VDP1 drawing are brought to rest first)
void rstv_sound_quiesce(void);
void rstv_vdp1_quiesce(void);
int rstv_state_save(const char *path)
{
	rstv_sound_quiesce();
	rstv_vdp1_quiesce();
	return YabSaveState(path);
}

int rstv_state_load(const char *path)
{
	rstv_sound_quiesce();
	rstv_vdp1_quiesce();
	return YabLoadState(path);
}
