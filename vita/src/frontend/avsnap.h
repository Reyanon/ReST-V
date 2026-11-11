// The game list's preview (snap/<set>.mp4, .png, .jpg; ADR-239, from ReBOX)
#ifndef RSTV_FRONTEND_AVSNAP_H
#define RSTV_FRONTEND_AVSNAP_H

#include "video/gx.h"

enum { RSTV_SNAP_NONE, RSTV_SNAP_IMAGE, RSTV_SNAP_VIDEO };

void rstv_snap_request(const char *game, const char *parent); // (NULL: none) what was shown goes at once
const GxTex *rstv_snap_frame(float *u1, float *v1); // once a frame: the picture to draw (u1, v1: its part of the
                                                    // texture), NULL while there is none
int rstv_snap_kind(void);              // what is on the screen: RSTV_SNAP_*
float rstv_snap_fade(void);            // how far it has faded in, 0 to 1

#endif
