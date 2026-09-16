/* SPDX-License-Identifier: MIT */
/* See ssrun.h. Apart from ssfetch.c on purpose: this half reaches for the
 * card, the shrinker and the shelf's paths, while that half is the reading and
 * the judging and is linked by tools/ss-check.c with no SDL anywhere near it.
 * ADR-0001 is what that seam is for. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ssrun.h"

#include "artscrape.h"
#include "artshrink.h"
#include "db.h"
#include "library.h"
#include "net.h"
#include "ss.h"


#define SS_TMP "/tmp/tortos-ss-reply.json"

/* Big enough for the largest reply seen: one game arrives with every region's
 * names and dates, every language's synopsis, and up to 54 media URLs. */
#define SS_REPLY_MAX (192 * 1024)

/* Sized from what actually goes in them, the way artscrape.c sizes its own.
 * A destination sized "like the directory" is how the cross compiler came to
 * point out that the middle of the path could not fit - clang says nothing
 * about any of this, so the device build is the only place it shows up. */
#define SS_DIR_MAX  (LIB_PATH * 2)
#define SS_DEST_MAX (SS_DIR_MAX + LIB_PATH + 32)

static struct {
	enum { R_OFF, R_ASK, R_RETRY, R_ART, R_DONE, R_FAIL } phase;
	char      folder[64], file[LIB_PATH], stem[LIB_PATH];
	char      dir[SS_DIR_MAX], dest[SS_DEST_MAX];
	uint32_t  crc;
	ss_result got;
	char      where[64];
} g_run;

const ss_result *ss_run_result(void) { return &g_run.got; }
const char      *ss_run_where(void)  { return g_run.where; }

void ss_run_cancel(void)
{
	if (g_run.phase == R_ASK || g_run.phase == R_RETRY || g_run.phase == R_ART)
		net_async_abort();
	remove(SS_TMP);
	memset(&g_run, 0, sizeof g_run);
}

/* Start one lookup, by checksum when there is one. */
static bool start_ask(uint32_t crc)
{
	char url[1024];
	bool ok;

	if (!ss_lookup_url(url, sizeof url, g_run.folder, g_run.file, crc))
		return false;
	remove(SS_TMP);
	ok = net_get_async(url, SS_TMP, 30);
	memset(url, 0, sizeof url);          /* it carried both credentials */
	return ok;
}

bool ss_run_begin(const char *folder, const char *file, const char *stem,
                  const char *rom_dir, const char *exts)
{
	memset(&g_run, 0, sizeof g_run);
	if (!ss_signed_in() || !folder || !file || !stem) return false;
	if (ss_system_id(folder) == 0) return false;

	snprintf(g_run.folder, sizeof g_run.folder, "%s", folder);
	snprintf(g_run.file, sizeof g_run.file, "%s", file);
	snprintf(g_run.stem, sizeof g_run.stem, "%s", stem);
	/* The system's directory, handed in rather than built from platform.h:
	 * this file has no business knowing where the card keeps its ROMs, and
	 * that is also what keeps it linkable without SDL. */
	snprintf(g_run.dir, sizeof g_run.dir, "%s", rom_dir ? rom_dir : ".");
	/* A loose file has no central directory to read, so it is asked about by
	 * name alone - the same path a disc image takes. */
	if (!art_rom_crc(rom_dir, stem, exts, &g_run.crc)) g_run.crc = 0;

	snprintf(g_run.where, sizeof g_run.where, "looking it up");
	if (!start_ask(g_run.crc)) return false;
	g_run.phase = R_ASK;
	return true;
}

/* The reply file, into g_run.got. */
static bool read_reply(void)
{
	char *body;
	FILE *f = fopen(SS_TMP, "rb");
	long n = 0;
	bool ok = false;

	if (!f) return false;
	if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && n < SS_REPLY_MAX &&
	    fseek(f, 0, SEEK_SET) == 0 && (body = malloc((size_t)n + 1))) {
		if (fread(body, 1, (size_t)n, f) == (size_t)n) {
			body[n] = '\0';
			ok = ss_parse(body, (size_t)n, g_run.file, &g_run.got);
		}
		free(body);
	}
	fclose(f);
	remove(SS_TMP);
	return ok;
}

int ss_run_step(void)
{
	int poll;

	switch (g_run.phase) {
	case R_OFF:  return -1;
	case R_DONE: return 0;
	case R_FAIL: return -1;

	case R_ASK:
	case R_RETRY:
		poll = net_async_poll();
		if (poll == 0) return 1;                     /* still in flight */
		if (poll < 0 || !read_reply() || !g_run.got.found) {
			g_run.phase = R_FAIL;
			return -1;
		}
		/* A checksum answer that fails the check is asked again by name; see
		 * ss_lookup for the measurement that says why. Once only - the retry
		 * has already been asked the only other way there is. */
		if (!g_run.got.name_ok && g_run.phase == R_ASK && g_run.crc) {
			snprintf(g_run.where, sizeof g_run.where, "asking by name");
			if (start_ask(0)) { g_run.phase = R_RETRY; return 1; }
		}
		if (!g_run.got.name_ok || !g_run.got.art[0]) {
			/* Either it is the wrong game or it has no cover. The text may
			 * still be worth keeping, but the caller asked for art and the
			 * answer is no; libretro gets its turn. */
			g_run.phase = R_FAIL;
			return -1;
		}
		snprintf(g_run.dest, sizeof g_run.dest, "%s/.media/%s.png",
		         g_run.dir, g_run.stem);
		snprintf(g_run.where, sizeof g_run.where, "fetching the cover");
		if (!net_get_async(g_run.got.art, g_run.dest, 60)) {
			g_run.phase = R_FAIL;
			return -1;
		}
		g_run.phase = R_ART;
		return 1;

	case R_ART:
		poll = net_async_poll();
		if (poll == 0) return 1;
		if (poll < 0) { g_run.phase = R_FAIL; return -1; }
		/* Down to the size a card is ever drawn at, the same as a libretro
		 * cover: art_shrink queues it and returns. */
		art_shrink(g_run.dest);
		/* The text goes in with it. A scrape that fetched a cover and threw
		 * the year and the synopsis away would have to be run again to get
		 * them, and this is the only moment they are in hand. */
		db_game_set(db_lib(), g_run.folder, g_run.file, &g_run.got.meta);
		g_run.phase = R_DONE;
		return 0;
	}
	return -1;
}
