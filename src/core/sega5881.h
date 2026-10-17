/*  Sega 315-5881 decryption / decompression (ST-V A-bus protection). See sega5881.c (port of MAME, BSD-3-Clause). */
#ifndef RSTV_SEGA5881_H
#define RSTV_SEGA5881_H

#include <stdint.h>

/* read: 16-bit word of cartridge ROM at word address addr (byte-swapped as MAME crypt_read_callback) */
void sega5881_init(uint32_t game_key, uint16_t (*read)(uint32_t addr));
uint16_t sega5881_do_decrypt(void);
void sega5881_set_addr_low(uint16_t data);
void sega5881_set_addr_high(uint16_t data);
void sega5881_set_subkey(uint16_t data);
#include <stdio.h>
int sega5881_state(FILE *fp, int save); // ReST-V save state

#endif
