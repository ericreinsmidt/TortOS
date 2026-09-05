/* Where does the sound go, given what is plugged in and what was asked for?
 *
 * Eight combinations of three facts, and the whole rule is five lines - which
 * is exactly the kind of thing that is obviously right until a headset connects
 * during a game and it is not. It is a pure function precisely so this can
 * enumerate every case rather than sample the ones somebody thought of.
 *
 * Links src/audioout.c and NOT SDL. If it ever needs SDL, the split has failed.
 */
#include "../src/audioout.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void ck(int cond, const char *what)
{
	if (!cond) { printf("  FAIL %s\n", what); fails++; }
}

#define BT "bluealsa:DEV=AA:BB:CC:DD:EE:FF,PROFILE=a2dp"

static aout_state st(aout_policy p, bool wired, const char *bt)
{
	aout_state s;
	s.policy = p; s.wired = wired; s.bt_sink = bt;
	return s;
}

/* The rule the whole feature is: wired > bluetooth > speaker. */
static void priority(void)
{
	aout_state s;

	printf("wired beats bluetooth beats speaker:\n");

	s = st(AOUT_AUTO, true, BT);
	ck(aout_resolve(&s) == AOUT_WIRED, "a cable wins over a connected headset");
	s = st(AOUT_AUTO, false, BT);
	ck(aout_resolve(&s) == AOUT_BT, "the headset wins over the speaker");
	s = st(AOUT_AUTO, false, NULL);
	ck(aout_resolve(&s) == AOUT_SPK, "with neither, the speaker");

	/* The cable wins in EVERY policy, which is the part a setting could
	 * plausibly have been allowed to override and must not. */
	s = st(AOUT_SPEAKER, true, BT);
	ck(aout_resolve(&s) == AOUT_WIRED, "a cable wins even when pinned to Speaker");
}

/* Speaker means "never Bluetooth", and nothing else. */
static void the_override(void)
{
	aout_state s;

	printf("the Speaker override:\n");

	s = st(AOUT_SPEAKER, false, BT);
	ck(aout_resolve(&s) == AOUT_SPK, "refuses a connected headset");
	ck(!strcmp(aout_device(&s), ""), "and sends the codec, not the sink");

	s = st(AOUT_AUTO, false, BT);
	ck(!strcmp(aout_device(&s), BT), "Auto sends the sink's own device string");
}

/* Two destinations share one device string, and that is not a bug: the jack
 * switch that separates them is in the codec, below ALSA. */
static void device_strings(void)
{
	aout_state w = st(AOUT_AUTO, true, NULL);
	aout_state k = st(AOUT_AUTO, false, NULL);
	aout_state b = st(AOUT_AUTO, false, BT);

	printf("device strings:\n");
	ck(!strcmp(aout_device(&w), ""), "wired is the default device");
	ck(!strcmp(aout_device(&k), ""), "so is the speaker");
	ck(aout_resolve(&w) != aout_resolve(&k),
	   "but they are still told apart, for the menu to name");
	ck(aout_device(&b)[0] != '\0', "bluetooth is not the default device");
	ck(aout_device(&w) != NULL && aout_device(&b) != NULL,
	   "never NULL, so a caller can always print it");
}

/* An empty sink string is what a disconnect leaves behind, and must read as
 * "no headset" rather than as a device named "". */
static void empty_sink(void)
{
	aout_state s = st(AOUT_AUTO, false, "");

	printf("a sink that went away:\n");
	ck(aout_resolve(&s) == AOUT_SPK, "an empty sink is not a destination");
	ck(!strcmp(aout_device(&s), ""), "and resolves to the codec");
}

/* Every combination, so nothing is left to a case nobody thought of. */
static void every_case(void)
{
	int p, w, b, n = 0;

	printf("all eight combinations resolve, and only wired outranks a sink:\n");
	for (p = 0; p < AOUT_POLICY_COUNT; p++)
		for (w = 0; w < 2; w++)
			for (b = 0; b < 2; b++) {
				aout_state s = st((aout_policy)p, w != 0, b ? BT : NULL);
				aout_dest  d = aout_resolve(&s);

				n++;
				ck(d == AOUT_SPK || d == AOUT_WIRED || d == AOUT_BT,
				   "resolves to a real destination");
				if (w) ck(d == AOUT_WIRED, "wired always wins");
				if (d == AOUT_BT) ck(!w && b && p == AOUT_AUTO,
				                     "bluetooth only unplugged, connected, on Auto");
			}
	ck(n == 8, "eight combinations were actually tried");
}

static void names(void)
{
	printf("what the menu says:\n");
	ck(!strcmp(aout_policy_name(AOUT_AUTO), "Auto"), "Auto");
	ck(!strcmp(aout_policy_name(AOUT_SPEAKER), "Speaker"), "Speaker");
	ck(strcmp(aout_dest_name(AOUT_WIRED), aout_dest_name(AOUT_SPK)) != 0,
	   "wired and speaker read differently");
	ck(strcmp(aout_dest_name(AOUT_BT), aout_dest_name(AOUT_SPK)) != 0,
	   "so do bluetooth and speaker");
}

int main(void)
{
	priority();
	the_override();
	device_strings();
	empty_sink();
	every_case();
	names();
	if (fails) { printf("\n%d audio output check(s) failed\n", fails); return 1; }
	printf("\nok: sound goes where it should\n");
	return 0;
}
