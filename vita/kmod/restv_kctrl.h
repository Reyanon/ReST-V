// ReST-V kernel helper, user side (vita/kmod/restv_kctrl.c, ADR-195)
#ifndef RSTV_KCTRL_H
#define RSTV_KCTRL_H
unsigned int rstvKCtrlPsState(void); // bits 0-14 single PS presses, 15-29 double presses, 30 held
int rstvKCtrlGoHome(void);           // press PS for the system once (after the app lets go of its lock)
int rstvKCtrlInstallOnExit(void); // ADR-209: once the app has exited: ux0:data/restv/update/files -> the app, start it
#endif
