/*
 * c2_ym.cpp - YM3438 (OPN2C) for Sega System C/C2, built on ymfm.
 *
 * Interface glue ported from MAME 0.264 src/devices/sound/ymfm_mame.h / ymopn.cpp
 * (BSD-3-Clause, copyright-holders: Aaron Giles); ymfm itself is in ./ymfm (BSD-3-Clause,
 * Copyright (c) 2021, Aaron Giles).
 * This file is part of ReST-V. GPL-2.0-or-later.
 *
 * Timers and the busy flag are driven by a master-clock time base (XL2 units) that only
 * advances in c2ym_advance_time(). No exceptions, RTTI, threads or iostreams are used.
 */
#include "c2_ym.h"
#include "ymfm/ymfm_opn.h"

#include <vector>

namespace {

class C2YmIntf : public ymfm::ymfm_interface
{
public:
	C2YmIntf() { clear(); }

	void clear()
	{
		m_now = 0;
		m_busy_end = 0;
		m_irq = 0;
		for (int t = 0; t < 2; t++) { m_timer_on[t] = 0; m_timer_left[t] = 0; }
	}

	// ymfm_interface
	void ymfm_set_timer(uint32_t tnum, int32_t duration_in_clocks) override
	{
		if (tnum > 1) return;
		if (duration_in_clocks < 0) { m_timer_on[tnum] = 0; return; }
		m_timer_on[tnum] = 1;
		m_timer_left[tnum] = (int64_t)duration_in_clocks * C2YM_MASTER_DIV;
	}
	void ymfm_set_busy_end(uint32_t clocks) override
	{
		m_busy_end = m_now + (uint64_t)clocks * C2YM_MASTER_DIV;
	}
	bool ymfm_is_busy() override { return m_busy_emu && m_now < m_busy_end; }
	void ymfm_update_irq(bool asserted) override { m_irq = asserted ? 1 : 0; }

	void advance(uint32_t master)
	{
		m_now += master;
		for (uint32_t t = 0; t < 2; t++)
		{
			if (!m_timer_on[t]) continue;
			m_timer_left[t] -= master;
			// fire; keep the overshoot so periodic timers do not drift
			while (m_timer_on[t] && m_timer_left[t] <= 0)
			{
				int64_t over = -m_timer_left[t];
				m_timer_on[t] = 0;
				m_engine->engine_timer_expired(t); // re-arms through ymfm_set_timer
				if (m_timer_on[t]) m_timer_left[t] -= over;
			}
		}
	}

	uint64_t m_now;          // master clocks since init
	uint64_t m_busy_end;
	int64_t  m_timer_left[2];
	int32_t  m_timer_on[2];
	int32_t  m_irq;
	int32_t  m_busy_emu = 0; // configuration, not saved
};

C2YmIntf g_intf;
ymfm::ym3438 g_chip(g_intf);
// sound-only chip (ADR-107): timers never run, so its interface stays idle
C2YmIntf g_gen_intf;
ymfm::ym3438 g_gen(g_gen_intf);

// little-endian helpers for the save file
bool put64(FILE *fp, uint64_t v)
{
	uint8_t b[8];
	for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (i * 8));
	return fwrite(b, 1, 8, fp) == 8;
}
bool get64(FILE *fp, uint64_t *v)
{
	uint8_t b[8];
	if (fread(b, 1, 8, fp) != 8) return false;
	uint64_t r = 0;
	for (int i = 0; i < 8; i++) r |= (uint64_t)b[i] << (i * 8);
	*v = r;
	return true;
}

} // namespace

extern "C" {

void c2ym_init(void)
{
	g_intf.clear();
	g_chip.reset();
	g_gen_intf.clear();
	g_gen.reset();
}

void c2ym_reset(void)
{
	g_intf.clear();
	g_chip.reset();
	g_gen_intf.clear();
	g_gen.reset();
}

void c2ym_gen_write(int a, uint8_t d) { g_gen.write((uint32_t)a & 3, d); }
void c2ym_count_tick(void) { g_chip.clock_count_only(); }

int32_t c2ym_gen_sample(void)
{
	ymfm::ym3438::output_data out;
	g_gen.generate(&out, 1);
	return out.data[0];
}

void c2ym_write(int a, uint8_t d) { g_chip.write((uint32_t)a & 3, d); }
uint8_t c2ym_read(int a) { return g_chip.read((uint32_t)a & 3); }
int c2ym_irq(void) { return g_intf.m_irq; }

int32_t c2ym_sample(void)
{
	ymfm::ym3438::output_data out;
	g_chip.generate(&out, 1);
	return out.data[0]; // left only; right is not connected on C/C2
}

void c2ym_advance_time(uint32_t master) { g_intf.advance(master); }
void c2ym_set_busy_emulation(int on) { g_intf.m_busy_emu = on ? 1 : 0; }

int c2ym_state(FILE *fp, int save)
{
	std::vector<uint8_t> buf;
	if (save)
	{
		ymfm::ymfm_saved_state st(buf, true);
		g_chip.save_restore(st);
		std::vector<uint8_t> gbuf;
		ymfm::ymfm_saved_state gst(gbuf, true);
		g_gen.save_restore(gst);
		bool ok = put64(fp, buf.size())
			&& (buf.empty() || fwrite(buf.data(), 1, buf.size(), fp) == buf.size())
			&& put64(fp, gbuf.size())
			&& (gbuf.empty() || fwrite(gbuf.data(), 1, gbuf.size(), fp) == gbuf.size())
			&& put64(fp, g_intf.m_now) && put64(fp, g_intf.m_busy_end)
			&& put64(fp, (uint64_t)g_intf.m_timer_left[0]) && put64(fp, (uint64_t)g_intf.m_timer_left[1])
			&& put64(fp, (uint64_t)(uint32_t)g_intf.m_timer_on[0]) && put64(fp, (uint64_t)(uint32_t)g_intf.m_timer_on[1])
			&& put64(fp, (uint64_t)(uint32_t)g_intf.m_irq);
		return ok ? 0 : -1;
	}

	uint64_t n, now, busy, tl0, tl1, to0, to1, irq;
	if (!get64(fp, &n) || n > (1u << 20)) return -1;
	buf.resize((size_t)n);
	if (n && fread(buf.data(), 1, (size_t)n, fp) != n) return -1;
	std::vector<uint8_t> gbuf;
	if (!get64(fp, &n) || n > (1u << 20)) return -1;
	gbuf.resize((size_t)n);
	if (n && fread(gbuf.data(), 1, (size_t)n, fp) != n) return -1;
	if (!get64(fp, &now) || !get64(fp, &busy) || !get64(fp, &tl0) || !get64(fp, &tl1)
		|| !get64(fp, &to0) || !get64(fp, &to1) || !get64(fp, &irq))
		return -1;
	ymfm::ymfm_saved_state st(buf, false);
	g_chip.save_restore(st);
	ymfm::ymfm_saved_state gst(gbuf, false);
	g_gen.save_restore(gst);
	g_intf.m_now = now;
	g_intf.m_busy_end = busy;
	g_intf.m_timer_left[0] = (int64_t)tl0;
	g_intf.m_timer_left[1] = (int64_t)tl1;
	g_intf.m_timer_on[0] = (int32_t)to0;
	g_intf.m_timer_on[1] = (int32_t)to1;
	g_intf.m_irq = (int32_t)irq;
	return 0;
}

} // extern "C"
