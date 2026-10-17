/*  ST-V I/O controller (315-5649) at 0x00400000. Ported from Kronos peripheral.c.
    GPL-2.0-or-later. */
#ifndef RSTV_STVIO_H
#define RSTV_STVIO_H

#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Player port bits (active low in hardware; the API below takes active-high masks)
#define STV_IN_B1    0x01
#define STV_IN_B2    0x02
#define STV_IN_B3    0x04
#define STV_IN_B4    0x08
#define STV_IN_DOWN  0x10
#define STV_IN_UP    0x20
#define STV_IN_RIGHT 0x40
#define STV_IN_LEFT  0x80

// System port (PORT-C) bits
#define STV_SYS_COIN1   0x01
#define STV_SYS_COIN2   0x02
#define STV_SYS_TEST    0x04
#define STV_SYS_SERVICE 0x08
#define STV_SYS_START1  0x10
#define STV_SYS_START2  0x20
#define STV_SYS_MULTI   0x40
#define STV_SYS_PAUSE   0x80

void StvIoReset(void);
// Board input variants (ADR-106)
enum { STV_IO_STD, STV_IO_HOPPER, STV_IO_MAHJONG };
// Mahjong panel (MAME "stvmp"): 5 key rows selected by the I/O chip mux (pressed bits, 1 = pressed). Row bits:
// 0: KAN 01 START 02 E 10 A 20 M 40 I 80 | 1: REACH 01 F 10 B 20 N 40 J 80 | 2: RON 01 G 10 C 20 CHI 40 K 80
// 3: H 10 D 20 PON 40 L 80 | 4: FLIP FLOP 40
void StvIoSetMahjong(const u8 rows[5]);
extern int stv_io_type;
void StvIoSetType(int type);
// Pressed-bit masks (1 = pressed)
void StvIoSetInputs(u8 p1, u8 p2, u8 system);

u8 FASTCALL StvIoReadByte(u32 addr);
u16 FASTCALL StvIoReadWord(u32 addr);
u32 FASTCALL StvIoReadLong(u32 addr);
void FASTCALL StvIoWriteByte(u32 addr, u8 val);
void FASTCALL StvIoWriteWord(u32 addr, u16 val);
void FASTCALL StvIoWriteLong(u32 addr, u32 val);

#include <stdio.h>
int StvIoState(FILE *fp, int save); // ReST-V save state: write (save = 1) or read the I/O registers
#ifdef __cplusplus
}
#endif

#endif
