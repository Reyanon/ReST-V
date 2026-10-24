// ReST-V kernel helper (ADR-195): the PS button for the app. The user side cannot see the PS button while the shell
// lock holds it (no sceCtrlPeekBuffer* reported it on the device), and a kernel read made inside the app's syscall
// only saw a long press, so a kernel thread of this module reads the pad every 8 ms, counts presses and double presses
// (owner: home only on a double press), and on request presses PS for the system (button emulation) to go home.
// The module name carries a version (restv_kctrl_v4): a loaded module stays until reboot, and a new export under an
// old name would never be linked.
#include <psp2kern/ctrl.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/threadmgr.h>

#define POLL_US 8000
#define DOUBLE_POLLS 50 // a second press within 400 ms is a double press
#define QUIET_POLLS 40  // after pressing PS for the system: its own press is not counted

static volatile unsigned int s_presses, s_doubles;
static volatile int s_held, s_run, s_home;
static SceUID s_thread = -1;

static int poll_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	int held = 0, since = 1 << 20, quiet = 0;
	while (s_run) {
		if (s_home) {
			s_home = 0;
			ksceCtrlSetButtonEmulation(0, 0, 0, SCE_CTRL_PSBUTTON, 8);
			quiet = QUIET_POLLS;
		}
		SceCtrlData d;
		d.buttons = 0;
		if (ksceCtrlPeekBufferPositive(0, &d, 1) >= 0) {
			const int ps = (d.buttons & SCE_CTRL_PSBUTTON) != 0;
			if (ps && !held && !quiet) {
				if (since < DOUBLE_POLLS) {
					s_doubles++;
					since = 1 << 20; // (a third press starts over)
				} else {
					s_presses++;
					since = 0;
				}
			}
			held = ps;
			s_held = ps;
		}
		if (since < (1 << 20))
			since++;
		if (quiet)
			quiet--;
		ksceKernelDelayThread(POLL_US);
	}
	return 0;
}

// PS button: bits 0-14 single presses, 15-29 double presses (both counted since the module started), bit 30 held now
unsigned int rstvKCtrlPsState(void)
{
	int state;
	ENTER_SYSCALL(state);
	const unsigned int c = (s_presses & 0x7FFF) | ((s_doubles & 0x7FFF) << 15) | (s_held ? 1u << 30 : 0u);
	EXIT_SYSCALL(state);
	return c;
}

// press PS for the system once (the app has let go of its PS lock first): the home screen
int rstvKCtrlGoHome(void)
{
	int state;
	ENTER_SYSCALL(state);
	s_home = 1;
	EXIT_SYSCALL(state);
	return 0;
}

int module_start(SceSize args, void *argp);
int _start(SceSize args, void *argp) __attribute__((weak, alias("module_start")));
int module_start(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	s_run = 1;
	s_thread = ksceKernelCreateThread("rstv_kctrl", poll_thread, 0x3C, 0x1000, 0, 0, NULL);
	if (s_thread >= 0)
		ksceKernelStartThread(s_thread, 0, NULL);
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	s_run = 0;
	if (s_thread >= 0) {
		ksceKernelWaitThreadEnd(s_thread, NULL, NULL);
		ksceKernelDeleteThread(s_thread);
	}
	return SCE_KERNEL_STOP_SUCCESS;
}
