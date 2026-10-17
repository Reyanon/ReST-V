/*  ST-V 93C46 serial EEPROM. Ported from Kronos. GPL-2.0-or-later. */
#ifndef RSTV_EEPROM_H
#define RSTV_EEPROM_H

#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

void eeprom_set_clk(u8 state);
void eeprom_set_di(u8 state);
void eeprom_set_cs(u8 state);
int eeprom_do_read(void);
void eeprom_start(const u8 *table); // NULL = default ST-V contents
int eeprom_load(const char *path);  // 0 = ok
int eeprom_save(const char *path);
int eeprom_dirty(void);
void eeprom_clear_dirty(void); // ReST-V: the frontend wrote a copy of eeprom_data() itself
int eeprom_size(void);
const u8 *eeprom_data(void);

#include <stdio.h>
int eeprom_state(FILE *fp, int save); // ReST-V save state
#ifdef __cplusplus
}
#endif

#endif
