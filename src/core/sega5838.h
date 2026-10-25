/*  Sega 315-5838 / 317-0229 / 317-0230 / 317-0231 decompression (ST-V cartridge, DecAthlete, Name Club, Print Club).
    Ported to C for ReST-V from MAME src/mame/sega/315-5838_317-0229_comp.cpp (mamedev/mame, last change 272ed7c2
    2026-10-01), license BSD-3-Clause, copyright-holders: David Haywood, Samuel Neves, Peter Wilhelmsen,
    Morten Shearman Kirkegaard. */
#ifndef SEGA5838_H
#define SEGA5838_H

#include <stdint.h>
#include <stdio.h>

/* variant 0 = 315-5838, 1 = 317-0229, 2 = 317-0230, 3 = 317-0231; read returns the encrypted word at a word address */
void sega5838_init(int variant, uint16_t (*read)(uint32_t word_addr));
void sega5838_source_w(uint32_t data, uint32_t mem_mask);
void sega5838_table_w(int offset, uint16_t data);
uint16_t sega5838_data_r(void);
void sega5838_read_words(uint16_t *dst, uint32_t n); // n sega5838_data_r calls at once (ADR-207)
int sega5838_state(FILE *fp, int save); /* 0 = ok */

#endif
