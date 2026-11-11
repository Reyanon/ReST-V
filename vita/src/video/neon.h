// Neon corridor: the moving artwork beside the game picture (ADR-236, from ReBOX)
#pragma once

#include <stdint.h>

struct GxTex;
// outside a scene: the still background and rails for the picture's columns game_x / game_w and the theme's colour
// accent (ABGR), as a texture of the screen's size (made again only when they change); NULL if it cannot be made
struct GxTex *rstv_neon_prepare(float game_x, float game_w, uint32_t accent);
// inside the screen's scene, over that texture: the moving rings, pulses and dust (made on a thread of their own)
void rstv_neon_draw(void);
