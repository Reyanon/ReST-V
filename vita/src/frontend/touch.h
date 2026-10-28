// Front touch screen: on-screen key areas (any number, made in the menu's touch layout editor). Ported from ReiREM /
// ReSys12 (ADR-214)
#ifndef RSTV_FRONTEND_TOUCH_H
#define RSTV_FRONTEND_TOUCH_H

#include <stdint.h>

// keys an area can press: RSTV_KEY_* (Cross .. Start), then the d-pad
enum { RSTV_TKEY_UP = 8, RSTV_TKEY_DOWN, RSTV_TKEY_LEFT, RSTV_TKEY_RIGHT, RSTV_TKEY_COUNT };
// owner: a touch button can also press two keys together (its own, apart from Controls > Combo keys):
// key = RSTV_TKEY_COMBO + bit mask (1 << RSTV_KEY_*) of the two
#define RSTV_TKEY_COMBO 256
#define RSTV_TKEY_IS_COMBO(k) ((k) >= RSTV_TKEY_COMBO)

typedef struct { int16_t x, y, w, h, key; } RstvTouchArea; // screen pixels (960x544)

void rstv_touch_init(void);                 // start sampling, read the layout (ux0:data/restv/touch.cfg)
int rstv_touch_count(void);
RstvTouchArea *rstv_touch_area(int i);
int rstv_touch_add(int x, int y, int w, int h, int key); // index of the new area (-1: no memory)
void rstv_touch_remove(int i);
void rstv_touch_clear(void);
void rstv_touch_save(void);
int rstv_touch_gen(void);                   // changes whenever the layout changes
void rstv_touch_changed(void);              // an area was edited in place
uint32_t rstv_touch_key_code(int key);      // RSTV_TKEY / RSTV_KEY -> SCE_CTRL_* bits
const char *rstv_touch_key_name(int key);

// the touches now (screen pixels, up to 6); returns the count
int rstv_touch_read(int *x, int *y);
// SCE_CTRL_* bits of the areas touched now
uint32_t rstv_touch_buttons(void);

#endif
