#include "frontend/cheats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "platform/log.h"

// Per-game cheats from a text file, ux0:data/restv/cheats/<set>.txt (not built into the app). One cheat a line:
//   name | address (hex) | start value (hex) | label = value (hex), label = value, ...
// Lines starting with # are comments. A cheat writes one byte: while the byte holds the start value (the game's own
// value at a new game), it is replaced by the chosen value, so the game starts from that value and runs on normally.
#define CHEAT_DIR "ux0:data/restv/cheats"
#define MAX_ACTIVE 8
#define MAX_VALS 8

typedef struct {
	char name[40];
	u32 addr;
	u8 when;
	int nvals; // choices after Off
	char labels[MAX_VALS][24];
	u8 vals[MAX_VALS];
} RstvCheat;

static RstvCheat s_act[MAX_ACTIVE];
static int s_val[MAX_ACTIVE]; // 0 = Off, k = vals[k - 1]
static int s_n;

static char *trim(char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;
	char *e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
		*--e = 0;
	return s;
}

// one "name | addr | when | label = value, ..." line; 0 if it is not a cheat
static int parse_line(char *line, RstvCheat *c)
{
	char *f[4];
	int n = 0;
	for (char *p = line; n < 4;) {
		f[n++] = p;
		char *bar = strchr(p, '|');
		if (!bar)
			break;
		*bar = 0;
		p = bar + 1;
	}
	if (n < 4)
		return 0;
	memset(c, 0, sizeof(*c));
	snprintf(c->name, sizeof(c->name), "%s", trim(f[0]));
	c->addr = (u32)strtoul(trim(f[1]), NULL, 16);
	c->when = (u8)strtoul(trim(f[2]), NULL, 16);
	for (char *v = f[3]; v && *v && c->nvals < MAX_VALS;) {
		char *comma = strchr(v, ',');
		if (comma)
			*comma = 0;
		char *eq = strchr(v, '=');
		if (eq) {
			*eq = 0;
			snprintf(c->labels[c->nvals], sizeof(c->labels[0]), "%s", trim(v));
			c->vals[c->nvals] = (u8)strtoul(trim(eq + 1), NULL, 16);
			c->nvals++;
		}
		v = comma ? comma + 1 : NULL;
	}
	return c->name[0] && c->addr && c->nvals > 0;
}

void rstv_cheats_init(const char *set)
{
	s_n = 0;
	if (!set || !set[0])
		return;
	char path[128];
	snprintf(path, sizeof(path), CHEAT_DIR "/%s.txt", set);
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	char line[256];
	while (s_n < MAX_ACTIVE && fgets(line, sizeof(line), f)) {
		char *l = trim(line);
		if (!*l || *l == '#')
			continue;
		if (parse_line(l, &s_act[s_n])) {
			s_val[s_n] = 0;
			s_n++;
		}
	}
	fclose(f);
	RSTV_LOG("cheats: %d from %s", s_n, path);
}

int rstv_cheats_count(void)
{
	return s_n;
}

const char *rstv_cheats_name(int i)
{
	return i >= 0 && i < s_n ? s_act[i].name : "";
}

const char *rstv_cheats_value(int i)
{
	if (i < 0 || i >= s_n)
		return "";
	return s_val[i] ? s_act[i].labels[s_val[i] - 1] : "Off";
}

void rstv_cheats_change(int i, int dir)
{
	if (i < 0 || i >= s_n)
		return;
	const int n = s_act[i].nvals + 1;
	s_val[i] = ((s_val[i] + dir) % n + n) % n;
}

int rstv_cheats_values(int i)
{
	return i >= 0 && i < s_n ? s_act[i].nvals + 1 : 0;
}

const char *rstv_cheats_label(int i, int k)
{
	if (i < 0 || i >= s_n || k < 0 || k > s_act[i].nvals)
		return "";
	return k ? s_act[i].labels[k - 1] : "Off";
}

int rstv_cheats_current(int i)
{
	return i >= 0 && i < s_n ? s_val[i] : 0;
}

void rstv_cheats_frame(void)
{
	for (int i = 0; i < s_n; i++) {
		if (!s_val[i])
			continue;
		const RstvCheat *c = &s_act[i];
		if (MappedMemoryReadByteNocache(c->addr) == c->when)
			MappedMemoryWriteByteNocache(c->addr, c->vals[s_val[i] - 1]);
	}
}
