#ifndef RSTV_CHEATS_H
#define RSTV_CHEATS_H

// Per-game cheats from ux0:data/restv/cheats/<set>.txt (in-game menu "Cheats"). Values start at Off and are not saved.
void rstv_cheats_init(const char *set); // pick the cheats of this set (all Off)
int rstv_cheats_count(void);
const char *rstv_cheats_name(int i);
const char *rstv_cheats_value(int i);
void rstv_cheats_change(int i, int dir);
int rstv_cheats_values(int i);                 // values of cheat i (Off first)
const char *rstv_cheats_label(int i, int k);  // value k of cheat i ("Off" for 0)
int rstv_cheats_current(int i);                // the value cheat i has now
void rstv_cheats_frame(void); // after every emulated frame (emulation thread)

#endif
