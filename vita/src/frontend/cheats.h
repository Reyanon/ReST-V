#ifndef RSTV_CHEATS_H
#define RSTV_CHEATS_H

// Built-in per-game cheats (in-game menu "Cheats"). Values start at Off for every game and are not saved.
void rstv_cheats_init(const char *set); // pick the cheats of this set (all Off)
int rstv_cheats_count(void);
const char *rstv_cheats_name(int i);
const char *rstv_cheats_value(int i);
void rstv_cheats_change(int i, int dir);
void rstv_cheats_frame(void); // after every emulated frame (emulation thread)

#endif
