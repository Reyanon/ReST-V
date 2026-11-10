// ReST-V kernel helper, user side (vita/kmod/restv_kctrl.c, ADR-195)
#ifndef RSTV_KCTRL_H
#define RSTV_KCTRL_H
unsigned int rstvKCtrlPsState(void); // bits 0-14 single PS presses, 15-29 double presses, 30 held
int rstvKCtrlGoHome(void);           // press PS for the system once (after the app lets go of its lock)
int rstvKCtrlInstallOnExit(void); // ADR-209: once the app has exited: ux0:data/restv/update/files -> the app, start it
// ADR-218: CPU3 for the app: on 1 hook, 0 release, -1 read only; out[4]: module NID, the check's first 8 bytes, 1 if
// hooked by this module. Returns 1 hooked, 0 not, 2 not the known code, 3 / 4 not found, < 0 error
int rstvKCtrlCpu3(int on, unsigned int *out);
#endif
