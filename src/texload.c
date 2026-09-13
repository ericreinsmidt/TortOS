/* SPDX-License-Identifier: MIT */
/* See texload.h for why this exists. */
#include <stdio.h>
#include <string.h>
#include <SDL.h>
#include <SDL_image.h>
#include "texload.h"

/* Slots each way. Comfortably more than the seven cards a shelf draws, so a
 * fast scroll queues ahead without the ring filling, and small enough that a
 * shelf abandoned mid-scroll leaves a few decodes to throw away rather than
 * hundreds. */
#define QN    24
/* LIB_PATH * 3, the size main.c builds these paths in, without taking a
 * dependency on library.h for one number. */
#define PATHN 1664

typedef struct {
	int      sys, idx;
	unsigned gen;
	char     first[PATHN], second[PATHN];
} req;

typedef struct {
	int          sys, idx;
	unsigned     gen;
	SDL_Surface *surf;
} res;

static SDL_Thread *g_thread;
static SDL_mutex  *g_lock;
static SDL_cond   *g_wake;
static bool        g_running;

static req      g_rq[QN];
static int      g_rq_head, g_rq_n;
static res      g_rs[QN];
static int      g_rs_head, g_rs_n;
static unsigned g_gen = 1;
/* What the worker holds right now. Without it, a card asked for every frame
 * would be queued again the moment it left the request ring and before its
 * result arrived - the one window where it is in neither. */
static int      g_cur_sys = -1, g_cur_idx = -1;

/* Called with the lock held. */
static bool queued(int sys, int idx)
{
	int k;

	if (sys == g_cur_sys && idx == g_cur_idx) return true;
	for (k = 0; k < g_rq_n; k++) {
		const req *q = &g_rq[(g_rq_head + k) % QN];
		if (q->sys == sys && q->idx == idx) return true;
	}
	for (k = 0; k < g_rs_n; k++) {
		const res *r = &g_rs[(g_rs_head + k) % QN];
		if (r->sys == sys && r->idx == idx) return true;
	}
	return false;
}

/* Worker side. Touches no renderer and no launcher state - only its own copy
 * of the job, which is why the paths are copied into the request rather than
 * pointed at: the list they came from can be rebuilt while this runs. */
static SDL_Surface *decode(const char *path)
{
	SDL_Surface *raw, *conv;

	if (!path || !*path) return NULL;
	if (!(raw = IMG_Load(path))) return NULL;
	conv = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_ARGB8888, 0);
	SDL_FreeSurface(raw);
	return conv;
}

static int worker(void *unused)
{
	(void)unused;
	for (;;) {
		req          job;
		SDL_Surface *s;

		SDL_LockMutex(g_lock);
		while (g_running && g_rq_n == 0) SDL_CondWait(g_wake, g_lock);
		if (!g_running) { SDL_UnlockMutex(g_lock); break; }
		job = g_rq[g_rq_head];
		g_rq_head = (g_rq_head + 1) % QN;
		g_rq_n--;
		g_cur_sys = job.sys;
		g_cur_idx = job.idx;
		SDL_UnlockMutex(g_lock);

		s = decode(job.first);
		if (!s) s = decode(job.second);

		SDL_LockMutex(g_lock);
		g_cur_sys = g_cur_idx = -1;
		/* A result whose generation moved on is for a shelf that no longer
		 * exists in that shape, and installing it would put one game's art
		 * on another. */
		if (job.gen == g_gen && g_rs_n < QN) {
			res *r = &g_rs[(g_rs_head + g_rs_n) % QN];
			r->sys = job.sys;
			r->idx = job.idx;
			r->gen = job.gen;
			r->surf = s;
			g_rs_n++;
		} else if (s) {
			SDL_FreeSurface(s);
		}
		SDL_UnlockMutex(g_lock);
	}
	return 0;
}

bool texload_start(void)
{
	if (g_thread) return true;
	if (!(g_lock = SDL_CreateMutex())) return false;
	if (!(g_wake = SDL_CreateCond())) {
		SDL_DestroyMutex(g_lock);
		g_lock = NULL;
		return false;
	}
	g_running = true;
	g_thread = SDL_CreateThread(worker, "tortos-texload", NULL);
	if (!g_thread) {
		g_running = false;
		SDL_DestroyCond(g_wake);   g_wake = NULL;
		SDL_DestroyMutex(g_lock);  g_lock = NULL;
		fprintf(stderr, "texload: no worker (%s); art decodes on the frame\n",
		        SDL_GetError());
		return false;
	}
	return true;
}

void texload_stop(void)
{
	if (!g_thread) return;
	SDL_LockMutex(g_lock);
	g_running = false;
	SDL_CondSignal(g_wake);
	SDL_UnlockMutex(g_lock);
	SDL_WaitThread(g_thread, NULL);
	g_thread = NULL;
	texload_bump();              /* frees whatever was still waiting */
	SDL_DestroyCond(g_wake);     g_wake = NULL;
	SDL_DestroyMutex(g_lock);    g_lock = NULL;
}

void texload_bump(void)
{
	int k;

	if (!g_lock) return;
	SDL_LockMutex(g_lock);
	g_gen++;
	/* Queued work is dropped outright. Work already in flight is left to
	 * finish and is thrown away when it lands, because its generation will
	 * no longer match - stopping it mid-decode would cost more than letting
	 * one image finish into the bin. */
	g_rq_n = g_rq_head = 0;
	for (k = 0; k < g_rs_n; k++) {
		res *r = &g_rs[(g_rs_head + k) % QN];
		if (r->surf) SDL_FreeSurface(r->surf);
	}
	g_rs_n = g_rs_head = 0;
	SDL_UnlockMutex(g_lock);
}

bool texload_want(int sys, int idx, const char *first, const char *second)
{
	req *q;

	if (!g_thread) return false;
	SDL_LockMutex(g_lock);
	if (g_rq_n >= QN || queued(sys, idx)) {
		SDL_UnlockMutex(g_lock);
		return true;
	}
	q = &g_rq[(g_rq_head + g_rq_n) % QN];
	q->sys = sys;
	q->idx = idx;
	q->gen = g_gen;
	snprintf(q->first,  sizeof q->first,  "%s", first  ? first  : "");
	snprintf(q->second, sizeof q->second, "%s", second ? second : "");
	g_rq_n++;
	SDL_CondSignal(g_wake);
	SDL_UnlockMutex(g_lock);
	return true;
}

bool texload_take(int *sys, int *idx, SDL_Surface **surf)
{
	res *r;

	if (!g_thread) return false;
	SDL_LockMutex(g_lock);
	if (g_rs_n == 0) {
		SDL_UnlockMutex(g_lock);
		return false;
	}
	r = &g_rs[g_rs_head];
	*sys  = r->sys;
	*idx  = r->idx;
	*surf = r->surf;
	g_rs_head = (g_rs_head + 1) % QN;
	g_rs_n--;
	SDL_UnlockMutex(g_lock);
	return true;
}
