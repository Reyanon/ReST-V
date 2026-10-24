// ReST-V Vita frontend (M3: single thread, SH-2 interpreter)
//
// Game select: set name (e.g. sandor) in ux0:data/restv/game.txt. If missing, first supported set found in roms/.
// Controls: d-pad = lever, ×○□△ = buttons 1-4, SELECT = coin, START = start,
//       SELECT+START = menu, SELECT+L = quick save, SELECT+R = quick load (same as Reirem), L+R+START = quit
#include <stdio.h>
#include <stdlib.h>
#include "rstv_capture.h"
#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/power.h>

#include "rstv_jit.h"
#include "stv.h"
#include "eeprom.h"
#include "rstv_prof.h"
#include "stv.h"
#include "stvio.h"
#include "c2.h"
#include "c2_sound.h"
void MappedMemoryWriteByteNocache(unsigned int addr, unsigned char val); // core (memory.h): poke.txt

#include "frontend/audio.h"
#include "frontend/emu.h"
#include "frontend/sampler.h"
#include "frontend/menu.h"
#include "frontend/stv_list.h"
#include <psp2/appmgr.h>
#include "rstv_vdp1rec.h"
#include "platform/clock.h"
#include "platform/jit.h"
#include "platform/log.h"
#include "platform/timer.h"
#include "video/gvdp2.h"
#include "video/gx.h"
#include "video/video.h"

#define DATA_DIR "ux0:data/restv"
#define ROM_DIR DATA_DIR "/roms"

int _newlib_heap_size_user = 144 * 1024 * 1024; // cart 48MB + core. The rest is GPU memory blocks (textures, atlas about 50MB)
unsigned int sceUserMainThreadStackSize = 1 * 1024 * 1024;

static void write_text_file(const char *path, const char *text, int append)
{
	SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | (append ? SCE_O_APPEND : SCE_O_TRUNC), 0777);
	if (fd < 0)
		return;
	sceIoWrite(fd, text, strlen(text));
	sceIoClose(fd);
}

// Perf log file writes during play go on a separate thread (memory card writes sometimes took tens of ms and the main thread missed frames).
// Higher priority than main: if lower, main never releases core 0 and this starves. Mostly sleeps; writes are IO waits, so little CPU
static char s_aw_acc[16384], s_aw_out[16384], s_aw_path[128];
static int s_aw_acc_n, s_aw_out_n;
static volatile int s_aw_pending;
// EEPROM copy (in-game saves are also done by the writer thread)
static unsigned char s_ee_buf[256];
static int s_ee_n;
static char s_ee_path[128];
static volatile int s_ee_pending;
// ADR-090: the exit request file (ux0:data/restv/exit) is also checked by this thread. The main loop called sceIoGetstat every 120 frames
// and stalled 0.5-1.8 s when that overlapped a card write (g2 f5820·f6060, underruns 85·320)
static volatile int s_exit_req;
static volatile int s_exit_next; // the exit file says "next": relaunch the app (autostart picks the next game, no home screen)
static int async_writer_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	for (unsigned tick = 0;; tick++) {
		sceKernelDelayThread(200 * 1000);
		if (tick % 10 == 9) {
			SceIoStat st;
			if (sceIoGetstat(DATA_DIR "/exit", &st) >= 0) {
				char c = 0;
				SceUID xf = sceIoOpen(DATA_DIR "/exit", SCE_O_RDONLY, 0);
				if (xf >= 0) {
					sceIoRead(xf, &c, 1);
					sceIoClose(xf);
				}
				s_exit_next = c == 'n';
				sceIoRemove(DATA_DIR "/exit");
				s_exit_req = 1;
			}
		}
		if (s_ee_pending) {
			__sync_synchronize();
			SceUID efd = sceIoOpen(s_ee_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
			if (efd >= 0) {
				sceIoWrite(efd, s_ee_buf, s_ee_n);
				sceIoClose(efd);
			}
			__sync_synchronize();
			s_ee_pending = 0;
		}
		if (!s_aw_pending)
			continue;
		__sync_synchronize();
		SceUID fd = sceIoOpen(s_aw_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
		if (fd >= 0) {
			sceIoWrite(fd, s_aw_out, s_aw_out_n);
			sceIoClose(fd);
		}
		__sync_synchronize();
		s_aw_pending = 0;
	}
	return 0;
}
static void writer_start(void)
{
	static int started;
	if (started)
		return;
	started = 1;
	SceUID t = sceKernelCreateThread("rstv_writer", async_writer_thread, 120, 0x4000, 0, 0x80000, NULL); // CPU3 (ADR-147)
	if (t >= 0)
		sceKernelStartThread(t, 0, NULL);
}
// In-game EEPROM save: snapshot it and hand it to the writer thread (card writes on the main thread sometimes stall for hundreds of ms)
static void eeprom_save_async(const char *path)
{
	if (s_ee_pending)
		return; // previous save still pending: try next time
	writer_start();
	snprintf(s_ee_path, sizeof(s_ee_path), "%s", path);
	s_ee_n = eeprom_size() < (int)sizeof(s_ee_buf) ? eeprom_size() : (int)sizeof(s_ee_buf);
	memcpy(s_ee_buf, eeprom_data(), (size_t)s_ee_n);
	eeprom_clear_dirty();
	__sync_synchronize();
	s_ee_pending = 1;
}
static void append_text_async(const char *path, const char *text)
{
	static int started;
	if (!started) {
		started = 1;
		snprintf(s_aw_path, sizeof(s_aw_path), "%s", path);
		writer_start();
	}
	const int n = (int)strlen(text);
	if (s_aw_acc_n + n < (int)sizeof(s_aw_acc)) {
		memcpy(s_aw_acc + s_aw_acc_n, text, n);
		s_aw_acc_n += n;
	}
	if (!s_aw_pending && s_aw_acc_n) {
		memcpy(s_aw_out, s_aw_acc, s_aw_acc_n);
		s_aw_out_n = s_aw_acc_n;
		s_aw_acc_n = 0;
		__sync_synchronize();
		s_aw_pending = 1;
	}
}

static int file_exists(const char *path)
{
	SceIoStat st;
	return sceIoGetstat(path, &st) >= 0;
}

// Measurement (ux0:data/restv/coretest): how much a spinning thread gets on each core mask, and the thread pointer register
static volatile unsigned s_ct_count, s_ct_tp;
static int core_test_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	unsigned tp;
	__asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(tp));
	s_ct_tp = tp;
	const uint64_t t0 = rstv_time_us();
	unsigned n = 0;
	while (rstv_time_us() - t0 < 300000)
		n++;
	s_ct_count = n;
	return 0;
}
static volatile int s_ct_block;
static int core_block_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	const uint64_t t0 = rstv_time_us();
	while (s_ct_block && rstv_time_us() - t0 < 700000) // ends by itself: never leave spinning threads behind
		__asm__ volatile("" ::: "memory");
	return 0;
}
static void rstv_core_test(void)
{
	unsigned tp;
	__asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(tp));
	RSTV_LOG("coretest: main tp %08x", tp);
	static const int masks[5] = { 0x10000, 0x20000, 0x40000, 0x80000, 0 };
	for (int k = 0; k < 5; k++) {
		s_ct_count = 0;
		s_ct_tp = 0;
		SceUID t = sceKernelCreateThread("rstv_coretest", core_test_thread, 64, 0x4000, 0, masks[k], NULL);
		int rs = t >= 0 ? sceKernelStartThread(t, 0, NULL) : -1;
		SceUInt t1 = 2000000;
		if (t >= 0 && rs >= 0)
			sceKernelWaitThreadEnd(t, NULL, &t1);
		RSTV_LOG("coretest: mask %05x create %08x start %08x count %u tp %08x", masks[k], (unsigned)t, (unsigned)rs, s_ct_count, s_ct_tp);
		sceKernelDelayThread(100000);
		if (t >= 0)
			sceKernelDeleteThread(t);
	}
	// cores 1 and 2 busy with spinning threads of the same priority (core 0 stays free for the main thread), then each mask again
	for (int k = 1; k < 4; k++) {
		SceUID b[3];
		s_ct_block = 1;
		for (int c = 1; c < 3; c++) {
			b[c] = sceKernelCreateThread("rstv_coreblk", core_block_thread, 64, 0x4000, 0, 0x10000 << c, NULL);
			sceKernelStartThread(b[c], 0, NULL);
		}
		s_ct_count = 0;
		SceUID t = sceKernelCreateThread("rstv_coretest", core_test_thread, 64, 0x4000, 0, masks[k], NULL);
		sceKernelStartThread(t, 0, NULL);
		SceUInt tmo = 2000000;
		sceKernelWaitThreadEnd(t, NULL, &tmo);
		s_ct_block = 0;
		for (int c = 1; c < 3; c++) {
			SceUInt tb = 2000000;
			sceKernelWaitThreadEnd(b[c], NULL, &tb);
			sceKernelDeleteThread(b[c]);
		}
		sceKernelDeleteThread(t);
		RSTV_LOG("coretest: busy 1-2, mask %05x count %u", masks[k], s_ct_count);
		sceKernelDelayThread(100000); // the log sends at most 30 lines a second
	}
}

// One-shot autostart request from measurement tools (ux0:data/restv/autostart = set name). Read and delete. If present, start that set without the menu
static int s_autostart;
// Measurement auto input (ux0:data/restv/autoinput.txt): debug builds only, and only for a game the measurement tools
// started (autostart). The file is deleted once read, so a left-over file never presses keys in normal play (owner).
static FILE *auto_input_open(void)
{
#ifdef RSTV_DEBUG_TOOLS
	if (s_autostart)
		return fopen(DATA_DIR "/autoinput.txt", "r");
#endif
	return NULL;
}
static void auto_input_close(FILE *af)
{
	if (af) {
		fclose(af);
		sceIoRemove(DATA_DIR "/autoinput.txt");
	}
}
static const char *pick_game(char *buf, int len)
{
	SceUID fd = sceIoOpen(DATA_DIR "/autostart", SCE_O_RDONLY, 0);
	if (fd >= 0) {
		int n = sceIoRead(fd, buf, len - 1);
		sceIoClose(fd);
		sceIoRemove(DATA_DIR "/autostart");
		if (n > 0) {
			buf[n] = 0;
			for (int i = 0; i < n; i++)
				if (buf[i] == '\r' || buf[i] == '\n' || buf[i] == ' ')
					buf[i] = 0;
			if (STVFindGame(buf) || c2_is_game(buf)) {
				s_autostart = 1;
				return buf;
			}
		}
	}
	fd = sceIoOpen(DATA_DIR "/game.txt", SCE_O_RDONLY, 0);
	if (fd >= 0) {
		int n = sceIoRead(fd, buf, len - 1);
		sceIoClose(fd);
		if (n > 0) {
			buf[n] = 0;
			for (int i = 0; i < n; i++)
				if (buf[i] == '\r' || buf[i] == '\n' || buf[i] == ' ')
					buf[i] = 0;
			if (STVFindGame(buf) || c2_is_game(buf))
				return buf;
		}
	}
	for (int i = 0; i < STVGameCount(); i++) {
		char path[256];
		snprintf(path, sizeof(path), ROM_DIR "/%s.zip", STVGameInfo(i)->romset);
		if (file_exists(path)) {
			snprintf(buf, len, "%s", STVGameInfo(i)->romset);
			return buf;
		}
	}
	return NULL;
}

// Input (ADR-094·095): d-pad and analog stick (setting: off/left/right/both) = lever, buttons 1-4 = configured Vita buttons, SELECT = coin,
// START = start. Buttons of system combos (SELECT+START menu, SELECT+L quick save, SELECT+R quick load; same as Reirem) are not passed
// to the game
static int s_test_frames; // frames left of the TEST switch press from the menu
#ifdef RSTV_DEBUG_TOOLS
// debug only (owner: not in released builds): ux0:data/restv/fastboot = frame number (empty: 1300). Up to that frame the
// emulation runs flat out without composing or presenting (the BIOS boot screens), then normally. Measuring helper
int rstv_fastboot_until, rstv_fastboot_now;
#endif
int rstv_test_session;     // after the menu's TEST switch press: L = SERVICE (move), R = TEST (enter) in the BIOS test menu
static void map_input(const SceCtrlData *pad, uint8_t *p1, uint8_t *sys, unsigned frame)
{
	uint32_t b = pad->buttons;
	uint8_t p = 0, s = 0;
	if ((b & SCE_CTRL_SELECT) && (b & (SCE_CTRL_START | SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER))) {
		// system combo: buttons held with it are not sent to the game
		*p1 = 0;
		*sys = 0;
		return;
	}
	if (b & SCE_CTRL_UP) p |= STV_IN_UP;
	if (b & SCE_CTRL_DOWN) p |= STV_IN_DOWN;
	if (b & SCE_CTRL_LEFT) p |= STV_IN_LEFT;
	if (b & SCE_CTRL_RIGHT) p |= STV_IN_RIGHT;
	// analog stick: tilting more than 48 from center 128 gives that direction (diagonals included)
	if (g_set.stick == RSTV_STICK_LEFT || g_set.stick == RSTV_STICK_BOTH) {
		if (pad->ly < 80) p |= STV_IN_UP;
		if (pad->ly > 176) p |= STV_IN_DOWN;
		if (pad->lx < 80) p |= STV_IN_LEFT;
		if (pad->lx > 176) p |= STV_IN_RIGHT;
	}
	if (g_set.stick == RSTV_STICK_RIGHT || g_set.stick == RSTV_STICK_BOTH) {
		if (pad->ry < 80) p |= STV_IN_UP;
		if (pad->ry > 176) p |= STV_IN_DOWN;
		if (pad->rx < 80) p |= STV_IN_LEFT;
		if (pad->rx > 176) p |= STV_IN_RIGHT;
	}
	if (rstv_input_rot) {
		// ADR-190: the Vita is held turned (Vertical L / R): turn the screen directions back into the game's
		// (90: screen right = game up; 270: screen left = game up)
		const int u = (p & STV_IN_UP) != 0, d = (p & STV_IN_DOWN) != 0, l = (p & STV_IN_LEFT) != 0, r = (p & STV_IN_RIGHT) != 0;
		p &= (uint8_t)~(STV_IN_UP | STV_IN_DOWN | STV_IN_LEFT | STV_IN_RIGHT);
		if (rstv_input_rot == 90)
			p |= (r ? STV_IN_UP : 0) | (l ? STV_IN_DOWN : 0) | (u ? STV_IN_LEFT : 0) | (d ? STV_IN_RIGHT : 0);
		else
			p |= (l ? STV_IN_UP : 0) | (r ? STV_IN_DOWN : 0) | (d ? STV_IN_LEFT : 0) | (u ? STV_IN_RIGHT : 0);
	}
	static const uint8_t stv_btn[4] = { STV_IN_B1, STV_IN_B2, STV_IN_B3, STV_IN_B4 };
	// ADR-097·126 turbo (same as Reirem): while the turbo key (None = off) is held, the turbo button is pressed for the first half of each period
	// (Turbo speed: 4/6/8 frames) and released for the second half. The turbo key's normal mapping is not used
	const uint32_t tm = rstv_key_code(g_set.turbo_key);
	const uint32_t nb = b & ~tm;
	if (tm && (b & tm) && g_set.turbo_btn >= 1 && g_set.turbo_btn <= 4) {
		const unsigned per = (unsigned)rstv_turbo_period(g_set.turbo_speed);
		if (frame % per < per / 2)
			p |= stv_btn[g_set.turbo_btn - 1];
	}
	for (int k = 0; k < rstv_menu_buttons(); k++)
		if (nb & rstv_key_code(g_set.keymap[RSTV_ACT_B1 + k]))
			p |= stv_btn[k];
	// coin and start also from assigned buttons (ADR-126, default SELECT, START)
	if (nb & rstv_key_code(g_set.keymap[RSTV_ACT_COIN]))
		s |= STV_SYS_COIN1;
	if (nb & rstv_key_code(g_set.keymap[RSTV_ACT_START]))
		s |= STV_SYS_START1;
	if (s_test_frames > 0) { // menu "Test mode (BIOS menu)"
		s_test_frames--;
		s |= STV_SYS_TEST;
	}
	if (rstv_test_session) {
		if (b & SCE_CTRL_LTRIGGER)
			s |= STV_SYS_SERVICE;
		if (b & SCE_CTRL_RTRIGGER)
			s |= STV_SYS_TEST;
	}
	*p1 = p;
	*sys = s;
	if (stv_io_type == STV_IO_MAHJONG) {
		// mahjong panel (ADR-106, MAME stvmp): default = d-pad up/right/down/left A B C D, △ ○ × □ E F G H;
		// L held = I J K L, △ M, ○ N, × Chi, □ Pon; R held = up Kan, right Reach, down Ron, left Flip Flop; START = panel start
		u8 r[5] = { 0, 0, 0, 0, 0 };
		const int l = (b & SCE_CTRL_LTRIGGER) != 0, rr = (b & SCE_CTRL_RTRIGGER) != 0;
		const int up = (b & SCE_CTRL_UP) != 0, rt = (b & SCE_CTRL_RIGHT) != 0, dn = (b & SCE_CTRL_DOWN) != 0, lf = (b & SCE_CTRL_LEFT) != 0;
		const int tri = (b & SCE_CTRL_TRIANGLE) != 0, cir = (b & SCE_CTRL_CIRCLE) != 0, crs = (b & SCE_CTRL_CROSS) != 0,
			sqr = (b & SCE_CTRL_SQUARE) != 0;
		if (rr) {
			if (up) r[0] |= 0x01;  // KAN
			if (rt) r[1] |= 0x01;  // REACH
			if (dn) r[2] |= 0x01;  // RON
			if (lf) r[4] |= 0x40;  // FLIP FLOP
		} else if (l) {
			if (up) r[0] |= 0x80;  // I
			if (rt) r[1] |= 0x80;  // J
			if (dn) r[2] |= 0x80;  // K
			if (lf) r[3] |= 0x80;  // L
			if (tri) r[0] |= 0x40; // M
			if (cir) r[1] |= 0x40; // N
			if (crs) r[2] |= 0x40; // CHI
			if (sqr) r[3] |= 0x40; // PON
		} else {
			if (up) r[0] |= 0x20;  // A
			if (rt) r[1] |= 0x20;  // B
			if (dn) r[2] |= 0x20;  // C
			if (lf) r[3] |= 0x20;  // D
			if (tri) r[0] |= 0x10; // E
			if (cir) r[1] |= 0x10; // F
			if (crs) r[2] |= 0x10; // G
			if (sqr) r[3] |= 0x10; // H
		}
		if (b & SCE_CTRL_START)
			r[0] |= 0x02;
		StvIoSetMahjong(r);
	}
}

// Start screen (ADR-094, Reirem list style): supported sets in roms. Cursor starts on the last picked game (game.txt); waits until one is picked.
// Measurement tools skip it with the autostart file (pick_game). Starts directly if there is only one set. The picked set is kept in game.txt
// ADR-191: the loading bar: ROMs 0-70%, GPU set-up (shaders, preloaded variants: seconds) 70-100%
static void load_rom_progress(int p) { rstv_loading_progress(p * 7 / 10); }
static void load_gpu_progress(int p) { rstv_loading_progress(700 + p * 3 / 10); }

static const char *game_menu(char *buf, int len)
{
	const char *set = rstv_game_select(buf[0] ? buf : NULL, ROM_DIR);
	snprintf(buf, len, "%s", set);
	write_text_file(DATA_DIR "/game.txt", buf, 0);
	return buf;
}

// show the error as a solid color screen and wait for START
static void fatal_screen(uint32_t abgr)
{
	for (;;) {
		uint32_t *px = rstv_video_frame_begin(320, 224);
		for (int y = 0; y < 224; y++)
			for (int x = 0; x < 320; x++)
				px[y * RSTV_VIDEO_PITCH + x] = abgr;
		rstv_video_frame_end();
		rstv_video_present();
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		if (pad.buttons & SCE_CTRL_START)
			break;
	}
}

// vita-elf-create appends SCE data (a few KB) to the end of the text segment. If text ends just below a 1MB boundary it overlaps the data segment
// and fails, so push text past the boundary with read-only padding (adjust the size if build error "segment 1 overlaps" shows up again)
__attribute__((used)) static const unsigned char s_text_pad[28 * 1024] = { 1 };

static unsigned s_spike_us = 25000; // late frame log threshold (us)
static int s_spike_sampler;         // if the sampling profiler is on, log top blocks on every late frame
extern unsigned long long rstv_snd_wall_us, rstv_snd_lat_us, rstv_snd_gap_us; extern unsigned rstv_snd_posts; // sound thread wall time of its frames, wake latency (scsp.c)
static int s_quit_mode;             // ADR-094 menu: 1 = quit, 2 = back to game select (relaunch app)

// ---- Sega System C / C2 (ADR-107): 68000 board. Video via CPU texture, sound at 44.1kHz into the same audio ring buffer ----
// YM sound generation on a CPU1 worker thread (c2snd_worker: replays the frame's write log, result independent of thread timing)
static SceUID s_c2w_go = -1, s_c2w_done = -1;
static volatile int s_c2w_quit;
static int c2_worker_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	for (;;) {
		sceKernelWaitSema(s_c2w_go, 1, NULL);
		if (s_c2w_quit)
			break;
		c2snd_worker();
		sceKernelSignalSema(s_c2w_done, 1);
	}
	return 0;
}
static void c2_worker_kick(void) { sceKernelSignalSema(s_c2w_go, 1); }
static void c2_worker_wait(void) { sceKernelWaitSema(s_c2w_done, 1, NULL); }

static void c2_loop(const char *game, const char *jit_msg)
{
	char err[256];
	const char *title = c2_game_title(game);
	rstv_menu_init(game, title ? title : game, 3);
	rstv_loading_title(title ? title : game);
	rstv_loading_progress(0);
	const int rc = c2_load(ROM_DIR, game, err, sizeof(err));
	extern uint64_t (*c2_clock_us)(void);
	extern unsigned c2_prof_us[4];
	c2_clock_us = rstv_time_us;
	SceUID c2w_thr = -1;
	if (rc == 0 && !file_exists(DATA_DIR "/noc2thr")) {
		s_c2w_go = sceKernelCreateSema("c2w_go", 0, 0, 1, NULL);
		s_c2w_done = sceKernelCreateSema("c2w_done", 0, 0, 1, NULL);
		c2w_thr = sceKernelCreateThread("c2_ym", c2_worker_thread, 64, 0x10000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
		if (s_c2w_go >= 0 && s_c2w_done >= 0 && c2w_thr >= 0 && sceKernelStartThread(c2w_thr, 0, NULL) >= 0) {
			c2snd_kick = c2_worker_kick;
			c2snd_wait = c2_worker_wait;
		}
		RSTV_LOG("c2: ym worker %s", c2snd_kick ? "on cpu1" : "inline");
	}
	extern int c2vdp_prof_on;
	extern unsigned c2vdp_prof_us[4];
	extern unsigned c2snd_prof_us[4];
	c2vdp_prof_on = file_exists(DATA_DIR "/proffine");
	rstv_loading_progress(1000);
	rstv_loading_done();
	if (rc != 0) {
		RSTV_LOG("c2 init failed: %s", err);
		write_text_file(DATA_DIR "/last_error.txt", err, 0);
		fatal_screen(0xFF0000C0u);
		return;
	}
	// board RAM (64KB, battery backed): nvram/<set>.nv
	char nv_path[128];
	snprintf(nv_path, sizeof(nv_path), DATA_DIR "/nvram/%s.nv", game);
	sceIoMkdir(DATA_DIR "/nvram", 0777);
	int nv_size = 0;
	uint8_t *nv = c2_nvram(&nv_size);
	{
		FILE *nf = fopen(nv_path, "rb");
		if (nf) {
			const size_t got = fread(nv, 1, (size_t)nv_size, nf);
			fclose(nf);
			RSTV_LOG("c2 nvram %s: %u bytes", nv_path, (unsigned)got);
			c2_reset();
		}
	}
	char perf_path[128], state_path[128];
	snprintf(perf_path, sizeof(perf_path), DATA_DIR "/perf_%s.txt", game);
	write_text_file(perf_path, jit_msg, 0);
	write_text_file(perf_path, "\nsystem: sega c2 (68000 musashi, cpu render)\n", 1);
	snprintf(state_path, sizeof(state_path), DATA_DIR "/state/%s.yss", game);
	sceIoMkdir(DATA_DIR "/state", 0777);
	if (file_exists(DATA_DIR "/nofps"))
		g_set.fps = 0;
	// measurement tool auto input (ux0:data/restv/autoinput.txt: "frame coin|start|up|...|b4")
	unsigned auto_frame[64];
	uint8_t auto_bits[64], auto_p1[64];
	int n_auto = 0;
	{
		FILE *af = auto_input_open();
		unsigned fno;
		char what[16];
		while (af && n_auto < 64 && fscanf(af, "%u %15s", &fno, what) == 2) {
			auto_frame[n_auto] = fno;
			auto_bits[n_auto] = !strcmp(what, "coin") ? STV_SYS_COIN1 : !strcmp(what, "start") ? STV_SYS_START1 : 0;
			auto_p1[n_auto] = !strcmp(what, "up") ? STV_IN_UP : !strcmp(what, "down") ? STV_IN_DOWN : !strcmp(what, "left") ? STV_IN_LEFT :
				!strcmp(what, "right") ? STV_IN_RIGHT : !strcmp(what, "b1") ? STV_IN_B1 : !strcmp(what, "b2") ? STV_IN_B2 :
				!strcmp(what, "b3") ? STV_IN_B3 : !strcmp(what, "b4") ? STV_IN_B4 : 0;
			n_auto++;
		}
		auto_input_close(af);
	}
	// capture for measurement: shot_<frame>_gpu.ppm for each frame number in ux0:data/restv/shot.txt (same name as ST-V, fetched by stshots.sh)
	unsigned shot_at[16];
	int n_shot = 0;
	{
		FILE *sf = fopen(DATA_DIR "/shot.txt", "r");
		while (sf && n_shot < 16 && fscanf(sf, "%u", &shot_at[n_shot]) == 1)
			n_shot++;
		if (sf)
			fclose(sf);
	}
	writer_start(); // exit request check (ADR-090)
	static int16_t abuf[2048 * 2];
	uint64_t win_start = rstv_time_us(), emu_sum = 0, emu_max = 0;
	uint32_t frame = 0, prev_buttons = ~0u;
	double osd_fps = 0.0;
	for (;;) {
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		if (rstv_menu_should_open(pad.buttons)) {
			for (int done = 0; !done;) {
				sceCtrlPeekBufferPositive(0, &pad, 1);
				const rstv_menu_result r = rstv_menu_update(pad.buttons);
				char sp[160];
				const int slot = rstv_menu_slot();
				switch (r) {
				case RSTV_MENU_CLOSE: done = 1; break;
				case RSTV_MENU_RESET: c2_reset(); rstv_osd_show("Game reset"); done = 1; break;
				case RSTV_MENU_SAVE:
					rstv_state_slot_path(sp, sizeof(sp), slot);
					rstv_osd_show(c2_state_save(sp) == 0 ? "Saved: slot %d" : "Save failed: slot %d", slot);
					break;
				case RSTV_MENU_LOAD:
					rstv_state_slot_path(sp, sizeof(sp), slot);
					rstv_osd_show(c2_state_load(sp) == 0 ? "Loaded: slot %d" : "Load failed: slot %d", slot);
					done = 1;
					break;
				case RSTV_MENU_SELECT_GAME: s_quit_mode = 2; done = 1; break;
				case RSTV_MENU_EXIT: s_quit_mode = 1; done = 1; break;
				default: break;
				}
				if (s_exit_req) {
					s_quit_mode = 1;
					done = 1;
				}
				if (!done)
					rstv_menu_draw();
				rstv_osd_tick();
				rstv_video_present();
			}
			rstv_menu_hide();
			rstv_video_present();
			prev_buttons = pad.buttons;
			win_start = rstv_time_us();
			if (s_quit_mode)
				break;
			continue;
		}
		uint8_t p1, sys;
		map_input(&pad, &p1, &sys, frame);
		for (int a = 0; a < n_auto; a++)
			if (frame + 1 >= auto_frame[a] && frame + 1 < auto_frame[a] + 6) {
				sys |= auto_bits[a];
				p1 |= auto_p1[a];
			}
		const uint64_t t0 = rstv_time_us();
		c2_run_frame(p1, 0, sys);
		int n;
		while ((n = c2_audio(abuf, 2048)) > 0)
			rstv_audio_push(abuf, n);
		{
			int w, h, pitch;
			const uint32_t *fb = c2_frame(&w, &h, &pitch);
			uint32_t *dst = rstv_video_frame_begin(w, h);
			for (int y = 0; y < h; y++)
				memcpy(dst + y * RSTV_VIDEO_PITCH, fb + y * pitch, (size_t)w * 4);
			rstv_video_frame_end();
			for (int k = 0; k < n_shot; k++)
				if (shot_at[k] == frame) {
					char sp[96];
					snprintf(sp, sizeof(sp), DATA_DIR "/shot_%u_gpu.ppm", frame);
					FILE *pf = fopen(sp, "wb");
					if (pf) {
						fprintf(pf, "P6\n%d %d\n255\n", w, h);
						static uint8_t row[320 * 3];
						for (int y = 0; y < h; y++) {
							for (int x = 0; x < w; x++) {
								const uint32_t c = fb[y * pitch + x];
								row[x * 3] = (uint8_t)c;
								row[x * 3 + 1] = (uint8_t)(c >> 8);
								row[x * 3 + 2] = (uint8_t)(c >> 16);
							}
							fwrite(row, 3, (size_t)w, pf);
						}
						fclose(pf);
					}
				}
		}
		const uint64_t dt = rstv_time_us() - t0;
		emu_sum += dt;
		if (dt > emu_max)
			emu_max = dt;
		{
			// quick save SELECT+L, quick load SELECT+R (ADR-095)
			const uint32_t qs = SCE_CTRL_SELECT | SCE_CTRL_LTRIGGER, ql = SCE_CTRL_SELECT | SCE_CTRL_RTRIGGER;
			if ((pad.buttons & qs) == qs && (prev_buttons & qs) != qs)
				rstv_osd_show(c2_state_save(state_path) == 0 ? "Saved: quick" : "Save failed: quick");
			else if ((pad.buttons & ql) == ql && (prev_buttons & ql) != ql)
				rstv_osd_show(!file_exists(state_path) ? "No state: quick" :
					c2_state_load(state_path) == 0 ? "Loaded: quick" : "Load failed: quick");
			prev_buttons = pad.buttons;
		}
		{
			const uint64_t tp0 = rstv_time_us();
			rstv_video_present();
			static uint64_t s_last;
			const uint64_t now = rstv_time_us();
			static unsigned s_late;
			if (s_last && now - s_last > 17500 && now - tp0 < 1500 && (++s_late & 3) == 0)
				sceKernelDelayThread(1000); // late frames: yield to the shell (vitacompanion), every 4th (ADR-147)
			s_last = rstv_time_us();
		}
		++frame;
		rstv_fps_update((float)osd_fps, frame);
		rstv_osd_tick();
		if (s_exit_req)
			break;
		if (frame % 60 == 0) {
			sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
			sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_OFF);
			const uint64_t now = rstv_time_us();
			osd_fps = 60.0 * 1e6 / (double)(now - win_start);
			int w, h, pitch;
			c2_frame(&w, &h, &pitch);
			rstv_audio_stats as;
			rstv_audio_get_stats(&as);
			static uint32_t s_und_prev;
			const uint32_t und = as.underruns - s_und_prev;
			s_und_prev = as.underruns;
			char line[256];
			snprintf(line, sizeof(line), "f%u emu %.2f/%.2fms fps %.2f %dx%d aud %d %dppm und %u ovf %u | c2 m68k %.2f vdp %.2f snd %.2f (planes %.2f win %.2f spr %.2f) snd psg %.2f ym %.2f upd %.2f ymt %.2f\n", frame,
				emu_sum / 60 / 1000.0, emu_max / 1000.0, osd_fps, w, h, as.fill, as.drc_ppm, und, as.overflows,
				c2_prof_us[0] / 60000.0, c2_prof_us[1] / 60000.0, c2_prof_us[2] / 60000.0,
				c2vdp_prof_us[0] / 60000.0, c2vdp_prof_us[1] / 60000.0, c2vdp_prof_us[2] / 60000.0,
				c2snd_prof_us[0] / 60000.0, c2snd_prof_us[1] / 60000.0, c2snd_prof_us[2] / 60000.0, c2snd_prof_us[3] / 60000.0);
			memset(c2snd_prof_us, 0, sizeof(c2snd_prof_us));
			memset(c2vdp_prof_us, 0, sizeof(c2vdp_prof_us));
			memset(c2_prof_us, 0, sizeof(c2_prof_us));
			RSTV_LOG("%s", line);
			if (frame % 300 == 0)
				append_text_async(perf_path, line);
			win_start = now;
			emu_sum = emu_max = 0;
		}
	}
	{
		FILE *nf = fopen(nv_path, "wb");
		if (nf) {
			fwrite(nv, 1, (size_t)nv_size, nf);
			fclose(nf);
		}
	}
	RSTV_LOG("c2 exit at f%u", frame);
	c2_unload(); // waits for the worker
	if (c2snd_kick) {
		c2snd_kick = NULL;
		c2snd_wait = NULL;
		s_c2w_quit = 1;
		sceKernelSignalSema(s_c2w_go, 1);
		sceKernelWaitThreadEnd(c2w_thr, NULL, NULL);
		sceKernelDeleteThread(c2w_thr);
	}
}

unsigned rstv_v1log_from, rstv_v1log_to;
int rstv_v1log_n; // diag: VDP1 GPU list lines left to log (ux0:data/restv/v1log)
int main(int argc, char *argv[])
{
	// main (SH-2, SCU, VDP) = CPU0, SCSP/68K thread = CPU1 (thr-restv.c), helper drawing = CPU2
	sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
	rstv_prof_clock = rstv_time_us;
	sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
	rstv_clock_boost();
	const int mhz_start = rstv_clock_measure_arm_mhz(); // measured before any other thread exists
	rstv_log_init();
	{
		int restarted = 0; // (started again by rstv_kctrl_init: "kctrl" among the arguments)
		for (int i = 0; i < argc; i++)
			restarted |= argv[i] && !strcmp(argv[i], "kctrl");
		rstv_kctrl_init(restarted);
	}
	if (file_exists(DATA_DIR "/coretest"))
		rstv_core_test(); // before any other thread exists
	sceIoMkdir(DATA_DIR, 0777);
	sceIoMkdir(ROM_DIR, 0777);

	char jit_msg[160];
	rstv_jit_selftest(jit_msg, sizeof(jit_msg));
	RSTV_LOG("%s", jit_msg);
	write_text_file(DATA_DIR "/jit_result.txt", jit_msg, 0);

	const int mhz_pre_video = rstv_clock_measure_arm_mhz();
	rstv_video_init();
	const int mhz_post_video = rstv_clock_measure_arm_mhz();
	rstv_audio_init();
	const int mhz_post_audio = rstv_clock_measure_arm_mhz();
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG); // left stick as lever (setting STICK)


	char game_buf[64], err[256];
	rstv_settings_load();
	rstv_settings_apply_video();
	const char *game = pick_game(game_buf, sizeof(game_buf));
	if (!game)
		game_buf[0] = 0; // owner: the game list also without ROMs (R in the list reads the folder again)
	if (!s_autostart)
		game = game_menu(game_buf, sizeof(game_buf));
	if (!game) {
		write_text_file(DATA_DIR "/last_error.txt", "no supported set in " ROM_DIR "\n", 0);
		fatal_screen(0xFF800000u); // blue: no ROM
		goto out;
	}
	RSTV_LOG("game: %s", game);
	// ADR-126: per-game settings (over global, if present) and border artwork
	rstv_settings_load_game(game);
	rstv_settings_apply_video();
	{
		const char *parent = NULL;
		for (int i = 0; i < RSTV_LIST_N; i++)
			if (!strcmp(s_stv_list[i].set, game))
				parent = s_stv_list[i].parent;
		rstv_video_set_art(game, parent);
	}
	if (c2_is_game(game)) { // ADR-107: Sega System C / C2
		c2_loop(game, jit_msg);
		goto out;
	}
	{
		const StvGameInfo *gi = STVFindGame(game);
		rstv_menu_init(game, gi && gi->name ? gi->name : game, gi && gi->buttons ? (int)gi->buttons : 4);
	}
	// SH-2 core: interpreter if ux0:data/restv/interp exists, else recompiler
	const int use_dynarec = !file_exists(DATA_DIR "/interp");
	// Section timing (ADR-075): only frame and section level by default. The clock called per HLE, nested run, sample and quad (sceKernelGetProcessTimeWide,
	// about 0.5us per call) only with proffine: thousands of calls per frame in 3D scenes, 1-1.5ms. noprof turns all of it off
	if (file_exists(DATA_DIR "/noprof"))
		rstv_prof_clock = NULL;
	// SH-2 and other fine timing (inside loops) only when ux0:data/restv/proffine exists
	rstv_prof_fine = file_exists(DATA_DIR "/proffine");
	{
		extern int rstv_chain_prof; // ADR-135: block chaining with the sampler's block PC and the chain count
		rstv_chain_prof = rstv_prof_fine || file_exists(DATA_DIR "/sampler");
		// ADR-136: slave SH-2 on its own thread (CPU2) when ux0:data/restv/slaveasync exists; its content: slices of lag
		extern int rstv_slave_cfg, rstv_slave_lead;
		FILE *sf = fopen(DATA_DIR "/slaveasync", "r");
		if (sf) {
			int lead = 0, spin = -1;
			extern int rstv_slave_spin;
			if (fscanf(sf, "%d %d", &lead, &spin) >= 1 && lead > 0 && lead <= 48)
				rstv_slave_lead = lead;
			if (spin >= 0)
				rstv_slave_spin = spin;
			fclose(sf);
			rstv_slave_cfg = 1;
		}
	}
	{ // diag: VDP1 GPU list / FBCR lines for frames "from to" in ux0:data/restv/v1log
		extern int rstv_v1log_n;
		FILE *lf = fopen(DATA_DIR "/v1log", "r");
		if (lf) {
			rstv_v1log_n = 3000;
			if (fscanf(lf, "%u %u", &rstv_v1log_from, &rstv_v1log_to) != 2) { rstv_v1log_from = 0; rstv_v1log_to = 0xFFFFFFFFu; }
			fclose(lf);
		}
	}
	{ extern int rstv_v1_fullsnap; rstv_v1_fullsnap = file_exists(DATA_DIR "/v1full"); }
	// VDP1 drawing helper thread (CPU2). Disabled if ux0:data/restv/novdp1thr exists
	extern int rstv_vdp1_thread;
	rstv_vdp1_thread = !file_exists(DATA_DIR "/novdp1thr");
	// sound runs one frame behind (overlapped with SH-2). Disabled if ux0:data/restv/nosndlag exists
	extern int rstv_scsp_lag;
	rstv_scsp_lag = !file_exists(DATA_DIR "/nosndlag");
	{ // ADR-158: SCSP DSP on CPU3, one batch behind the slots (ux0:data/restv/nodspthr = on the sound thread)
		extern int rstv_scsp_dsp_thread;
		rstv_scsp_dsp_thread = !file_exists(DATA_DIR "/nodspthr");
	}
	// run the 9 decilines before HBlankIN as one SH-2 slice (1/5 the calls). Disabled if ux0:data/restv/nomerge exists
	extern int rstv_deciline_merge;
	rstv_deciline_merge = !file_exists(DATA_DIR "/nomerge");
	// recompiler fast memory functions for high-speed RAM (ADR-017). Disabled if ux0:data/restv/nomemfast exists
	extern int rstv_memfast;
	rstv_memfast = !file_exists(DATA_DIR "/nomemfast");
#ifdef RSTV_DEBUG_TOOLS
	{
		FILE *ff = fopen(DATA_DIR "/fastboot", "r");
		if (ff) {
			int n = 0;
			rstv_fastboot_until = fscanf(ff, "%d", &n) == 1 && n > 0 ? n : 1300;
			fclose(ff);
			RSTV_LOG("fastboot: to f%d (debug build)", rstv_fastboot_until);
		}
	}
#endif
	{ // diag: ux0:data/restv/pctrap = hex PC of a master block to log registers at (DynarecSh2.cpp)
		extern u32 rstv_dev_trap;
		FILE *tf = fopen(DATA_DIR "/pctrap", "r");
		if (tf) {
			unsigned v = 0, f0 = 0, f1 = 0;
			extern u32 rstv_dev_trace_from, rstv_dev_trace_to;
			const int n = fscanf(tf, "%x %u %u", &v, &f0, &f1);
			if (n >= 1)
				rstv_dev_trap = v;
			if (n == 3) {
				rstv_dev_trace_from = f0;
				rstv_dev_trace_to = f1;
				extern int rstv_chain_trace_on, rstv_chain_prof;
				rstv_chain_trace_on = 1;
				rstv_chain_prof = 1; // the chain variant with the trace hook
			}
			fclose(tf);
		}
	}
	// recompiler block chaining (ADR-018). Disabled if ux0:data/restv/nochain exists
	// SCU DMA completes instantly (ux0:data/restv/dmainstant, experimental)
	extern int rstv_dma_instant;
	rstv_dma_instant = file_exists(DATA_DIR "/dmainstant");
	extern int rstv_v1_force_cpu;
	rstv_v1_force_cpu = file_exists(DATA_DIR "/v1cpu"); // diag: VDP1 on the CPU (reference for the GPU renderer)
	{ // diag: ux0:data/restv/v1skip = bit mask of VDP1 GPU item kinds to leave out (1 tex, 2 poly, 4 erase, 8 line)
		extern int v1gpu_skip_kinds;
		FILE *kf = fopen(DATA_DIR "/v1skip", "r");
		if (kf) { if (fscanf(kf, "%d", &v1gpu_skip_kinds) != 1) v1gpu_skip_kinds = 0; fclose(kf); }
	}
	extern int rstv_chain_on;
	rstv_chain_on = !file_exists(DATA_DIR "/nochain");
	{
		// ADR-100: hi-res / double-density interlace screens at half size (GPU composition, 60fps). nohalf = full size (CPU)
		extern int rstv_hires_half;
		rstv_hires_half = !file_exists(DATA_DIR "/nohalf");
	}
	{
		extern int rstv_scsp_fm_batch;
		rstv_scsp_fm_batch = !file_exists(DATA_DIR "/nofmbatch"); // ADR-076
	}
	{
		extern int rstv_rbg_half;
		rstv_rbg_half = file_exists(DATA_DIR "/rbghalf") ? 1 : file_exists(DATA_DIR "/norbghalf") ? 2 : 0; // ADR-077
		extern int rstv_rbg_unrot_on;
		rstv_rbg_unrot_on = !file_exists(DATA_DIR "/norbgunrot"); // ADR-083
	}
	uint64_t t_init = rstv_time_us();
	{
		// ADR-095: loading screen (same bar as Reirem). The ROM loader reports progress per blob
		extern void (*rstv_load_progress)(int permille);
		const StvGameInfo *gi = STVFindGame(game);
		rstv_loading_title(gi && gi->name ? gi->name : game);
		rstv_load_progress = load_rom_progress;
		extern void (*rstv_load_tick)(void);
		rstv_load_tick = rstv_loading_tick;
		rstv_loading_progress(0);
	}
	const int init_rc = rstv_emu_init(ROM_DIR, game, use_dynarec, err, sizeof(err));
	{
		extern void (*rstv_load_progress)(int permille);
		rstv_load_progress = NULL;
		extern void (*rstv_load_tick)(void);
		rstv_load_tick = NULL;
		rstv_loading_progress(700); // the GPU set-up below fills the rest (ADR-191)
	}
	if (init_rc != 0) {
		rstv_loading_done();
		RSTV_LOG("init failed: %s", err);
		write_text_file(DATA_DIR "/last_error.txt", err, 0);
		fatal_screen(0xFF0000C0u); // red: init failed
		goto out;
	}
	RSTV_LOG("init %.2fs", (rstv_time_us() - t_init) / 1e6);
	// EEPROM (game settings, records): load ux0:data/restv/nvram/<set>.nv if present; save every 10 s when changed and on exit
	char nv_path[128];
	snprintf(nv_path, sizeof(nv_path), DATA_DIR "/nvram/%s.nv", game);
	sceIoMkdir(DATA_DIR "/nvram", 0777);
	RSTV_LOG("eeprom %s: %s", nv_path, eeprom_load(nv_path) == 0 ? "loaded" : "default");
	// disable idle loop hack (ux0:data/restv/noidle, debug)
	if (file_exists(DATA_DIR "/noidle"))
		STVSetIdleHacks(0);
	// open/close the JIT memory VM domain per block (ux0:data/restv/novmopen; default keeps it open, ADR-043)
	if (file_exists(DATA_DIR "/novmopen")) {
		extern int rstv_jit_keep_open;
		rstv_jit_keep_open = 0;
	}
	if (file_exists(DATA_DIR "/spike17"))
		s_spike_us = 17000;
	// ADR-175: RAX DSP block compiler, opt-in while it is being checked on the device (ux0:data/restv/raxjit)
	{ // ADR-177 RAX block compiler (on by default): ux0:data/restv/raxjit holds 0 = off, 1 = copy blocks only, 2 = on
		SceUID jf = sceIoOpen(DATA_DIR "/raxjit", SCE_O_RDONLY, 0);
		if (jf >= 0) {
			char c = 0;
			sceIoRead(jf, &c, 1);
			sceIoClose(jf);
			extern int rstv_rax_jit_on;
			rstv_rax_jit_on = c == '0' ? 0 : c == '1' ? 1 : 2; // 0 off, 1 copy only (check), 2 on
		}
	}
	{ // ADR-176 (check): ux0:data/restv/raxthr = 2 or 3 runs the RAX board on its own thread on that CPU
		SceUID tf = sceIoOpen(DATA_DIR "/raxthr", SCE_O_RDONLY, 0);
		if (tf >= 0) {
			char c = 0;
			sceIoRead(tf, &c, 1);
			sceIoClose(tf);
			extern int rstv_rax_thread_cpu;
			rstv_rax_thread_cpu = c - '0';
		}
	}
	if (file_exists(DATA_DIR "/raxbench")) { // ADR-175 diag: RAX handler timing
		extern void rstv_rax_bench(void);
		rstv_rax_bench();
	}
	// disable native execution of game C library routines (ux0:data/restv/nohle, ADR-045)
	if (file_exists(DATA_DIR "/nohle")) {
		extern int rstv_hle_on;
		rstv_hle_on = 0;
	}
	// disable idle PC sleep (ux0:data/restv/nosleep, ADR-050)
	if (file_exists(DATA_DIR "/nosleep")) {
		extern int rstv_sleep_max;
		rstv_sleep_max = 0;
	}
	// enable direct block linking (ux0:data/restv/link, ADR-048 experiment)
	if (file_exists(DATA_DIR "/link")) {
		extern int rstv_link_on;
		rstv_link_on = 1;
	}
	// disable interpreting new blocks for their first 2 runs (ux0:data/restv/notier, ADR-044)
	if (file_exists(DATA_DIR "/notier")) {
		extern int rstv_tier_n;
		rstv_tier_n = 0;
	}
	{
		// experiment: the number in ux0:data/restv/tier = times to interpret before compiling (default 2)
		FILE *tf = fopen(DATA_DIR "/tier", "r");
		int tn = 0;
		if (tf && fscanf(tf, "%d", &tn) == 1 && tn >= 0 && tn <= 250) {
			extern int rstv_tier_n;
			rstv_tier_n = tn;
			RSTV_LOG("tier: %d", tn);
		}
		if (tf)
			fclose(tf);
	}
	// disable the once-per-frame hack for per-line polling functions (ux0:data/restv/noonce, ADR-042)
	if (file_exists(DATA_DIR "/noonce")) {
		extern u32 rstv_once_fn;
		rstv_once_fn = 0;
	}
	// GPU compose (ADR-011). CPU compose only if ux0:data/restv/cpucompose exists
	{
		extern int rstv_probe_on;
		rstv_probe_on = file_exists(DATA_DIR "/probe"); // measurement: per-section times on spike lines
	}
	gvdp2_set_game(game);
	extern void (*gvdp2_progress)(int permille);
	gvdp2_progress = load_gpu_progress;
	gx_set_busy(rstv_loading_tick); // ADR-193: shader compiles keep the loading bar moving
	const int use_gpu = !file_exists(DATA_DIR "/cpucompose") && gvdp2_init() == 0;
	gx_set_busy(NULL);
	gvdp2_progress = NULL;
	rstv_loading_progress(1000);
	rstv_loading_done();

	// perf log: average and max every 60 frames, appended to file every 600 frames
	char perf_path[128];
	snprintf(perf_path, sizeof(perf_path), DATA_DIR "/perf_%s.txt", game);
	write_text_file(perf_path, jit_msg, 0);
	{
		char cl[256];
		snprintf(cl, sizeof(cl), "\nclock: arm %d bus %d gpu %d (measured MHz: start %d, pre-video %d, post-video %d, post-audio %d, now %d)",
			scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency(), mhz_start,
			mhz_pre_video, mhz_post_video, mhz_post_audio, rstv_clock_measure_arm_mhz());
		write_text_file(perf_path, cl, 1);
	}
	write_text_file(perf_path, use_dynarec ? "\nsh2: dynarec" : "\nsh2: interpreter", 1);
	write_text_file(perf_path, use_gpu ? ", compose: gpu" : ", compose: cpu", 1);
	write_text_file(perf_path, rstv_vdp1_thread ? ", vdp1: thread" : ", vdp1: main", 1);
	write_text_file(perf_path, rstv_scsp_lag ? ", sound: lag1" : ", sound: lockstep", 1);
	write_text_file(perf_path, rstv_deciline_merge ? ", sh2: merge9" : ", sh2: deciline", 1);
	write_text_file(perf_path, rstv_memfast ? ", mem: fast" : ", mem: c", 1);
	write_text_file(perf_path, rstv_chain_on ? ", chain: on" : ", chain: off", 1);
	write_text_file(perf_path, !rstv_prof_clock ? ", prof: off\n" : rstv_prof_fine ? ", prof: fine\n" : ", prof: coarse\n", 1);
	// FPS display at top left (disabled if ux0:data/restv/nofps exists)
	if (file_exists(DATA_DIR "/nofps"))
		g_set.fps = 0;
	double osd_fps = 0.0;
	unsigned auto_frame[64];
	uint8_t auto_bits[64], auto_p1[64];
	int n_auto = 0;
	unsigned mash_from = 0;
	// Test memory pin (ux0:data/restv/poke.txt: "addr value" in hex, one byte per line written every frame; e.g. thuntk lives
	// 0604E600 3 lets the MASH free run reach the end). Does nothing if the file is missing
	unsigned poke_addr[16];
	unsigned char poke_val[16];
	int n_poke = 0;
	{
		FILE *pf = fopen(DATA_DIR "/poke.txt", "r");
		unsigned pa, pv;
		while (pf && n_poke < 16 && fscanf(pf, "%x %x", &pa, &pv) == 2) {
			poke_addr[n_poke] = pa;
			poke_val[n_poke] = (unsigned char)pv;
			n_poke++;
		}
		if (pf)
			fclose(pf);
		if (n_poke)
			RSTV_LOG("poke: %d entries (first %08x = %02x)", n_poke, poke_addr[0], poke_val[0]);
	}
	{
		FILE *af = auto_input_open();
		unsigned fno;
		char what[16];
		while (af && n_auto < 64 && fscanf(af, "%u %15s", &fno, what) == 2) {
			auto_frame[n_auto] = fno;
			auto_bits[n_auto] = !strcmp(what, "coin") ? STV_SYS_COIN1 : !strcmp(what, "start") ? STV_SYS_START1 : 0;
			// lever and buttons (6 frames): up down left right b1 b2 b3 b4; character (= minigame) select etc.
			auto_p1[n_auto] = !strcmp(what, "up") ? STV_IN_UP : !strcmp(what, "down") ? STV_IN_DOWN : !strcmp(what, "left") ? STV_IN_LEFT :
				!strcmp(what, "right") ? STV_IN_RIGHT : !strcmp(what, "b1") ? STV_IN_B1 : !strcmp(what, "b2") ? STV_IN_B2 :
				!strcmp(what, "b3") ? STV_IN_B3 : !strcmp(what, "b4") ? STV_IN_B4 : 0;
			if (!strcmp(what, "mash")) // from that frame, random lever/button mashing + coin and start every 600 frames (to go through several minigames)
				mash_from = fno;
			else
				n_auto++;
		}
		auto_input_close(af);
	}
	// Save state: ux0:data/restv/state/<set>.yss. L+START saves immediately, holding R+START for 1 s loads.
	// Measurement: if ux0:data/restv/loadstate exists (content = frame number, after BIOS boot), auto-load at that frame and
	// skip auto input. If ux0:data/restv/savestate exists ("frame [name] frame [name] ...", up to 8),
	// save to state/<name>.yss (<set>.yss if no name) at each of those frames.
	char state_path[128], load_path[128]; // saves always go to <set>.yss, loads can take a name
	snprintf(state_path, sizeof(state_path), DATA_DIR "/state/%s.yss", game);
	snprintf(load_path, sizeof(load_path), "%s", state_path);
	sceIoMkdir(DATA_DIR "/state", 0777);
	unsigned auto_load = 0, auto_save[16] = { 0 }, n_save = 0;
	static char save_names[16][48];
	{
		FILE *sf = fopen(DATA_DIR "/loadstate", "r");
		if (sf) {
			// "frame [name]": with a name, state/<name>.yss (per-scene state)
			char nm[48];
			const int k = fscanf(sf, "%u %47s", &auto_load, nm);
			if (k < 1)
				auto_load = 900;
			if (k == 2)
				snprintf(load_path, sizeof(load_path), DATA_DIR "/state/%s.yss", nm);
			fclose(sf);
			// (load and auto input can be combined: run_perf only sends AUTO with STATE)
		}
		sf = fopen(DATA_DIR "/savestate", "r");
		if (sf) {
			char tok[48];
			while (fscanf(sf, "%47s", tok) == 1) {
				if (tok[0] >= '0' && tok[0] <= '9') {
					if (n_save >= 16)
						break; // drop excess entries (before: the 8th was saved to <set>.yss without a name and could overwrite the player's save)
					auto_save[n_save] = (unsigned)strtoul(tok, NULL, 10);
					snprintf(save_names[n_save], sizeof(save_names[0]), "%s", game);
					n_save++;
				} else if (n_save > 0)
					snprintf(save_names[n_save - 1], sizeof(save_names[0]), "%s", tok);
			}
			fclose(sf);
		}
	}
	// sampling profiler (only when ux0:data/restv/sampler exists)
	const int use_sampler = file_exists(DATA_DIR "/sampler");
	writer_start(); // exit request check (ADR-090)
	if (use_sampler) {
		rstv_sampler_start();
		s_spike_sampler = !file_exists(DATA_DIR "/sampwhole"); // sampwhole: whole windows only (late frames do not reset them)
	}
	uint64_t win_start = rstv_time_us(), emu_sum = 0, emu_max = 0, pres_sum = 0, pres_max = 0;
	uint32_t frame = 0;


	uint32_t prev_buttons = ~0u;
	for (;;) {
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		if ((pad.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START)) ==
			(SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START))
			break;
		if (frame == 1200 && file_exists(DATA_DIR "/menutest")) {
			sceIoRemove(DATA_DIR "/menutest");
			rstv_menu_selftest();
			RSTV_LOG("menu: selftest images written");
		}
		if (rstv_menu_should_open(pad.buttons)) {
			// ADR-094: in-game menu (SELECT+START). Emulation stops while the menu is open; the menu is drawn over the last frame
			RSTV_LOG("menu: open f%u", frame);
			for (int done = 0; !done;) {
				sceCtrlPeekBufferPositive(0, &pad, 1);
				const rstv_menu_result r = rstv_menu_update(pad.buttons);
				char sp[160];
				const int slot = rstv_menu_slot();
				switch (r) {
				case RSTV_MENU_CLOSE:
					done = 1;
					break;
				case RSTV_MENU_RESET:
					rstv_emu_reset();
					rstv_test_session = 0;
					rstv_osd_show("Game reset");
					done = 1;
					break;
				case RSTV_MENU_SAVE: {
					rstv_state_slot_path(sp, sizeof(sp), slot);
					const int rc = rstv_state_save(sp);
					rstv_osd_show(rc == 0 ? "Saved: slot %d" : "Save failed: slot %d", slot);
					break;
				}
				case RSTV_MENU_LOAD: {
					rstv_state_slot_path(sp, sizeof(sp), slot);
					const int rc = rstv_state_load(sp);
					rstv_osd_show(rc == 0 ? "Loaded: slot %d" : "Load failed: slot %d", slot);
					done = 1;
					break;
				}
				case RSTV_MENU_SELECT_GAME:
					s_quit_mode = 2;
					done = 1;
					break;
				case RSTV_MENU_EXIT:
					s_quit_mode = 1;
					done = 1;
					break;
				case RSTV_MENU_TEST:
					// owner: an action, not a toggle: every press is one TEST switch press (the board enters its test
					// menu on one press and leaves it on the next). L / R work as SERVICE / TEST until the next press
					s_test_frames = 12; // the TEST switch held for 12 frames
					rstv_test_session = !rstv_test_session;
					rstv_osd_show(rstv_test_session ? "TEST switch pressed - in the test menu: L = Service (select), R = Test (enter)"
						: "TEST switch pressed");
					done = 1;
					break;
				default:
					break;
				}
				if (s_exit_req) {
					s_quit_mode = 1;
					done = 1;
				}
				if (!done)
					rstv_menu_draw();
				rstv_osd_tick();
				rstv_video_present();
			}
			rstv_menu_hide();
			rstv_video_present();
			RSTV_LOG("menu: close (quit %d)", s_quit_mode);
			prev_buttons = pad.buttons;
			win_start = rstv_time_us();
			if (s_quit_mode)
				break;
			continue;
		}
		uint8_t p1, sys;
		map_input(&pad, &p1, &sys, frame);
		// auto input (measurement): press for 6 frames on each "frame coin|start" line of ux0:data/restv/autoinput.txt
		for (int a = 0; a < n_auto; a++)
			if (frame + 1 >= auto_frame[a] && frame + 1 < auto_frame[a] + 6) {
				sys |= auto_bits[a];
				p1 |= auto_p1[a];
			}
		if (mash_from && frame + 1 >= mash_from) {
			// deterministic random: new lever/button combo every 8 frames, coin (0-5) and start (30-35) every 600 frames
			static uint32_t rng = 0x12345678u, held;
			const unsigned t = frame + 1 - mash_from;
			if ((t & 7) == 0) {
				rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
				static const uint8_t dirs[5] = { 0, STV_IN_UP, STV_IN_DOWN, STV_IN_LEFT, STV_IN_RIGHT };
				held = dirs[rng % 5] | (((rng >> 8) & 1) ? STV_IN_B1 : 0) | (((rng >> 9) & 1) ? STV_IN_B2 : 0) |
					(((rng >> 10) & 3) == 0 ? STV_IN_B3 : 0);
			}
			p1 |= (uint8_t)held;
			if (t % 600 < 6)
				sys |= STV_SYS_COIN1;
			else if (t % 600 >= 30 && t % 600 < 36)
				sys |= STV_SYS_START1;
		}

		extern unsigned rstv_dyn_compile, rstv_dsp_insns, rstv_dma_units, rstv_div_count, rstv_comp_us[3];
		const unsigned sp_comp = rstv_dyn_compile, sp_dsp = rstv_dsp_insns, sp_dma = rstv_dma_units;
		const unsigned sp_c0 = rstv_comp_us[0], sp_c1 = rstv_comp_us[1], sp_c2 = rstv_comp_us[2];
		extern unsigned rstv_dyn_cyc[2], rstv_tier_runs;
		const unsigned sp_cm = rstv_dyn_cyc[0], sp_cs = rstv_dyn_cyc[1], sp_tr = rstv_tier_runs;
		{
			extern unsigned rstv_probe_us[12], rstv_probe_smpc_us;
			memset(rstv_probe_us, 0, sizeof(rstv_probe_us));
			rstv_probe_smpc_us = 0;
		}
		unsigned long long sp_pw, sp_pwait, sp_draw;
		gvdp2_peek_times(&sp_pw, &sp_pwait, &sp_draw);
		const u64 sp_v1t = rstv_prof_us[RSTV_PROF_VDP1_THREAD], sp_v1w = rstv_prof_us[RSTV_PROF_VDP1_WAIT];
		extern unsigned rstv_hle_runs, rstv_hle_bytes, rstv_hle_us[8];
		const unsigned sp_hr = rstv_hle_runs, sp_hb = rstv_hle_bytes;
		unsigned sp_hus = 0;
		for (int k = 0; k < 8; k++)
			sp_hus += rstv_hle_us[k];
		uint64_t t0 = rstv_time_us();
#ifdef RSTV_DEBUG_TOOLS
		rstv_fastboot_now = frame < rstv_fastboot_until;
#endif
		rstv_emu_run_frame(p1, 0, sys);
		for (int k = 0; k < n_poke; k++)
			MappedMemoryWriteByteNocache(poke_addr[k], poke_val[k]);
		rstv_act = 9;
		uint64_t dt = rstv_time_us() - t0;
		if (dt > s_spike_us) // late frame diagnostics (default 25ms, 17ms if ux0:data/restv/spike17)
			RSTV_LOG("spike f%u emu %.1fms comp %u (open %u emit %u sync %u us) dsp %u dma %u cyc %uk %uk tier %u", frame, dt / 1000.0,
				rstv_dyn_compile - sp_comp, rstv_comp_us[0] - sp_c0, rstv_comp_us[1] - sp_c1, rstv_comp_us[2] - sp_c2,
				rstv_dsp_insns - sp_dsp, rstv_dma_units - sp_dma, (rstv_dyn_cyc[0] - sp_cm) / 1000, (rstv_dyn_cyc[1] - sp_cs) / 1000,
				rstv_tier_runs - sp_tr);
		if (dt > s_spike_us) {
			unsigned hus = 0;
			for (int k = 0; k < 8; k++)
				hus += rstv_hle_us[k];
			extern u32 rstv_hle_big[4];
			{
				extern int rstv_probe_on;
				extern unsigned rstv_probe_us[12], rstv_probe_smpc_cmd, rstv_probe_smpc_us;
				unsigned long long pw, pwait, pdraw;
				gvdp2_peek_times(&pw, &pwait, &pdraw);
				RSTV_LOG("spike f%u video prep %llu wait %llu draw %llu us v1thr %u v1wait %u us", frame, pw - sp_pw, pwait - sp_pwait,
					pdraw - sp_draw, (unsigned)(rstv_prof_us[RSTV_PROF_VDP1_THREAD] - sp_v1t),
					(unsigned)(rstv_prof_us[RSTV_PROF_VDP1_WAIT] - sp_v1w));
				if (rstv_probe_on)
					RSTV_LOG("spike f%u probe msh2 %u ssh2 %u vbin %u vbout %u scu %u m68k %u smpc %u (cmd %02x %u) scsp %u hbin %u hbout %u cs2 %u us", frame,
						rstv_probe_us[0], rstv_probe_us[1], rstv_probe_us[2], rstv_probe_us[3], rstv_probe_us[4], rstv_probe_us[5],
						rstv_probe_us[6], rstv_probe_smpc_cmd, rstv_probe_smpc_us, rstv_probe_us[7], rstv_probe_us[8], rstv_probe_us[9],
						rstv_probe_us[10]);
			}
			if (s_spike_sampler) {
				// diagnostics: if the sampling profiler is on, top blocks of one late frame (cleared just before so only that frame shows)
				static char sl[2600];
				rstv_sampler_report(sl, sizeof(sl));
				RSTV_LOG("spike f%u %.900s", frame, sl);
			}
			RSTV_LOG("spike f%u hle runs %u bytes %u us %u (last big: kind %u dst %08x src %08x n %u)", frame, rstv_hle_runs - sp_hr,
				rstv_hle_bytes - sp_hb, hus - sp_hus, (unsigned)rstv_hle_big[0], (unsigned)rstv_hle_big[1], (unsigned)rstv_hle_big[2],
				(unsigned)rstv_hle_big[3]);
		}
		emu_sum += dt;
		if (dt > emu_max)
			emu_max = dt;
		{
			// save state (only between frames)
			// quick save SELECT+L, quick load SELECT+R (once on press, same as Reirem, ADR-095). File is state/<set>.yss
			const uint32_t qs = SCE_CTRL_SELECT | SCE_CTRL_LTRIGGER, ql = SCE_CTRL_SELECT | SCE_CTRL_RTRIGGER;
			int act = 0, quick = 0; // act 1 = save, 2 = load
			if ((pad.buttons & qs) == qs && (prev_buttons & qs) != qs)
				act = 1, quick = 1;
			else if ((pad.buttons & ql) == ql && (prev_buttons & ql) != ql)
				act = 2, quick = 1;
			const char *save_path = state_path;
			static char auto_path[128];
			for (unsigned k = 0; k < n_save; k++)
				if (frame + 1 == auto_save[k]) {
					act = 1;
					snprintf(auto_path, sizeof(auto_path), DATA_DIR "/state/%s.yss", save_names[k]);
					save_path = auto_path;
				}
			if (auto_load && frame + 1 == auto_load)
				act = 2;
			if (act == 2 && quick && !file_exists(state_path)) {
				rstv_osd_show("No state: quick");
				act = 0;
			}
			if (act) {
				const uint64_t ts = rstv_time_us();
				const char *sp = act == 1 ? save_path : (quick ? state_path : load_path);
				const int r = act == 1 ? rstv_state_save(sp) : rstv_state_load(sp);
				RSTV_LOG("state %s f%u %s: %d (%.1fms)", act == 1 ? "save" : "load", frame + 1, sp, r, (rstv_time_us() - ts) / 1000.0);
				if (quick)
					rstv_osd_show(act == 1 ? (r == 0 ? "Saved: quick" : "Save failed: quick") : (r == 0 ? "Loaded: quick" : "Load failed: quick"));
			}
			prev_buttons = pad.buttons;
		}

		uint64_t tp0 = rstv_time_us();
#ifdef RSTV_DEBUG_TOOLS
		if (!rstv_fastboot_now)
#endif
		rstv_video_present();
		uint64_t tp = rstv_time_us() - tp0;
		pres_sum += tp;
		if (tp > pres_max)
			pres_max = tp;
		// If the frame was late and did not wait on vsync, yield 1ms. When the emu thread held the CPU nonstop in slow games,
		// the shell's vitacompanion (FTP, commands) and the exit watcher thread starved and the Vita looked hung
		{
			static uint64_t s_last_present;
			const uint64_t now = rstv_time_us();
			// ADR-147: every 4th late frame only (1 ms a frame was 5% of a 48 fps game); the exit watcher is on CPU3 now
			static unsigned s_late;
			if (s_last_present && now - s_last_present > 17500 && tp < 1500 && (++s_late & 3) == 0)
				sceKernelDelayThread(1000);
			s_last_present = rstv_time_us();
		}

		++frame;
		{
			// debug: if ux0:data/restv/strace exists, dump the slave block address log to a file at frame 1500
			extern u32 rstv_strace[1024];
			extern unsigned rstv_strace_n;
			extern int rstv_strace_on;
			if (frame == 1)
				rstv_strace_on = file_exists(DATA_DIR "/strace");
			if (rstv_strace_on && frame == 1500) {
				FILE *tf = fopen(DATA_DIR "/strace.txt", "w");
				if (tf) {
					fprintf(tf, "n %u\n", rstv_strace_n);
					for (unsigned k = 0; k < 1024 && k < rstv_strace_n; k++)
						fprintf(tf, "%08x\n", (unsigned)rstv_strace[(rstv_strace_n - 1024 + k) & 1023]);
					extern u32 rstv_strace_regs[17], rstv_strace_mem[33];
					extern int rstv_strace_hit;
					fprintf(tf, "hit %d\n", rstv_strace_hit);
					for (int k = 0; k < 17; k++)
						fprintf(tf, "r%d %08x\n", k, (unsigned)rstv_strace_regs[k]);
					for (int k = 0; k < 33; k++)
						fprintf(tf, "m%d %08x\n", k, (unsigned)rstv_strace_mem[k]);
					fclose(tf);
				}
				rstv_strace_on = 0;
			}
		}
		// FPS (updated every second) + frame number; top-right box as in Reirem (ADR-094); expire notices
		rstv_fps_update((float)osd_fps, frame);
		rstv_osd_tick();
		if (s_exit_req) {
			// ADR-086: exit request from measurement tools; clean up and exit instead of a forced quit (checked by the writer thread, ADR-090)
			break;
		}
		if (frame % 60 == 0) {
			// prevent auto suspend and screen off (long unattended runs and measurements must not sleep)
			sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
			if (frame % 600 == 0 && eeprom_dirty()) {
				eeprom_save_async(nv_path);
				RSTV_LOG("eeprom save %s (writer thread)", nv_path);
			}
			sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_OFF);
			uint64_t now = rstv_time_us();
			char line[4096];
			int w, h;
			osd_fps = 60.0 * 1e6 / (double)(now - win_start);
			rstv_emu_screen_size(&w, &h);
			rstv_audio_stats as;
			rstv_audio_get_stats(&as);
			static uint32_t s_und_prev;
			const uint32_t und = as.underruns - s_und_prev;
			s_und_prev = as.underruns;
			static uint32_t s_in_prev, s_out_prev;
			const uint32_t ain = as.in_total - s_in_prev, aout = as.out_total - s_out_prev;
			s_in_prev = as.in_total;
			s_out_prev = as.out_total;
			snprintf(line, sizeof(line), "f%u emu %.2f/%.2fms fps %.2f %dx%d aud %d %dppm und %u ovf %u in %u out %u | msh2 %.1f ssh2 %.1f vdp %.1f (v1 %.1f v2 %.1f cmp %.1f) snd %.1f oth %.1f | sndthr %.1f (68k %.1f dsp %.1f slots %.1f) sndwait %.1f wall %.1f lat %.2f gap %.1f/%u v1thr %.1f v1wait %.1f | lt N0 %.2f N1 %.2f N2 %.2f N3 %.2f R0 %.2f\n",
				frame, emu_sum / 60 / 1000.0, emu_max / 1000.0, 60.0 * 1e6 / (double)(now - win_start), w, h, as.fill, as.drc_ppm,
				und, as.overflows, ain, aout,
				rstv_prof_us[RSTV_PROF_MSH2] / 60000.0, rstv_prof_us[RSTV_PROF_SSH2] / 60000.0,
				rstv_prof_us[RSTV_PROF_VDP] / 60000.0, rstv_prof_us[RSTV_PROF_VDP1] / 60000.0,
				rstv_prof_us[RSTV_PROF_VDP2L] / 60000.0, rstv_prof_us[RSTV_PROF_COMPOSE] / 60000.0,
				rstv_prof_us[RSTV_PROF_SOUND] / 60000.0, rstv_prof_us[RSTV_PROF_OTHER] / 60000.0,
				rstv_prof_us[RSTV_PROF_SND_THREAD] / 60000.0, rstv_prof_us[RSTV_PROF_SND_68K] / 60000.0,
				rstv_prof_us[RSTV_PROF_SND_DSP] / 60000.0, rstv_prof_us[RSTV_PROF_SND_SLOTS] / 60000.0,
				rstv_prof_us[RSTV_PROF_SND_WAIT] / 60000.0, rstv_snd_wall_us / 60000.0, rstv_snd_lat_us / 60000.0, rstv_snd_posts ? rstv_snd_gap_us / 1000.0 / rstv_snd_posts : 0.0, rstv_snd_posts,
				rstv_prof_us[RSTV_PROF_VDP1_THREAD] / 60000.0,
				rstv_prof_us[RSTV_PROF_VDP1_WAIT] / 60000.0, rstv_prof_us[RSTV_PROF_L_N0] / 60000.0,
				rstv_prof_us[RSTV_PROF_L_N1] / 60000.0, rstv_prof_us[RSTV_PROF_L_N2] / 60000.0, rstv_prof_us[RSTV_PROF_L_N3] / 60000.0,
				rstv_prof_us[RSTV_PROF_L_R0] / 60000.0);
			rstv_snd_wall_us = rstv_snd_lat_us = rstv_snd_gap_us = 0;
			rstv_snd_posts = 0;
			{ // ADR-173: batmanfr sound board DSP (time on the sound thread, instructions a frame)
				extern int rstv_rax_active;
				extern unsigned rstv_rax_insns;
				extern u64 rstv_rax_us, rstv_rax_cpu_us;
				extern unsigned rstv_rax_jit_gens, rstv_rax_jit_insns, rstv_rax_jit_blocks, rstv_rax_nl_insns;
				static unsigned s_rax_prev, s_raxj_prev, s_raxn_prev;
				size_t n = strlen(line);
				if (rstv_rax_active && n > 0 && line[n - 1] == '\n')
					snprintf(line + n - 1, sizeof(line) - n + 1, " | rax %.1fms cpu %.1fms %uk jit %uk nl %uk gen %u blk %u\n",
						rstv_rax_us / 60000.0, rstv_rax_cpu_us / 60000.0, (rstv_rax_insns - s_rax_prev) / 60000,
						(rstv_rax_jit_insns - s_raxj_prev) / 60000, (rstv_rax_nl_insns - s_raxn_prev) / 60000,
						rstv_rax_jit_gens, rstv_rax_jit_blocks);
				s_rax_prev = rstv_rax_insns;
				s_raxj_prev = rstv_rax_jit_insns;
				s_raxn_prev = rstv_rax_nl_insns;
				rstv_rax_us = 0;
				rstv_rax_cpu_us = 0;
			}
			{
				gvdp2_stats gs;
				gvdp2_get_stats(&gs);
				size_t n = strlen(line);
				if (n > 0 && line[n - 1] == '\n')
					snprintf(line + n - 1, sizeof(line) - n + 1, " | gpu %u cpu %u why %u | lyr gpu %u (quad %u) cpu %u (mask %x why %x rot %x) fastcmp %u var %u/%u (v%u %ui) qn %d %d %d %d try %d (b %.1f f %.1f c %.1f) rbg %u rvar %u flat %u two %u | core %.1f copy %.1f fill %.1f quad %.1f draw %.1f | present %.1f/%.1f gpu nbg %.1f cmp %.1f | qclr %d %d %d %d qhyb %d %d %d %d reuse %u %u %u %u %u samev %u %u %u %u %u gen %u pal %u plane %d%d%d%d/%u (%u %u %u %u) rows %u tq %.1f %.1f %.1f %.1f %.1f | rbgmap %dx%d cn %d two %d bmp %d\n",
						gs.gpu_frames, gs.cpu_frames, gs.last_fallback_reason, gs.gpu_layers, gs.quad_layers, gs.cpu_layers, gs.cpu_mask, gs.nbg_why, gs.rbg_why, gs.fast_frames, gs.var_frames, gs.nvar, gs.cmp_vi, gs.cmp_instr, gs.qn[0], gs.qn[1], gs.qn[2], gs.qn[3], gs.qtry[2], gs.t_qbuild / 60000.0, gs.t_qfill / 60000.0, gs.t_qcopy / 60000.0, gs.rbg_layers, gs.rbg_var_frames, gs.rbg_flat_frames, gs.rbg_two_maps,
						gs.t_core / 60000.0, gs.t_copy / 60000.0, gs.t_fill / 60000.0, gs.t_quad / 60000.0, gs.t_draw / 60000.0, pres_sum / 60000.0, pres_max / 1000.0, gs.t_gpu_nbg / 60000.0, gs.t_gpu / 60000.0, gs.qclr[0], gs.qclr[1], gs.qclr[2], gs.qclr[3], gs.qhyb[0], gs.qhyb[1], gs.qhyb[2], gs.qhyb[3], gs.lreuse[0], gs.lreuse[1], gs.lreuse[2], gs.lreuse[3], gs.lreuse[4], gs.lsamev[0], gs.lsamev[1], gs.lsamev[2], gs.lsamev[3], gs.lsamev[4], gs.lgen, gs.lpal, gs.qplane[0], gs.qplane[1], gs.qplane[2], gs.qplane[3], gs.plane_draws, gs.plane_why[0], gs.plane_why[1], gs.plane_why[2], gs.plane_why[3], gs.plane_rows, gs.t_qlayer[0] / 60000.0, gs.t_qlayer[1] / 60000.0, gs.t_qlayer[2] / 60000.0, gs.t_qlayer[3] / 60000.0, gs.t_qlayer[4] / 60000.0, rstv_rot_capture.mapw, rstv_rot_capture.maph, rstv_rot_capture.colornumber, rstv_rot_capture.two_maps, rstv_rot_capture.isbitmap);
					n = strlen(line);
					if (n > 0 && line[n - 1] == 10)
						snprintf(line + n - 1, sizeof(line) - n + 1, " | atlas %.1fms %u pages rbgprep %.1f | v1gpu %u (cpu %u why %u snap %u) verts %u gpu %.1f build %.1f dec %u slut %.1f | prep %.1f wait %.1f sync %u%c", gs.t_atlas / 60000.0, gs.atlas_pages / 60, gs.t_rbgprep / 60000.0, gs.v1_gpu, rstv_v1_cpu_frames, rstv_v1_why, gs.v1_snaps, gs.v1_verts / 60, gs.t_gpu_v1 / 60000.0, gs.t_v1build / 60000.0, gs.v1_decoded / 60, gs.t_slut / 60000.0, gs.t_prep_worker / 60000.0, gs.t_prep_wait / 60000.0, gs.prep_sync, 10);
					if (gs.t_gpu_nbg) {
						n = strlen(line);
						if (n > 0 && line[n - 1] == 10)
							snprintf(line + n - 1, sizeof(line) - n + 1, " (L %.1f %.1f %.1f %.1f %.1f)%c", gs.t_gpu_layer[0] / 60000.0, gs.t_gpu_layer[1] / 60000.0, gs.t_gpu_layer[2] / 60000.0, gs.t_gpu_layer[3] / 60000.0, gs.t_gpu_layer[4] / 60000.0, 10);
						// the perf line does not fit in one UDP line (512 chars): GPU timing summary sent separately (every 60 frames)
						RSTV_LOG("gpuw f%u v1 %.1f nbg %.1f cmp %.1f L %.1f %.1f %.1f %.1f %.1f verts %u qn %d %d %d %d", frame,
							gs.t_gpu_v1 / 60000.0, gs.t_gpu_nbg / 60000.0, gs.t_gpu / 60000.0, gs.t_gpu_layer[0] / 60000.0,
							gs.t_gpu_layer[1] / 60000.0, gs.t_gpu_layer[2] / 60000.0, gs.t_gpu_layer[3] / 60000.0, gs.t_gpu_layer[4] / 60000.0,
							gs.v1_verts / 60, gs.qn[0], gs.qn[1], gs.qn[2], gs.qn[3]);
					}
			}
			{
				// per VDP1 path (ms/frame, cmds/frame, area kpx/frame); path numbers in vidsoft.c rstv_v1s_name
				extern unsigned long long rstv_v1s_n[8], rstv_v1s_px[8], rstv_v1s_ns[8];
				size_t n = strlen(line);
				if (n > 0 && line[n - 1] == '\n') {
					n--;
					for (int k = 0; k < 8; k++)
						if (rstv_v1s_n[k])
							n += snprintf(line + n, sizeof(line) - n, " | v1[%d] %.2fms n%.1f %.1fk", k, rstv_v1s_ns[k] / 60e6,
								rstv_v1s_n[k] / 60.0, rstv_v1s_px[k] / 60000.0);
					snprintf(line + n, sizeof(line) - n, "\n");
				}
				{
					extern unsigned rstv_dyn_exec, rstv_dyn_compile, rstv_dyn_chain, rstv_dyn_cyc[2], rstv_hle_runs, rstv_hle_bytes;
					n = strlen(line);
					if (n > 0 && line[n - 1] == '\n')
						snprintf(line + n - 1, sizeof(line) - n + 1, " | dyn exec %u comp %u chain %u cyc %uk %uk hle %u/%uk\n", rstv_dyn_exec / 60, rstv_dyn_compile / 60, rstv_dyn_chain / 60,
							rstv_dyn_cyc[0] / 60000, rstv_dyn_cyc[1] / 60000, rstv_hle_runs, rstv_hle_bytes / 1024);
					{
						extern unsigned rstv_hle_kind[8], rstv_hle_fail[8], rstv_hle_us[8], rstv_nested_us, rstv_nested_n, rstv_icap_us, rstv_hle_items[8];
						n = strlen(line);
						if (n > 0 && line[n - 1] == 10) {
							n--;
							for (int k = 0; k < 8; k++) // k0 = 8 (SGL), k1 = 9 (thuntk) / 1, k2 = 10 (sanjeon) / 2: kind & 7
								n += snprintf(line + n, sizeof(line) - n, " k%d %u/%u %uus", k, rstv_hle_kind[k], rstv_hle_fail[k], rstv_hle_us[k]);
							extern unsigned rstv_hle_why;
							extern int rstv_hle_n;
							snprintf(line + n, sizeof(line) - n, " why %x hooks %d nest %u %.2fms icap %.2fms items %u %u rechk %u ndisp %u%c", rstv_hle_why, rstv_hle_n, rstv_nested_n / 60,
								rstv_nested_us / 60000.0, rstv_icap_us / 60000.0, rstv_hle_items[0] / 60, rstv_hle_items[3] / 60,
								rstv_hle_items[7] / 60, rstv_hle_items[6] / 60, 10);
							memset(rstv_hle_items, 0, sizeof(rstv_hle_items));
							rstv_nested_us = rstv_nested_n = rstv_icap_us = 0;
						}
						memset(rstv_hle_kind, 0, sizeof(rstv_hle_kind));
						memset(rstv_hle_fail, 0, sizeof(rstv_hle_fail));
						memset(rstv_hle_us, 0, sizeof(rstv_hle_us));
					}
					rstv_hle_runs = rstv_hle_bytes = 0;
					rstv_dyn_exec = rstv_dyn_compile = rstv_dyn_chain = 0;
					rstv_dyn_cyc[0] = rstv_dyn_cyc[1] = 0;
					{
						// diagnostics: layer config (default NBG1 = Titan 2; Titan number in ux0:data/restv/diaglayer: 3 NBG0 2 NBG1 1 NBG2 0 NBG3)
						extern RstvLayerCapture rstv_layer_capture[6];
						static int diag_l = -1;
						if (diag_l < 0) {
							diag_l = 2;
							FILE *df = fopen(DATA_DIR "/diaglayer", "r");
							if (df) {
								if (fscanf(df, "%d", &diag_l) != 1 || diag_l < 0 || diag_l > 4)
									diag_l = 2;
								fclose(df);
							}
						}
						const RstvLayerCapture *c1 = &rstv_layer_capture[diag_l];
						n = strlen(line);
						if (n > 0 && line[n - 1] == 10)
							snprintf(line + n - 1, sizeof(line) - n + 1, " | n1 cap %d ok %d cn %d cw %d pw %d pds %d zx %.2f win %d bmp %d rows %d sp %d%c",
								c1->captured, c1->ok, c1->colornumber, c1->cellw, c1->patternwh, c1->patterndatasize, (double)c1->zx,
								c1->has_win, c1->isbitmap, c1->nrows, c1->specialprimode, 10);
						n = strlen(line);
						if (n > 0 && line[n - 1] == 10) {
							n--;
							n += snprintf(line + n, sizeof(line) - n, " xm %d ym %d rows:", c1->xmask, c1->ymask);
							for (int ry = 0; ry < 240 && ry < c1->nrows; ry += 24)
								n += snprintf(line + n, sizeof(line) - n, " %d:%d/%d/%d/%.2f/%d", ry, c1->row[ry].x, c1->row[ry].lsx, c1->row[ry].y,
									(double)c1->row[ry].zx * 1000.0, c1->row[ry].nwin);
							snprintf(line + n, sizeof(line) - n, "%c", 10);
						}
					}
					extern unsigned rstv_dyn_mem[12];
					n = strlen(line);
					if (n > 0 && line[n - 1] == '\n') {
						n--;
						n += snprintf(line + n, sizeof(line) - n, " | mem");
						for (int k = 0; k < 12; k++)
							n += snprintf(line + n, sizeof(line) - n, " %u", rstv_dyn_mem[k] / 60);
						snprintf(line + n, sizeof(line) - n, "\n");
					}
					memset(rstv_dyn_mem, 0, sizeof(rstv_dyn_mem));
					extern unsigned rstv_dyn_blk_bytes, rstv_dyn_blk_n, rstv_dyn_blk_max;
					n = strlen(line);
					if (n > 0 && line[n - 1] == 10 && rstv_dyn_blk_n)
						snprintf(line + n - 1, sizeof(line) - n + 1, " | blocks %u avg %u max %u%c", rstv_dyn_blk_n,
							rstv_dyn_blk_bytes / rstv_dyn_blk_n, rstv_dyn_blk_max, 10);
					extern unsigned rstv_div_count;
					n = strlen(line);
					if (n > 0 && line[n - 1] == 10)
						snprintf(line + n - 1, sizeof(line) - n + 1, " | div %u%c", rstv_div_count / 60, 10);
					rstv_div_count = 0;
					extern unsigned rstv_dma_units;
					n = strlen(line);
					if (n > 0 && line[n - 1] == '\n')
						snprintf(line + n - 1, sizeof(line) - n + 1, " | dma %u\n", rstv_dma_units / 60);
					rstv_dma_units = 0;
					{
						// ADR-136: slave SH-2 thread (ms/frame): its busy time, main waiting for it, I/O lock waits; wakes/frame
						extern volatile int rstv_slave_on;
						extern unsigned rstv_slave_busy_us, rstv_slave_wait_us, rstv_slave_lockwait_us, rstv_slave_wakes;
						n = strlen(line);
						if (rstv_slave_on && n > 0 && line[n - 1] == '\n')
							snprintf(line + n - 1, sizeof(line) - n + 1, " | slv busy %.1f wait %.1f lock %.1f wakes %u\n", rstv_slave_busy_us / 60000.0,
								rstv_slave_wait_us / 60000.0, rstv_slave_lockwait_us / 60000.0, rstv_slave_wakes / 60);
						rstv_slave_busy_us = rstv_slave_wait_us = rstv_slave_lockwait_us = rstv_slave_wakes = 0;
					}
				}
				memset(rstv_v1s_n, 0, sizeof(rstv_v1s_n));
				memset(rstv_v1s_px, 0, sizeof(rstv_v1s_px));
				memset(rstv_v1s_ns, 0, sizeof(rstv_v1s_ns));
			}
			memset(rstv_prof_us, 0, sizeof(rstv_prof_us));
			{
				// end with a newline even if the line is cut at the buffer end (measurement end is judged by the log file line count)
				size_t n = strlen(line);
				if (n > 0 && line[n - 1] != 10) {
					if (n > sizeof(line) - 2)
						n = sizeof(line) - 2;
					line[n] = 10;
					line[n + 1] = 0;
				}
			}
			RSTV_LOG("%s", line);
			if (frame % 1800 == 0) {
				// ADR-086: free memory (leak check during runs, every 30 s)
				SceKernelFreeMemorySizeInfo fm;
				fm.size = sizeof(fm);
				if (sceKernelGetFreeMemorySize(&fm) == 0)
					RSTV_LOG("mem f%u: free user %u KB cdram %u KB phycont %u KB", frame, fm.size_user >> 10, fm.size_cdram >> 10,
						fm.size_phycont >> 10);
			}
			if (frame % 300 == 0) {
				append_text_async(perf_path, line);
				if (use_sampler) {
					static char sl[2600];
					rstv_sampler_report(sl, sizeof(sl));
					append_text_async(perf_path, sl);
				}
			}
			win_start = now;
			emu_sum = emu_max = 0;
			pres_sum = pres_max = 0;
		}
	}
	if (eeprom_dirty())
		eeprom_save(nv_path);
	RSTV_LOG("exit: clean shutdown at f%u", frame);
	rstv_emu_shutdown();
	rstv_jit_shutdown(); // ADR-086: close VM area, free code blocks
out:
	rstv_audio_fini();
	rstv_video_shutdown();
	rstv_log_shutdown();
	if (s_quit_mode == 2 || s_exit_next) // menu "Select game" / measuring tools: relaunch the app
		sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
	sceKernelExitProcess(0);
	return 0;
}
