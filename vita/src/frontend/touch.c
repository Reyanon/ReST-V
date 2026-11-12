// Front touch screen: on-screen key areas and the touch points. See touch.h (ADR-214, from ReiREM)
#include "frontend/touch.h"

#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frontend/menu.h"
#include "platform/log.h"
#include "platform/timer.h"

#define TOUCH_PATH "ux0:data/restv/touch.cfg"

static RstvTouchArea *s_area;
static int s_n, s_cap, s_gen;

void rstv_touch_init(void)
{
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
	FILE *f = fopen(TOUCH_PATH, "r");
	if (!f)
		return;
	int x, y, w, h, k;
	while (fscanf(f, "%d %d %d %d %d", &x, &y, &w, &h, &k) == 5)
		rstv_touch_add(x, y, w, h, k);
	fclose(f);
	RSTV_LOG("touch: %d key areas", s_n);
}

int rstv_touch_count(void) { return s_n; }
RstvTouchArea *rstv_touch_area(int i) { return i >= 0 && i < s_n ? &s_area[i] : NULL; }
int rstv_touch_gen(void) { return s_gen; }
void rstv_touch_changed(void) { s_gen++; }

int rstv_touch_add(int x, int y, int w, int h, int key)
{
	if (s_n == s_cap) {
		const int cap = s_cap ? s_cap * 2 : 16;
		RstvTouchArea *a = (RstvTouchArea *)realloc(s_area, (size_t)cap * sizeof(*a));
		if (!a)
			return -1;
		s_area = a;
		s_cap = cap;
	}
	RstvTouchArea *a = &s_area[s_n];
	a->x = (int16_t)x;
	a->y = (int16_t)y;
	a->w = (int16_t)w;
	a->h = (int16_t)h;
	a->key = (int16_t)((key >= 0 && key < RSTV_TKEY_COUNT) || (key > RSTV_TKEY_COMBO && key < RSTV_TKEY_COMBO + (1 << RSTV_KEY_COUNT))
		? key : RSTV_KEY_CROSS);
	s_gen++;
	return s_n++;
}

void rstv_touch_remove(int i)
{
	if (i < 0 || i >= s_n)
		return;
	memmove(&s_area[i], &s_area[i + 1], (size_t)(s_n - i - 1) * sizeof(*s_area));
	s_n--;
	s_gen++;
}

void rstv_touch_clear(void)
{
	s_n = 0;
	s_gen++;
}

void rstv_touch_save(void)
{
	FILE *f = fopen(TOUCH_PATH, "w");
	if (!f) {
		RSTV_LOG("touch: save failed %s", TOUCH_PATH);
		return;
	}
	for (int i = 0; i < s_n; i++)
		fprintf(f, "%d %d %d %d %d\n", s_area[i].x, s_area[i].y, s_area[i].w, s_area[i].h, s_area[i].key);
	fclose(f);
}

uint32_t rstv_touch_key_code(int key)
{
	static const uint32_t dirs[4] = { SCE_CTRL_UP, SCE_CTRL_DOWN, SCE_CTRL_LEFT, SCE_CTRL_RIGHT };
	if (key >= RSTV_TKEY_UP && key < RSTV_TKEY_COUNT)
		return dirs[key - RSTV_TKEY_UP];
	if (RSTV_TKEY_IS_COMBO(key)) {
		uint32_t b = 0;
		for (int k = 0; k < RSTV_KEY_COUNT; k++)
			if ((key - RSTV_TKEY_COMBO) & (1 << k))
				b |= rstv_key_code(k);
		return b;
	}
	return rstv_key_code(key);
}

const char *rstv_touch_key_name(int key)
{
	static const char *const dirs[4] = { "Up", "Down", "Left", "Right" };
	if (key >= RSTV_TKEY_UP && key < RSTV_TKEY_COUNT)
		return dirs[key - RSTV_TKEY_UP];
	if (RSTV_TKEY_IS_COMBO(key)) {
		static char nm[32];
		nm[0] = 0;
		for (int k = 0; k < RSTV_KEY_COUNT; k++)
			if ((key - RSTV_TKEY_COMBO) & (1 << k))
				snprintf(nm + strlen(nm), sizeof(nm) - strlen(nm), "%s%s", nm[0] ? "+" : "", rstv_key_name(k));
		return nm;
	}
	return rstv_key_name(key);
}

#ifdef RSTV_DEBUG_TOOLS
// test: ux0:data/restv/touchsim = a finger script, one step a line "x y reads" (x < 0: no finger), played by the reads
// that follow (one a frame in the list); the file is removed when it is loaded
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
static int s_sim_x[64], s_sim_y[64], s_sim_n[64], s_sim_steps, s_sim_at;
static int touch_sim(int *x, int *y, int *n)
{
	static unsigned check;
	if (s_sim_at >= s_sim_steps && (check++ & 15) == 0) {
		SceIoStat st;
		if (sceIoGetstat("ux0:data/restv/touchsim", &st) >= 0 && st.st_size > 0) {
			FILE *f = fopen("ux0:data/restv/touchsim", "r");
			s_sim_steps = s_sim_at = 0;
			while (f && s_sim_steps < 64 &&
				fscanf(f, "%d %d %d", &s_sim_x[s_sim_steps], &s_sim_y[s_sim_steps], &s_sim_n[s_sim_steps]) == 3)
				s_sim_steps++;
			if (f)
				fclose(f);
			sceIoRemove("ux0:data/restv/touchsim");
			RSTV_LOG("touch: test script, %d steps", s_sim_steps);
		}
	}
	if (s_sim_at >= s_sim_steps)
		return 0;
	*n = s_sim_x[s_sim_at] >= 0;
	x[0] = s_sim_x[s_sim_at];
	y[0] = s_sim_y[s_sim_at];
	if (--s_sim_n[s_sim_at] <= 0)
		s_sim_at++;
	return 1;
}
#endif

int rstv_touch_read(int *x, int *y)
{
#ifdef RSTV_DEBUG_TOOLS
	{
		int n;
		if (touch_sim(x, y, &n))
			return n;
	}
#endif
	SceTouchData t;
	int n = 0;
	if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t, 1) < 1)
		return 0;
	for (unsigned i = 0; i < t.reportNum && n < 6; i++, n++) { // panel 1920x1088 -> screen 960x544
		x[n] = t.report[i].x / 2;
		y[n] = t.report[i].y / 2;
	}
	return n;
}

uint32_t rstv_touch_buttons(void)
{
	if (!s_n)
		return 0;
	int x[6], y[6];
	const int n = rstv_touch_read(x, y);
	// owner: a coin went in by itself at times when a game started (thuntk): the finger that tapped Play or a game in the
	// list (the touch Select button sits over that corner) was still down. After a pause in these calls (a game start, a
	// closed menu, loading: over 100 ms) the touch buttons wait until no finger is down
	static uint64_t last;
	static int hold;
	const uint64_t now = rstv_time_us();
	if (now - last > 100000)
		hold = 1;
	last = now;
	if (hold) {
		if (n == 0)
			hold = 0;
		return 0;
	}
	uint32_t b = 0;
	for (int k = 0; k < n; k++)
		for (int i = 0; i < s_n; i++) {
			const RstvTouchArea *a = &s_area[i];
			if (x[k] >= a->x && x[k] < a->x + a->w && y[k] >= a->y && y[k] < a->y + a->h)
				b |= rstv_touch_key_code(a->key);
		}
	return b;
}
