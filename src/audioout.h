/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Where the system's sound goes, as a decision rather than as a device.
 *
 * In Diatom, the launcher owns WHICH output and the port owns opening it. This
 * is that ownership, on this side: three facts in, one device string out. It
 * is deliberately the SYSTEM's audio output and not "Diatom's sink" - the
 * audiobook and music daemon will read the same answer, and naming it the
 * narrow way is the thing that would have to be undone.
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
	AOUT_AUTO = 0,   /* USB, then wired, then Bluetooth, then the speaker */
	AOUT_SPEAKER,    /* never Bluetooth; a cable still works */
	AOUT_POLICY_COUNT
} aout_policy;

/* Where the sound actually ends up. Three destinations, but only TWO device
 * strings: wired and speaker are the same ALSA device, because the jack switch
 * that separates them lives in the codec, below ALSA, and is the port's job
 * (plat_audio_jack_poll). The distinction survives here only because the menu
 * has to be able to say which one you are hearing. */
/* USB is a fourth: a USB-C DAC, on a device whose port can be a host (the
 * GKD Pixel 2). Its device string is the system's "usb", a mixed device in
 * front of the DAC, so unlike a Bluetooth sink it is not handed between Muse
 * and Diatom: both have it at once, as both have the codec. */
typedef enum { AOUT_SPK = 0, AOUT_WIRED, AOUT_BT, AOUT_USB } aout_dest;

/* The USB destination's device string, in the system's ALSA config. */
#define AOUT_USB_DEVICE "usb"

typedef struct {
	aout_policy policy;
	bool        wired;     /* a jack is inserted, from SW_HEADPHONE_INSERT */
	bool        usb;       /* a USB sound card is plugged in */
	const char *bt_sink;   /* ALSA device for a connected sink, else NULL/"" */
} aout_state;

/* usb > wired > bluetooth > speaker. A cable is the clearest statement of
 * intent a player can make, and it beats a headset that merely happens to be
 * connected; of two cables, a DAC is the one plugged in on purpose. Eric's
 * call, 2026-10-05. A charging cable is not a sound card, so it changes
 * nothing. */
aout_dest   aout_resolve(const aout_state *s);

/* The device string to hand Diatom: the sink's own name, AOUT_USB_DEVICE, or
 * "" for the codec.
 * Never NULL, so a caller can always print or compare it. */
const char *aout_device(const aout_state *s);

/* For the menu: "Auto" / "Speaker", and what Auto currently resolves to. */
const char *aout_policy_name(aout_policy p);
const char *aout_dest_name(aout_dest d);

/* ---- which ears hear what --------------------------------------------------
 *
 * For a player who hears with one ear, after someone deaf in one asked for it
 * (Eric, 2026-10-11). Stereo as things were made; mono, the two channels as
 * one in both ears, which on its own puts everything in the ear that hears;
 * and mono on one side only, with silence on the other, for a player who would
 * rather have nothing in the other ear. Diatom and Muse do the mixing, each in
 * its own stream - the codec has no mono, the Brick's ALSA chain is TrimUI's,
 * and a Bluetooth sink bypasses any chain - so this side only decides. */
typedef enum {
	AOUT_STEREO = 0,
	AOUT_MONO,
	AOUT_MONO_LEFT,
	AOUT_MONO_RIGHT,
	AOUT_CHANNELS_COUNT
} aout_channels;

/* Where a device string reaches. "" is the codec, so the jack decides between
 * wired and the speaker; the system's "usb" is the DAC; anything else is a
 * sink. Each program asks for its own: with Muse on a headset, Diatom is on
 * the codec, and the two need different mixes. */
aout_dest     aout_dest_of(const aout_state *s, const char *dev);

/* The mix a program sending to `d` makes of what was chosen. One side is about
 * ears, and a speaker is not on one: the Brick's two are a few centimeters
 * apart, so a side would only lose loudness, and the GKD Pixel 2 has one,
 * which a side could silence altogether if the codec feeds it from the other
 * channel. On the speaker, left and right are plain mono. Eric's call,
 * 2026-10-11. */
aout_channels aout_channels_for(aout_channels chosen, aout_dest d);

/* The menu's names - "stereo", "mono", "mono left", "mono right" - and the
 * ones on the wire to Diatom and Muse and in the settings database: "stereo",
 * "mono", "left", "right". Parsing takes the wire names; anything else is
 * stereo, which is what every output did before there was a choice. */
const char   *aout_channels_name(aout_channels c);
const char   *aout_channels_wire(aout_channels c);
aout_channels aout_channels_parse(const char *wire);

/* ---- the headphone jack, and why a remembered state is not enough --------- */

/* "Nobody has acted on the jack yet, or whatever was acted on happened while
 * another process was driving." Distinct from both true and false, because the
 * whole defect was treating "I do not know" as "unchanged". */
#define AOUT_JACK_UNKNOWN (-1)

/* Should the volume be written again?
 *
 * The level is mapped into one of two raw windows depending on whether a cable
 * is in, so a transition has to re-write it or the register keeps a value that
 * belongs to the other window. Each process re-maps only on a transition IT
 * observes, and observes none while the other owns the pad - so `remembered`
 * has to be set to AOUT_JACK_UNKNOWN at every handover, and this must then say
 * yes without a transition to point at.
 *
 * Measured 2026-09-05: with a cable pulled during a game, the launcher's memory
 * still said "out" from before the game, the poll found it already agreeing
 * with the hardware, and the register kept 29 - a headphone value against a
 * speaker window whose quiet end is 39, which sounds like a broken speaker.
 *
 * `have_level` is false before any level is known; there is nothing to write. */
bool aout_should_reapply(int remembered, bool wired_now, bool have_level);

/* Level to raw register, for whichever window the cable selects.
 *
 * INVERTED: the codec's control is attenuation, so the top of the window is the
 * LOUD end and a bigger raw is quieter. Both ends are clamped to the window,
 * because the ranges outside it are either inaudible or painful - see the
 * ladder constants in the caller.
 *
 * `level_max` is the top index, not a count. The two differ by one, and this
 * takes the index form deliberately: Diatom pins the other convention on the
 * wire, and having both spelled the same way here is how they get confused. */
int aout_level_to_raw(int level, int level_max, int win_top, int win_bottom);

#endif
