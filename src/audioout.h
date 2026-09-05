/* SPDX-License-Identifier: 0BSD */
/* Where the system's sound goes, as a decision rather than as a device.
 *
 * Diatom's ADR-0029 says the launcher owns WHICH output and the port owns
 * opening it. This is that ownership, on this side: three facts in, one device
 * string out. It is deliberately the SYSTEM's audio output and not "Diatom's
 * sink" - the audiobook and music daemon will read the same answer, and naming
 * it the narrow way is the thing that would have to be undone.
 *
 * No SDL, no device reads, no protocol. The gathering lives in main.c; this is
 * only the rule, so tools/audioout-check.c can hold it to that rule without a
 * handheld. Same split as the menus.
 */
#ifndef TORTOS_AUDIOOUT_H
#define TORTOS_AUDIOOUT_H

#include <stdbool.h>

/* What the player asked for. Two positions, not three.
 *
 * An earlier sketch had Auto / Speaker / Headset. Once a plugged-in cable wins
 * over everything, a pinned Headset does nothing Auto does not already do -
 * Auto takes Bluetooth whenever it is connected, and neither position can route
 * to a headset that is not there. The only override worth having is REFUSING
 * Bluetooth, so that is the only one offered. */
typedef enum {
	AOUT_AUTO = 0,   /* wired, then Bluetooth, then the speaker */
	AOUT_SPEAKER,    /* never Bluetooth; a cable still works */
	AOUT_POLICY_COUNT
} aout_policy;

/* Where the sound actually ends up. Three destinations, but only TWO device
 * strings: wired and speaker are the same ALSA device, because the jack switch
 * that separates them lives in the codec, below ALSA, and is the port's job
 * (plat_audio_jack_poll). The distinction survives here only because the menu
 * has to be able to say which one you are hearing. */
typedef enum { AOUT_SPK = 0, AOUT_WIRED, AOUT_BT } aout_dest;

typedef struct {
	aout_policy policy;
	bool        wired;     /* a jack is inserted, from SW_HEADPHONE_INSERT */
	const char *bt_sink;   /* ALSA device for a connected sink, else NULL/"" */
} aout_state;

/* wired > bluetooth > speaker. A cable is the clearest statement of intent a
 * player can make, and it beats a headset that merely happens to be connected. */
aout_dest   aout_resolve(const aout_state *s);

/* The device string to hand Diatom: the sink's own name, or "" for the codec.
 * Never NULL, so a caller can always print or compare it. */
const char *aout_device(const aout_state *s);

/* For the menu: "Auto" / "Speaker", and what Auto currently resolves to. */
const char *aout_policy_name(aout_policy p);
const char *aout_dest_name(aout_dest d);

#endif
