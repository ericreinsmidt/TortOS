/* SPDX-License-Identifier: 0BSD */
/* Does the device's title matcher agree with the host tool's?
 *
 *     make check-artscrape
 *
 * The C in src/artscrape.c is a port of tools/scrape-art.py, and the only
 * thing the port has to get right is norm(). Everything the feature claims -
 * 97% of a 178-ROM library matched, against 83% for guessing filenames -
 * rests on the two agreeing about what a title reduces to.
 *
 * A drift here does not fail. It finds fewer games and looks exactly like
 * libretro carrying less art than it does, which is unfalsifiable from the
 * device. So this prints what the C makes of a list of names and the Python
 * half compares it against re.sub, over the real library when there is one -
 * the same shape as check-rahash, and for the same reason.
 */
#include <stdio.h>
#include <string.h>

#include "../src/artscrape.h"

int main(int argc, char **argv)
{
	char line[512], out[512];
	size_t n;

	(void)argc; (void)argv;
	/* One name per line in, one normalised form per line out. Deliberately
	 * dumb: the Python drives it, so this stays a pipe rather than growing
	 * its own idea of where a library is. */
	while (fgets(line, sizeof line, stdin)) {
		n = strlen(line);
		while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
		art_norm(line, out, sizeof out);
		printf("%s\n", out);
	}
	return 0;
}
