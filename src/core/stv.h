/*  ST-V ROM set loader for ReST-V (set table from MAME stv.cpp, blob model from Kronos stv.c).
    GPL-2.0-or-later. */
#ifndef RSTV_STV_H
#define RSTV_STV_H

#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STV_CART_SIZE 0x3000000

typedef struct {
	const char *romset;
	const char *parent; // zip searched after the game's own zip (may be NULL)
	const char *name;
	// Idle-loop heads (from MAME sh2drc_add_pcflush hints, moved to branch targets). 0 = none
	u32 idle_master[4];
	u32 idle_slave;
	// ADR-042: master function the game calls on every scanline while it waits for vblank (polling work);
	// it runs once per frame, later calls return at once. 0 = none
	u32 once_fn;
	// ADR-045: C library routines run natively (pc, kind RSTV_HLE_*; the code is checked before each use). 0-terminated
	const struct RstvHleHook *hle;
	// per-game speed hacks (RSTV_HACK_*; the frontend can force them on or off)
	unsigned hacks;
	// ADR-093: dynarec, cycles the slave may run inside the master's input-capture write (0 = default 2048, ADR-020)
	unsigned icap_cycles;
	// ADR-097: buttons per player the game uses (menu button rows and turbo choices; 0 = 4, the MAME "stv" inputs)
	unsigned buttons;
} StvGameInfo;
// ADR-077: draw RBG0 at half width and stretch it (the rotation shader is the GPU's heaviest pass in sanjeon's 3D games)
#define RSTV_HACK_RBG_HALF 1
// do not start SCU DMAs whose source is a NULL-based address (scu.c rstv_null_dma; thunt/thuntk final boss)
#define RSTV_HACK_NULL_DMA 2
extern unsigned rstv_game_hacks; // hacks of the loaded game (STVSetIdleHacks)

enum { RSTV_HLE_NONE, RSTV_HLE_MEMMOVE, RSTV_HLE_MEMSET8, RSTV_HLE_FILL16, RSTV_HLE_FILL32, RSTV_HLE_MEMCPY, RSTV_HLE_BSSCLR, RSTV_HLE_LZ, RSTV_HLE_SGLVTX, RSTV_HLE_TKVTX, RSTV_HLE_SJINTERP, RSTV_HLE_SGLPOLY, RSTV_HLE_SGLSPR, RSTV_HLE_SGLONSCR, RSTV_HLE_BSSCLR2, RSTV_HLE_DELAY, RSTV_HLE_DELAYR, RSTV_HLE_DELAYR2, RSTV_HLE_VFVTX, RSTV_HLE_VKVTX, RSTV_HLE_MEMMOVE2, RSTV_HLE_DELAYDT, RSTV_HLE_CPY16, RSTV_HLE_FHVTX, RSTV_HLE_FHROW, RSTV_HLE_FHFACE, RSTV_HLE_VKSORT, RSTV_HLE_FHPROJ, RSTV_HLE_FHSND, RSTV_HLE_PURGE, RSTV_HLE_FHRLE, RSTV_HLE_FHMOVE, RSTV_HLE_FHSET, RSTV_HLE_LZB };
typedef struct RstvHleHook {
	u32 pc;
	int kind;
} RstvHleHook;
#define RSTV_MAX_HLE 32
extern RstvHleHook rstv_hle[RSTV_MAX_HLE];
extern int rstv_hle_n;

// Number of supported sets and access by index
int STVGameCount(void);
const StvGameInfo *STVGameInfo(int index);
const StvGameInfo *STVFindGame(const char *romset);

// Load BIOS + cartridge for romset from rom_dir (<rom_dir>/<set>.zip, parent, stvbios.zip).
// Must be called after CartInit(CART_ROMSTV) and BiosRom allocation.
// Returns 0 on success, negative on error (message in errbuf).
int STVLoad(const char *rom_dir, const char *romset, char *errbuf, int errlen);

extern const StvGameInfo *stv_current_game;

// Install the idle-loop speed hacks for the loaded game (STVLoad does this with on = 1)
void STVSetIdleHacks(int on);

#include <stdio.h>
int STVSaveState(FILE *fp);                          // ReST-V: "STV " chunk (I/O, EEPROM, A-bus protection)
int STVLoadState(FILE *fp, int version, int size);
int rstv_state_save(const char *path); // 0 = ok; between frames only
int rstv_state_load(const char *path);
int rstv_state_save_mem(char **out, size_t *len); // ADR-212: into memory (malloc'd), 0 = ok; between frames only
extern int rstv_coin_mailbox_latch; // ADR-151: sound RAM 0x4F1 keeps the SH-2 value until the SH-2 reads it

#ifdef __cplusplus
}
#endif

#endif
