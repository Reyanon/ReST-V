// Built-in UDP logger. No dependency on plugins (PrincessLog etc.).
#ifdef RSTV_ENABLE_LOG

#include "platform/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include "platform/timer.h"

#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/stat.h>

#ifndef RSTV_LOG_HOST
#define RSTV_LOG_HOST "255.255.255.255"
#endif

#define NET_MEM_SIZE (128 * 1024)
#define LINE_MAX_LEN 512

static char s_net_mem[NET_MEM_SIZE] __attribute__((aligned(64)));
static int s_sock = -1;
static int s_ctl_inited;
static int s_net_inited;
static SceNetSockaddrIn s_addr;

// diag: with ux0:data/restv/logfile every line also goes to ux0:data/restv/diag.txt (no rate limit)
static SceUID s_diag_fd = -1, s_diag_mtx = -1;
static char s_diag_buf[32 * 1024];
static unsigned s_diag_len;

/* diag file: write what is buffered (frame loop, exit) */
void rstv_log_flush(void)
{
	if (s_diag_fd < 0 || !s_diag_len)
		return;
	sceKernelLockMutex(s_diag_mtx, 1, NULL);
	sceIoWrite(s_diag_fd, s_diag_buf, s_diag_len);
	s_diag_len = 0;
	sceKernelUnlockMutex(s_diag_mtx, 1);
}

void rstv_log_init(void)
{
	if (s_diag_fd < 0) {
		SceIoStat st;
		if (sceIoGetstat("ux0:data/restv/logfile", &st) >= 0) {
			s_diag_mtx = sceKernelCreateMutex("rstv_diaglog", 0, 0, NULL);
			if (s_diag_mtx >= 0)
				s_diag_fd = sceIoOpen("ux0:data/restv/diag.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777); // appended: the relaunch into the game list opens it again
		}
	}
	if (s_sock >= 0)
		return;
	// ADR-087: send only to the PC address in ux0:data/restv/loghost (unicast). Logging off if missing.
	// Used to broadcast to 255.255.255.255; Wi-Fi broadcast goes at the lowest rate with no retransmit, so tens to a hundred lines/s in heavy scenes
	// ate the wireless bandwidth, slowed Vita responses and stalled vitacompanion FTP
	char host[64] = { 0 };
	{
		SceUID fd = sceIoOpen("ux0:data/restv/loghost", SCE_O_RDONLY, 0);
		if (fd < 0)
			return;
		int n = sceIoRead(fd, host, sizeof(host) - 1);
		sceIoClose(fd);
		if (n <= 0)
			return;
		host[n] = 0;
		for (int i = 0; host[i]; i++)
			if (host[i] == '\r' || host[i] == '\n' || host[i] == ' ') {
				host[i] = 0;
				break;
			}
	}

	if (sceSysmoduleIsLoaded(SCE_SYSMODULE_NET) != SCE_SYSMODULE_LOADED)
		sceSysmoduleLoadModule(SCE_SYSMODULE_NET);

	SceNetInitParam param;
	param.memory = s_net_mem;
	param.size = sizeof(s_net_mem);
	param.flags = 0;
	int r = sceNetInit(&param);
	if (r < 0 && r != (int)SCE_NET_ERROR_EBUSY)
		return; // network unavailable: quietly disable logging
	s_net_inited = (r >= 0);
	s_ctl_inited = sceNetCtlInit() >= 0;

	s_sock = sceNetSocket("rstv_log", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, SCE_NET_IPPROTO_UDP);
	if (s_sock < 0)
		return;

	int on = 1;
	sceNetSetsockopt(s_sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_BROADCAST, &on, sizeof(on));

	memset(&s_addr, 0, sizeof(s_addr));
	s_addr.sin_len = sizeof(s_addr);
	s_addr.sin_family = SCE_NET_AF_INET;
	s_addr.sin_port = sceNetHtons(RSTV_LOG_PORT);
	if (sceNetInetPton(SCE_NET_AF_INET, host, &s_addr.sin_addr) <= 0) {
		sceNetSocketClose(s_sock);
		s_sock = -1;
		return;
	}

	rstv_log_printf("log: start -> %s:%d (unicast, at most %d lines a second)", host, RSTV_LOG_PORT, 30);
}

void rstv_log_shutdown(void)
{
	rstv_log_flush();
	if (s_sock >= 0) {
		sceNetSocketClose(s_sock);
		s_sock = -1;
	}
	if (s_ctl_inited) { // only after sceNetCtlInit: without loghost the NetCtl module is never loaded (null import)
		sceNetCtlTerm();
		s_ctl_inited = 0;
	}
	if (s_net_inited) {
		sceNetTerm();
		s_net_inited = 0;
	}
}

// safe to call from multiple threads (line buffer on the stack, one sendto per line)
void rstv_log_printf(const char *fmt, ...)
{
	if (s_sock < 0)
		return;

	char line[LINE_MAX_LEN];
	uint64_t t = rstv_time_us();
	int n = snprintf(line, sizeof(line), "[%7u.%03u] ",
		(unsigned)(t / 1000000), (unsigned)(t / 1000 % 1000));

	va_list ap;
	va_start(ap, fmt);
	int m = vsnprintf(line + n, sizeof(line) - n - 1, fmt, ap);
	va_end(ap);
	if (m < 0)
		return;
	n += m;
	if (n > (int)sizeof(line) - 2)
		n = sizeof(line) - 2;
	line[n++] = '\n';

	if (s_diag_fd >= 0) { // diag file (rstv_log_init): kept in memory, written 32KB at a time (a write per line slowed loading 4x)
		sceKernelLockMutex(s_diag_mtx, 1, NULL);
		if (s_diag_len + (unsigned)n > sizeof(s_diag_buf)) {
			sceIoWrite(s_diag_fd, s_diag_buf, s_diag_len);
			s_diag_len = 0;
		}
		memcpy(s_diag_buf + s_diag_len, line, (size_t)n);
		s_diag_len += (unsigned)n;
		sceKernelUnlockMutex(s_diag_mtx, 1);
	}
	// ADR-087: up to 30 lines/s (dropped lines are counted and reported on the next line). Called from several threads, so the count is approximate
	static volatile uint32_t s_sec, s_cnt, s_drop;
	const uint32_t sec = (uint32_t)(t / 1000000);
	if (sec != s_sec) {
		s_sec = sec;
		s_cnt = 0;
	}
	if (++s_cnt > 30) {
		s_drop++;
		return;
	}
	if (s_drop) {
		char d[64];
		const int dn = snprintf(d, sizeof(d), "[%7u.%03u] log: %u lines dropped\n", (unsigned)(t / 1000000),
			(unsigned)(t / 1000 % 1000), (unsigned)s_drop);
		s_drop = 0;
		sceNetSendto(s_sock, d, dn, 0, (SceNetSockaddr *)&s_addr, sizeof(s_addr));
	}
	sceNetSendto(s_sock, line, n, 0, (SceNetSockaddr *)&s_addr, sizeof(s_addr));
}

#else
// (logging off: the core's diagnostic lines call this directly)
void rstv_log_printf(const char *fmt, ...) { (void)fmt; }
#endif // RSTV_ENABLE_LOG
