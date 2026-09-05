/* SPDX-License-Identifier: 0BSD */
/* See src/audioout.h for why this is a file of its own. */
#include "audioout.h"

aout_dest aout_resolve(const aout_state *s)
{
	/* A cable wins outright, in every policy. Someone who physically plugged
	 * something in has said what they want more plainly than any setting, and
	 * a headset that is merely connected has not said anything at all. */
	if (s->wired) return AOUT_WIRED;
	if (s->policy == AOUT_AUTO && s->bt_sink && s->bt_sink[0]) return AOUT_BT;
	return AOUT_SPK;
}

const char *aout_device(const aout_state *s)
{
	/* "" is the port's default device, which on this hardware is the codec
	 * through dmix - and therefore both the speaker AND the wired jack. */
	return aout_resolve(s) == AOUT_BT ? s->bt_sink : "";
}

const char *aout_policy_name(aout_policy p)
{
	return p == AOUT_SPEAKER ? "Speaker" : "Auto";
}

const char *aout_dest_name(aout_dest d)
{
	return d == AOUT_BT ? "bluetooth" : d == AOUT_WIRED ? "wired" : "speaker";
}
