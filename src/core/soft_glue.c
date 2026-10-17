/*  ReST-V: symbols the soft renderer build needs that YabaSanshiro defines in its GL
    files (vidogl.c / ygles.c) or in optional C++ modules.
    Vdp2ColorRamGetColor is copied from YabaSanshiro vidogl.c. GPL-2.0-or-later. */
#include "core.h"
#include "vdp2.h"
#include "yabause.h"

/* Same as ygl.h (little-endian host): R in bits 0-7, alpha in bits 24-31 */
#define SAT2YAB1(alpha,temp)      (alpha << 24 | (temp & 0x1F) << 3 | (temp & 0x3E0) << 6 | (temp & 0x7C00) << 9)
#define SAT2YAB2(alpha,dot1,dot2) (alpha << 24 | ((dot1 & 0xFF) << 16) | (dot2 & 0xFF00) | (dot2 & 0xFF))

u32 Vdp2ColorRamGetColor(u32 colorindex, int alpha)
{
  switch (Vdp2Internal.ColorMode)
  {
  case 0:
  case 1:
  {
    u32 tmp;
    colorindex <<= 1;
    tmp = T2ReadWord(Vdp2ColorRam, colorindex & 0xFFF);
    return SAT2YAB1(alpha, tmp);
  }
  case 2:
  {
    u32 tmp1, tmp2;
    colorindex <<= 2;
    colorindex &= 0xFFF;
    tmp1 = T2ReadWord(Vdp2ColorRam, colorindex);
    tmp2 = T2ReadWord(Vdp2ColorRam, colorindex + 2);
    return SAT2YAB2(alpha, tmp1, tmp2);
  }
  default: break;
  }
  return 0;
}

/* GL colour-RAM texture cache: not used by the soft renderer */
void YglOnUpdateColorRamWord(u32 addr) { (void)addr; }

/* Input recorder (PlayRecorder.cpp) is not used */
int PlayRecorder_setPlayMode(const char *path, yabauseinit_struct *init) { (void)path; (void)init; return -1; }
int PlayRecorder_proc(u32 frame) { (void)frame; return 0; }
