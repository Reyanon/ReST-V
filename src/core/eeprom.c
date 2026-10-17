/*  ST-V 93C46 serial EEPROM (64 x 16 bit), driven through SMPC PDR1.
    Ported from Kronos (sys/memory/src/eeprom.c, GPL-2.0-or-later) for ReST-V.
    Changes: di_index is signed, logging removed, explicit save path API.

    This file is part of ReST-V, based on Yabause/Kronos. GPL-2.0-or-later.
*/
#include "eeprom.h"

#include <stdio.h>
#include <string.h>

// Command: start bit, 2-bit opcode, 6-bit address, 16-bit data = 26 bits

static u8 cs, clk, di, d_o = 1;
static u8 reg[64 * 2];
static u32 input;
static int di_index;
static u8 EW;
static int cmd = -1;
static u16 ext_data;
static int dirty;

static void executeCmd(void)
{
	d_o = 1;
	if (di_index == 22) {
		u8 new_cmd = (input >> 22) & 0x3;
		cmd = -1;
		if (new_cmd == 0) {
			cmd = 0;
			EW = EW ? 0 : 1; // EWEN / EWDS
		} else {
			cmd = (new_cmd == 2) ? 2 : (EW == 0 ? -1 : new_cmd);
		}
	}
	if (di_index == 0 && cmd == 1) { // WRITE
		u8 addr = (input >> 16) & 0x3F;
		u16 data = input & 0xFFFF;
		reg[addr * 2 + 1] = data & 0xFF;
		reg[addr * 2] = data >> 8;
		dirty = 1;
	}
	if (di_index == 16 && cmd == 2) { // READ
		u8 addr = (input >> 16) & 0x3F;
		ext_data = reg[addr * 2 + 1] | (reg[addr * 2] << 8);
	}
	if (di_index < 16 && cmd == 2)
		d_o = (ext_data >> di_index) & 0x1;
	if (di_index == 16 && cmd == 3) { // ERASE
		u8 addr = (input >> 16) & 0x3F;
		reg[addr * 2] = 0;
		reg[addr * 2 + 1] = 0;
		dirty = 1;
	}
}

void eeprom_set_clk(u8 state)
{
	state &= 1;
	if (state == clk)
		return;
	if (cs == 1 && state == 1 && di_index >= 0) {
		input &= ~(1u << di_index);
		input |= (u32)di << di_index;
		executeCmd();
		di_index--;
	}
	clk = state;
}

void eeprom_set_di(u8 state) { di = state & 1; }

void eeprom_set_cs(u8 state)
{
	state &= 1;
	if (state == cs)
		return;
	if (state == 1) {
		input = 0;
		di_index = 25;
		cmd = -1;
	} else {
		d_o = 1;
	}
	cs = state;
}

int eeprom_do_read(void) { return d_o; }

static const u8 s_default_nv[0x80] = {
	0x53,0x45,0x47,0x41,0xff,0xff,0xff,0xff,0xdf,0xf9,0xff,0xff,0x00,0x00,0x00,0x04,
	0x01,0x00,0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x08,0x08,0xfd,0x01,0x01,0x01,0x02,
	0x02,0x02,0x02,0x01,0x02,0x02,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
	0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
	0xff,0xff,0xff,0xff,0xdf,0xf9,0xff,0xff,0x00,0x00,0x00,0x04,0x01,0x00,0x01,0x01,
	0x00,0x00,0x00,0x00,0x00,0x08,0x08,0xfd,0x01,0x01,0x01,0x02,0x02,0x02,0x02,0x01,
	0x02,0x02,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
	0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};

void eeprom_start(const u8 *table)
{
	memcpy(reg, table ? table : s_default_nv, sizeof(reg));
	cs = clk = di = 0;
	d_o = 1;
	cmd = -1;
	dirty = 0;
}

int eeprom_load(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	u8 buf[sizeof(reg)];
	size_t n = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	if (n != sizeof(buf))
		return -1;
	memcpy(reg, buf, sizeof(reg));
	dirty = 0;
	return 0;
}

int eeprom_save(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	size_t n = fwrite(reg, 1, sizeof(reg), f);
	fclose(f);
	dirty = 0;
	return n == sizeof(reg) ? 0 : -1;
}

int eeprom_dirty(void) { return dirty; }
void eeprom_clear_dirty(void) { dirty = 0; }
int eeprom_size(void) { return (int)sizeof(reg); }
const u8 *eeprom_data(void) { return reg; }

#define ST_(x) do { if (save) fwrite(&(x), sizeof(x), 1, fp); else if (fread(&(x), sizeof(x), 1, fp) != 1) return -1; } while (0)
int eeprom_state(FILE *fp, int save)
{
	ST_(cs); ST_(clk); ST_(di); ST_(d_o);
	ST_(reg);
	ST_(input); ST_(di_index); ST_(EW); ST_(cmd); ST_(ext_data);
	return 0;
}
#undef ST_
