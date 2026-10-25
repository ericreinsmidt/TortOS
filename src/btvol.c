/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See btvol.h. alsa-lib's simple mixer against bluealsa's control plugin:
 * forking amixer out of this process is what menu_wifi exists to avoid. */
#include "btvol.h"

#ifdef __linux__
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <string.h>

bool btvol_set(const char *sink, int level)
{
	char card[48];
	snd_mixer_t *mixer = NULL;
	snd_mixer_elem_t *e;
	bool done = false;
	int i;

	/* bt_C0_86_B3_A7_48_7F back to C0:86:B3:A7:48:7F. */
	if (!sink || strncmp(sink, "bt_", 3) || strlen(sink) != 3 + 17) return false;
	snprintf(card, sizeof card, "bluealsa:DEV=%s", sink + 3);
	for (i = 13 + 2; card[i]; i += 3) card[i] = ':';
	if (level < 0) level = 0;
	if (level > 127) level = 127;
	if (snd_mixer_open(&mixer, 0) < 0) return false;
	/* One device's controls, by its address. The whole `bluealsa` card lists
	 * every connected headset's, and with two connected the first one's was
	 * set - not necessarily the one being heard. Scoped, the control is named
	 * plainly `A2DP`, and a device that is not connected is refused. */
	if (snd_mixer_attach(mixer, card) < 0 ||
	    snd_mixer_selem_register(mixer, NULL, NULL) < 0 ||
	    snd_mixer_load(mixer) < 0) {
		snd_mixer_close(mixer);
		return false;
	}
	for (e = snd_mixer_first_elem(mixer); e; e = snd_mixer_elem_next(e)) {
		const char *name = snd_mixer_selem_get_name(e);

		if (!name || strcmp(name, "A2DP") ||
		    !snd_mixer_selem_has_playback_volume(e))
			continue;
		done = snd_mixer_selem_set_playback_volume_all(e, level) >= 0;
		break;
	}
	snd_mixer_close(mixer);
	return done;
}

#else   /* not __linux__ */

bool btvol_set(const char *sink, int level) { (void)sink; (void)level; return false; }

#endif
