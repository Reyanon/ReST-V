/*  ST-V I/O controller (315-5649) at 0x00400000.
    Register layout and behaviour ported from Kronos core/peripheral/common/peripheral.c
    (GPL-2.0-or-later), which follows MAME stv.cpp.

    0x01 PORT-A (P1)   0x03 PORT-B (P2)   0x05 PORT-C (SYSTEM)   0x07 PORT-D (OUTPUT)
    0x09 PORT-E (P3)   0x0b PORT-F (P4)   0x0d PORT-G            0x1d MODE

    This file is part of ReST-V, based on Yabause/Kronos. GPL-2.0-or-later.
*/
#include "stvio.h"
#include "yabause.h"
#include <stdio.h>
#include <stdlib.h>

enum { PORT_A, PORT_B, PORT_C, PORT_E, PORT_F, PORT_G, PORT_COUNT };

static u8 s_port[PORT_COUNT];
static u8 s_system_output;
static u8 s_ioga_mode;
static u8 s_ioga_portg;
// ADR-106: board input variants (MAME machine "hopper", input "patocar")
int stv_io_type;
// medal hopper (MAME ticket_dispenser, defaults: motor and status active low, 100 ms period): a write of PORT-D bit 7 = 0
// powers the motor; while powered the status line toggles every 100 ms (6 frames), it rests high
static u32 s_frame, s_hop_t0;
static u8 s_hop_power, s_hop_status = 1;
static u8 s_mj_rows[5], s_mj_mux, s_mj_portsel; // mahjong panel (MAME stvmp_ioga_r / _w)

void StvIoSetMahjong(const u8 rows[5])
{
	for (int i = 0; i < 5; i++)
		s_mj_rows[i] = rows[i];
}

void StvIoSetType(int type) { stv_io_type = type; }

void StvIoReset(void)
{
	for (int i = 0; i < PORT_COUNT; i++)
		s_port[i] = 0xFF;
	s_system_output = 0;
	s_ioga_mode = 0;
	s_ioga_portg = 0;
	s_hop_power = 0;
	s_hop_status = 1;
	s_mj_mux = s_mj_portsel = 0;
}

void StvIoSetInputs(u8 p1, u8 p2, u8 system)
{
	s_port[PORT_A] = (u8)~p1;
	s_port[PORT_B] = (u8)~p2;
	s_frame++;
	if (stv_io_type == STV_IO_HOPPER && s_hop_power)
		s_hop_status = (u8)(((s_frame - s_hop_t0) / 6) & 1 ? 0 : 1);
	if (stv_io_type == STV_IO_HOPPER) {
		// PORT-A: bit 0 button 2 (active high), bit 1 hopper sensor (read below); PORT-B: bit 1 door switch (active high,
		// closed), bit 5 medal (button 3), the rest unused
		s_port[PORT_A] = (u8)(0xFC | ((p1 & STV_IN_B2) ? 0x01 : 0));
		s_port[PORT_B] = (u8)(0xFD & ~((p1 & STV_IN_B3) ? 0x20 : 0));
	}
	s_port[PORT_C] = (u8)~system;
}

static u8 StvIoReadByte_(u32 addr);
u8 FASTCALL StvIoReadByte(u32 addr)
{
	const u8 v = StvIoReadByte_(addr);
#if !defined(__vita__)
	{ // harness RSTV_IOLOG=from,to : I/O reads (frame, line, address, value)
		static int from = -2, to;
		if (from == -2) { const char *e = getenv("RSTV_IOLOG"); from = -1; if (e) sscanf(e, "%d,%d", &from, &to); }
		if (from >= 0 && (int)yabsys.frame_count >= from && (int)yabsys.frame_count <= to)
			printf("io f%d l%d r %02x = %02x\n", (int)yabsys.frame_count, (int)yabsys.LineCount, (unsigned)(addr & 0x1F), v);
	}
#endif
	return v;
}
static u8 StvIoReadByte_(u32 addr)
{
	switch (addr & 0x1F) {
	case 0x01:
		if (stv_io_type == STV_IO_MAHJONG && !(s_mj_portsel & 0x10)) { // panel rows selected by the mux
			u8 res = 0xFF;
			for (int i = 0; i < 5; i++)
				if ((s_mj_mux >> i) & 1)
					res &= (u8)~s_mj_rows[i];
			return res;
		}
		if (stv_io_type == STV_IO_HOPPER) // bit 1: hopper status line
			return (u8)((s_port[PORT_A] & ~0x02) | (s_hop_status ? 0x02 : 0));
		return s_port[PORT_A];
	case 0x03:
		if (stv_io_type == STV_IO_MAHJONG && !(s_mj_portsel & 0x10))
			return 0xFF; // player 2 panel: nothing pressed
		return s_port[PORT_B];
	case 0x05: return s_port[PORT_C];
	case 0x07: return s_system_output; // read-back
	case 0x09: return s_port[PORT_E];
	case 0x0b: return s_port[PORT_F];
	case 0x0d:
		if (s_ioga_mode & 0x80) { // PORT-G in counter mode (MAME ioga_r): no counter inputs on the ST-V -> 0, counter# auto-increments
			s_ioga_portg = (u8)((s_ioga_portg & 0xf8) | ((s_ioga_portg + 1) & 0x07));
			return 0;
		}
		return s_port[PORT_G];
	case 0x1b: return 0; // serial status
	case 0x1d: return s_ioga_mode;
	default: return 0;
	}
}

u16 FASTCALL StvIoReadWord(u32 addr) { return StvIoReadByte(addr | 1); }

u32 FASTCALL StvIoReadLong(u32 addr)
{
	return ((u32)StvIoReadWord(addr) << 16) | StvIoReadWord(addr + 2);
}

void FASTCALL StvIoWriteByte(u32 addr, u8 val)
{
	switch (addr & 0x1F) {
	case 0x07:
		s_system_output = val; // coin counters / lamps
		if (stv_io_type == STV_IO_HOPPER) {
			if (!(val & 0x80)) {
				if (!s_hop_power) {
					s_hop_power = 1;
					s_hop_t0 = s_frame;
					s_hop_status = 1;
				}
			} else
				s_hop_power = 0;
		}
		break;
	case 0x09:
		if (stv_io_type == STV_IO_MAHJONG)
			s_mj_mux = (u8)(val ^ 0xFF);
		else
			s_port[PORT_E] = val;
		break;
	case 0x11:
		if (stv_io_type == STV_IO_MAHJONG)
			s_mj_portsel = val;
		break;
	case 0x0b: s_port[PORT_F] = val; break;
	case 0x0d: s_ioga_portg = val; break;
	case 0x1d: s_ioga_mode = val; break;
	default: break;
	}
}

void FASTCALL StvIoWriteWord(u32 addr, u16 val) { StvIoWriteByte(addr | 1, (u8)val); }

void FASTCALL StvIoWriteLong(u32 addr, u32 val)
{
	StvIoWriteWord(addr, (u16)(val >> 16));
	StvIoWriteWord(addr + 2, (u16)val);
}

#define ST_(x) do { if (save) fwrite(&(x), sizeof(x), 1, fp); else if (fread(&(x), sizeof(x), 1, fp) != 1) return -1; } while (0)
int StvIoState(FILE *fp, int save)
{
	ST_(s_port);
	ST_(s_system_output);
	ST_(s_ioga_mode);
	ST_(s_ioga_portg);
	if (stv_io_type == STV_IO_HOPPER) {
		ST_(s_frame);
		ST_(s_hop_t0);
		ST_(s_hop_power);
		ST_(s_hop_status);
	}
	if (stv_io_type == STV_IO_MAHJONG) {
		ST_(s_mj_mux);
		ST_(s_mj_portsel);
	}
	return 0;
}
#undef ST_
