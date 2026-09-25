/* SPDX-License-Identifier: MIT */
/* See btvol.h. alsa-lib's simple mixer against bluealsa's control plugin,
 * reached with dlopen like Muse reaches ALSA: the launcher links no libasound,
 * and forking amixer out of this process is what menu_wifi exists to avoid. */
#include "btvol.h"

#ifdef __linux__
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

static int  (*m_open)(void **, int);
static int  (*m_attach)(void *, const char *);
static int  (*m_register)(void *, void *, void *);
static int  (*m_load)(void *);
static int  (*m_close)(void *);
static void *(*m_first)(void *);
static void *(*m_next)(void *);
static const char *(*m_name)(void *);
static int  (*m_has_volume)(void *);
static int  (*m_set_all)(void *, long);

static bool bind(void)
{
	static int state;          /* 0 not tried, 1 bound, -1 failed */
	void *lib;

	if (state) return state > 0;
	state = -1;
	if (!(lib = dlopen("libasound.so.2", RTLD_NOW))) return false;
#define SYM(v, n) if (!(*(void **)&v = dlsym(lib, n))) return false
	SYM(m_open,       "snd_mixer_open");
	SYM(m_attach,     "snd_mixer_attach");
	SYM(m_register,   "snd_mixer_selem_register");
	SYM(m_load,       "snd_mixer_load");
	SYM(m_close,      "snd_mixer_close");
	SYM(m_first,      "snd_mixer_first_elem");
	SYM(m_next,       "snd_mixer_elem_next");
	SYM(m_name,       "snd_mixer_selem_get_name");
	SYM(m_has_volume, "snd_mixer_selem_has_playback_volume");
	SYM(m_set_all,    "snd_mixer_selem_set_playback_volume_all");
#undef SYM
	state = 1;
	return true;
}

bool btvol_set(int level)
{
	static const char tail[] = " - A2DP";
	void *mixer = NULL, *e;
	bool done = false;

	if (level < 0) level = 0;
	if (level > 127) level = 127;
	if (!bind() || m_open(&mixer, 0) < 0) return false;
	if (m_attach(mixer, "bluealsa") < 0 || m_register(mixer, NULL, NULL) < 0 ||
	    m_load(mixer) < 0) {
		m_close(mixer);
		return false;
	}
	/* The A2DP control of whichever headset is connected. bluealsa names it
	 * after the device, so it is found by its tail rather than by a name
	 * compiled in; the SCO and battery controls beside it end differently. */
	for (e = m_first(mixer); e; e = m_next(e)) {
		const char *name = m_name(e);
		size_t n = name ? strlen(name) : 0;

		if (n < sizeof tail - 1 || strcmp(name + n - (sizeof tail - 1), tail))
			continue;
		if (!m_has_volume(e)) continue;
		done = m_set_all(e, level) >= 0;
		break;
	}
	m_close(mixer);
	return done;
}

#else   /* not __linux__ */

bool btvol_set(int level) { (void)level; return false; }

#endif
