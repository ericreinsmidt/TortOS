/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See src/audioout.h for why this is a file of its own. */
#include "audioout.h"

#include <string.h>

aout_dest aout_resolve(const aout_state *s)
{
	/* A cable wins outright, in every policy. Someone who physically plugged
	 * something in has said what they want more plainly than any setting, and
	 * a headset that is merely connected has not said anything at all. */
	if (s->usb)   return AOUT_USB;
	if (s->wired) return AOUT_WIRED;
	if (s->policy == AOUT_AUTO && s->bt_sink && s->bt_sink[0]) return AOUT_BT;
	return AOUT_SPK;
}

const char *aout_device(const aout_state *s)
{
	/* "" is the port's default device, which on this hardware is the codec
	 * through dmix - and therefore both the speaker AND the wired jack. */
	switch (aout_resolve(s)) {
	case AOUT_BT:  return s->bt_sink;
	case AOUT_USB: return AOUT_USB_DEVICE;
	default:       return "";
	}
}

const char *aout_policy_name(aout_policy p)
{
	return p == AOUT_SPEAKER ? "Speaker" : "Auto";
}

const char *aout_dest_name(aout_dest d)
{
	return d == AOUT_USB ? "usb" : d == AOUT_BT ? "bluetooth"
	     : d == AOUT_WIRED ? "wired" : "speaker";
}

aout_dest aout_dest_of(const aout_state *s, const char *dev)
{
	if (!dev || !dev[0]) return s->wired ? AOUT_WIRED : AOUT_SPK;
	return strcmp(dev, AOUT_USB_DEVICE) ? AOUT_BT : AOUT_USB;
}

aout_channels aout_channels_for(aout_channels chosen, aout_dest d)
{
	if (d == AOUT_SPK && (chosen == AOUT_MONO_LEFT || chosen == AOUT_MONO_RIGHT))
		return AOUT_MONO;
	return chosen;
}

static const char *const CHANNELS_NAME[AOUT_CHANNELS_COUNT] = {
	"stereo", "mono", "mono left", "mono right",
};
static const char *const CHANNELS_WIRE[AOUT_CHANNELS_COUNT] = {
	"stereo", "mono", "left", "right",
};

const char *aout_channels_name(aout_channels c)
{
	return c >= 0 && c < AOUT_CHANNELS_COUNT ? CHANNELS_NAME[c] : CHANNELS_NAME[0];
}

const char *aout_channels_wire(aout_channels c)
{
	return c >= 0 && c < AOUT_CHANNELS_COUNT ? CHANNELS_WIRE[c] : CHANNELS_WIRE[0];
}

aout_channels aout_channels_parse(const char *wire)
{
	int c;

	for (c = 0; wire && c < AOUT_CHANNELS_COUNT; c++)
		if (!strcmp(wire, CHANNELS_WIRE[c])) return (aout_channels)c;
	return AOUT_STEREO;
}

bool aout_should_reapply(int remembered, bool wired_now, bool have_level)
{
	if (!have_level) return false;
	/* Unknown is not "unchanged". This is the line the defect was. */
	if (remembered == AOUT_JACK_UNKNOWN) return true;
	return remembered != (wired_now ? 1 : 0);
}

int aout_level_to_raw(int level, int level_max, int win_top, int win_bottom)
{
	long span = (long)win_bottom - win_top;
	long raw;

	if (level_max <= 0) return win_top;
	if (level < 0) level = 0;
	if (level > level_max) level = level_max;
	/* Rounded, not truncated: a half-rung lost at every step walks the whole
	 * ladder away from the ends it was calibrated against. */
	raw = win_top + ((long)(level_max - level) * span + level_max / 2) / level_max;
	if (raw < win_top) raw = win_top;
	if (raw > win_bottom) raw = win_bottom;
	return (int)raw;
}
