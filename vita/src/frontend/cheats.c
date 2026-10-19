#include "frontend/cheats.h"

#include <stdio.h>
#include <string.h>

#include "memory.h"

// A cheat writes one byte. While the byte holds `when` (the game's own start value), it is replaced by the chosen value,
// so the game starts from that value and runs on normally afterwards (it sets `when` again on a new game, and the cheat
// applies again).
typedef struct {
	const char *sets;    // set names separated by spaces
	const char *name;
	u32 addr;
	u8 when;
	int nvals;           // choices after Off
	const char *labels[8];
	u8 vals[8];
} RstvCheat;

// Treasure Hunt: stage number (0 = stage 1 ... 3 = final stage), work RAM byte, same in thunt and thuntk
static const RstvCheat s_table[] = {
	{ " thunt thuntk ", "Start stage", 0x0604E613, 0, 3, { "Stage 2", "Stage 3", "Final stage" }, { 1, 2, 3 } },
};
#define TABLE_N ((int)(sizeof(s_table) / sizeof(s_table[0])))

#define MAX_ACTIVE 8
static const RstvCheat *s_act[MAX_ACTIVE];
static int s_val[MAX_ACTIVE]; // 0 = Off, k = vals[k - 1]
static int s_n;

void rstv_cheats_init(const char *set)
{
	char key[40];
	snprintf(key, sizeof(key), " %s ", set ? set : "");
	s_n = 0;
	for (int i = 0; i < TABLE_N && s_n < MAX_ACTIVE; i++)
		if (strstr(s_table[i].sets, key)) {
			s_act[s_n] = &s_table[i];
			s_val[s_n] = 0;
			s_n++;
		}
}

int rstv_cheats_count(void)
{
	return s_n;
}

const char *rstv_cheats_name(int i)
{
	return i >= 0 && i < s_n ? s_act[i]->name : "";
}

const char *rstv_cheats_value(int i)
{
	if (i < 0 || i >= s_n)
		return "";
	return s_val[i] ? s_act[i]->labels[s_val[i] - 1] : "Off";
}

void rstv_cheats_change(int i, int dir)
{
	if (i < 0 || i >= s_n)
		return;
	const int n = s_act[i]->nvals + 1;
	s_val[i] = ((s_val[i] + dir) % n + n) % n;
}

void rstv_cheats_frame(void)
{
	for (int i = 0; i < s_n; i++) {
		if (!s_val[i])
			continue;
		const RstvCheat *c = s_act[i];
		if (MappedMemoryReadByteNocache(c->addr) == c->when)
			MappedMemoryWriteByteNocache(c->addr, c->vals[s_val[i] - 1]);
	}
}
