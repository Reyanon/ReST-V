/*  ReST-V: threads.h implementation (pthread). Shared by PC harness and Vita (VitaSDK pthread).
    Replaces YabaSanshiro thr-linux.cpp. Core-internal threads are off by default (usethreads=0).

    This file is part of ReST-V, based on Yabause. GPL-2.0-or-later.
*/
#include "core.h"
#include "threads.h"

#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#endif

#define MAX_THREADS (YAB_NUM_THREADS)

static pthread_t s_thread[MAX_THREADS];
static int s_running[MAX_THREADS];
static pthread_mutex_t s_sleep_mtx[MAX_THREADS];
static pthread_cond_t s_sleep_cond[MAX_THREADS];
static int s_inited;

static void init_once(void)
{
	if (s_inited)
		return;
	for (int i = 0; i < MAX_THREADS; i++) {
		pthread_mutex_init(&s_sleep_mtx[i], NULL);
		pthread_cond_init(&s_sleep_cond[i], NULL);
	}
	s_inited = 1;
}

// Vita: the main thread stays on CPU0, SCSP/68K on CPU1, video helpers on CPU2.
typedef struct {
	void *(*func)(void *);
	void *arg;
	unsigned int id;
} StartInfo;

static StartInfo s_start[MAX_THREADS];

static void *thread_entry(void *p)
{
	StartInfo *si = (StartInfo *)p;
#ifdef __vita__
	int mask = (si->id == YAB_THREAD_SCSP) ? SCE_KERNEL_CPU_MASK_USER_1 : SCE_KERNEL_CPU_MASK_USER_2;
	sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), mask);
#endif
	return si->func(si->arg);
}

int YabThreadStart(unsigned int id, void *(*func)(void *), void *arg)
{
	init_once();
	if (id >= MAX_THREADS || s_running[id])
		return -1;
	s_start[id].func = func;
	s_start[id].arg = arg;
	s_start[id].id = id;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 256 * 1024);
	int r = pthread_create(&s_thread[id], &attr, thread_entry, &s_start[id]);
	pthread_attr_destroy(&attr);
	if (r != 0)
		return -1;
	s_running[id] = 1;
	return 0;
}

void YabThreadWait(unsigned int id)
{
	if (id >= MAX_THREADS || !s_running[id])
		return;
	pthread_join(s_thread[id], NULL);
	s_running[id] = 0;
}

void YabThreadYield(void) { sched_yield(); }

void YabThreadSleep(void)
{
	init_once();
	pthread_t self = pthread_self();
	for (int i = 0; i < MAX_THREADS; i++) {
		if (s_running[i] && pthread_equal(s_thread[i], self)) {
			pthread_mutex_lock(&s_sleep_mtx[i]);
			pthread_cond_wait(&s_sleep_cond[i], &s_sleep_mtx[i]);
			pthread_mutex_unlock(&s_sleep_mtx[i]);
			return;
		}
	}
}

void YabThreadRemoteSleep(unsigned int id) { (void)id; }

void YabThreadWake(unsigned int id)
{
	init_once();
	if (id >= MAX_THREADS)
		return;
	pthread_mutex_lock(&s_sleep_mtx[id]);
	pthread_cond_signal(&s_sleep_cond[id]);
	pthread_mutex_unlock(&s_sleep_mtx[id]);
}

typedef struct {
	pthread_mutex_t mtx;
	pthread_cond_t cond_full, cond_empty;
	int *buf;
	int size, head, count;
} RstvQueue;

YabEventQueue *YabThreadCreateQueue(int qsize)
{
	RstvQueue *q = calloc(1, sizeof(*q));
	q->buf = calloc(qsize, sizeof(int));
	q->size = qsize;
	pthread_mutex_init(&q->mtx, NULL);
	pthread_cond_init(&q->cond_full, NULL);
	pthread_cond_init(&q->cond_empty, NULL);
	return (YabEventQueue *)q;
}

void YabThreadDestoryQueue(YabEventQueue *queue_t)
{
	RstvQueue *q = (RstvQueue *)queue_t;
	if (!q)
		return;
	pthread_mutex_destroy(&q->mtx);
	pthread_cond_destroy(&q->cond_full);
	pthread_cond_destroy(&q->cond_empty);
	free(q->buf);
	free(q);
}

void YabAddEventQueue(YabEventQueue *queue_t, int evcode)
{
	RstvQueue *q = (RstvQueue *)queue_t;
	pthread_mutex_lock(&q->mtx);
	while (q->count == q->size)
		pthread_cond_wait(&q->cond_full, &q->mtx);
	q->buf[(q->head + q->count) % q->size] = evcode;
	q->count++;
	pthread_cond_signal(&q->cond_empty);
	pthread_mutex_unlock(&q->mtx);
}

int YabWaitEventQueue(YabEventQueue *queue_t)
{
	RstvQueue *q = (RstvQueue *)queue_t;
	pthread_mutex_lock(&q->mtx);
	while (q->count == 0)
		pthread_cond_wait(&q->cond_empty, &q->mtx);
	int v = q->buf[q->head];
	q->head = (q->head + 1) % q->size;
	q->count--;
	pthread_cond_signal(&q->cond_full);
	pthread_mutex_unlock(&q->mtx);
	return v;
}

int YaGetQueueSize(YabEventQueue *queue_t)
{
	RstvQueue *q = (RstvQueue *)queue_t;
	pthread_mutex_lock(&q->mtx);
	int n = q->count;
	pthread_mutex_unlock(&q->mtx);
	return n;
}

int YabClearEventQueue(YabEventQueue *queue_t)
{
	RstvQueue *q = (RstvQueue *)queue_t;
	pthread_mutex_lock(&q->mtx);
	q->count = 0;
	q->head = 0;
	pthread_cond_broadcast(&q->cond_full);
	pthread_mutex_unlock(&q->mtx);
	return 0;
}

void YabThreadLock(YabMutex *mtx) { pthread_mutex_lock((pthread_mutex_t *)mtx); }
void YabThreadUnLock(YabMutex *mtx) { pthread_mutex_unlock((pthread_mutex_t *)mtx); }

YabMutex *YabThreadCreateMutex(void)
{
	pthread_mutex_t *m = malloc(sizeof(*m));
	pthread_mutex_init(m, NULL);
	return (YabMutex *)m;
}

void YabThreadFreeMutex(YabMutex *mtx)
{
	if (!mtx)
		return;
	pthread_mutex_destroy((pthread_mutex_t *)mtx);
	free(mtx);
}

void YabThreadSetCurrentThreadAffinityMask(int mask) { (void)mask; }
int YabThreadGetCurrentThreadAffinityMask(void) { return 0; }

void YabThreadUSleep(unsigned int stime) { usleep(stime); }
