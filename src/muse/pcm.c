/* SPDX-License-Identifier: MIT */
/* See pcm.h. */
#include "pcm.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

/* libasound's own values, which are ABI and have not moved in two decades. */
#define SND_PCM_STREAM_PLAYBACK       0
#define SND_PCM_FORMAT_S16_LE         2
#define SND_PCM_ACCESS_RW_INTERLEAVED 3

typedef struct snd_pcm snd_pcm_t;

static int         (*a_open)(snd_pcm_t **, const char *, int, int);
static int         (*a_set_params)(snd_pcm_t *, int, int, unsigned, unsigned,
                                   int, unsigned);
static long        (*a_writei)(snd_pcm_t *, const void *, unsigned long);
static int         (*a_recover)(snd_pcm_t *, int, int);
static int         (*a_drop)(snd_pcm_t *);
static int         (*a_prepare)(snd_pcm_t *);
static int         (*a_delay)(snd_pcm_t *, long *);
static int         (*a_close)(snd_pcm_t *);
static const char *(*a_strerror)(int);

static snd_pcm_t *g_pcm;
static char       g_err[128];

static bool bind(void)
{
	static void *h;

	if (h) return true;
	h = dlopen("libasound.so.2", RTLD_NOW);
	if (!h) { snprintf(g_err, sizeof g_err, "no libasound: %s", dlerror()); return false; }
	a_open       = dlsym(h, "snd_pcm_open");
	a_set_params = dlsym(h, "snd_pcm_set_params");
	a_writei     = dlsym(h, "snd_pcm_writei");
	a_recover    = dlsym(h, "snd_pcm_recover");
	a_drop       = dlsym(h, "snd_pcm_drop");
	a_prepare    = dlsym(h, "snd_pcm_prepare");
	a_delay      = dlsym(h, "snd_pcm_delay");
	a_close      = dlsym(h, "snd_pcm_close");
	a_strerror   = dlsym(h, "snd_strerror");
	if (!a_open || !a_set_params || !a_writei || !a_recover || !a_drop ||
	    !a_prepare || !a_delay || !a_close || !a_strerror) {
		snprintf(g_err, sizeof g_err, "libasound is missing a function");
		dlclose(h);
		h = NULL;
		return false;
	}
	return true;
}

bool pcm_open(const char *device)
{
	int r;

	if (!bind()) return false;
	pcm_close();
	r = a_open(&g_pcm, device, SND_PCM_STREAM_PLAYBACK, 0);
	if (r < 0) {
		snprintf(g_err, sizeof g_err, "open %s: %s", device, a_strerror(r));
		g_pcm = NULL;
		return false;
	}
	/* soft_resample 0: the decoder already delivers 48 kHz, and asking ALSA
	 * to resample again would be the awrate path this exists to avoid.
	 * 200 ms of latency: long enough that a busy frame in a running game does
	 * not starve it, short enough that pause and seek feel immediate. */
	r = a_set_params(g_pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
	                 2, 48000, 0, 200000);
	if (r < 0) {
		snprintf(g_err, sizeof g_err, "set params on %s: %s", device, a_strerror(r));
		pcm_close();
		return false;
	}
	return true;
}

void pcm_close(void)
{
	if (g_pcm) a_close(g_pcm);
	g_pcm = NULL;
}

bool pcm_write(const int16_t *frames, int n)
{
	while (g_pcm && n > 0) {
		long w = a_writei(g_pcm, frames, (unsigned long)n);

		if (w < 0) {
			/* An underrun is ordinary - a pause, a slow card read - and
			 * recover re-prepares the stream. Anything else is not. */
			if (a_recover(g_pcm, (int)w, 1) < 0) {
				snprintf(g_err, sizeof g_err, "write: %s", a_strerror((int)w));
				return false;
			}
			continue;
		}
		frames += w * 2;
		n -= (int)w;
	}
	return g_pcm != NULL;
}

void pcm_drop(void)
{
	if (!g_pcm) return;
	a_drop(g_pcm);
	a_prepare(g_pcm);
}

long pcm_queued(void)
{
	long d = 0;

	if (!g_pcm || a_delay(g_pcm, &d) < 0 || d < 0) return 0;
	return d;
}

const char *pcm_error(void) { return g_err; }
