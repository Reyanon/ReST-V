#ifndef RSTV_FRONTEND_EMU_H
#define RSTV_FRONTEND_EMU_H

#include <stdint.h>

// Core (YabaSanshiro + ST-V) glue. Core list, Yui callbacks, sound output, screen copy.

// start from <set>.zip and stvbios.zip in rom_dir. On failure the reason is in err
// use_dynarec: 1 = SH-2 recompiler, 0 = interpreter
int rstv_emu_init(const char *rom_dir, const char *romset, int use_dynarec, char *err, int errlen);
void rstv_emu_shutdown(void);

// run one frame (input is STV_IN_* / STV_SYS_* bits, 1 = pressed)
void rstv_emu_run_frame(uint8_t p1, uint8_t p2, uint8_t system);

// menu "Reset game" (ADR-094): board reset (from BIOS again, cart and EEPROM kept). Only between frames
void rstv_emu_reset(void);

// game screen size of the previous frame
void rstv_emu_screen_size(int *w, int *h);

#endif
