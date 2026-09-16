/* SPDX-License-Identifier: MIT */
#ifndef TORTOS_SSRUN_H
#define TORTOS_SSRUN_H

#include <stdbool.h>

#include "ssfetch.h"

/* ---- one game, without blocking a frame ---------------------------------
 *
 * The same work as ss_lookup, driven a step at a time, because the loop that
 * would wait for it is the loop that reads the power button - and a
 * ScreenScraper call measured 5.5 seconds per game. A handheld that stops
 * answering its own power button for five seconds is not slow, it is broken.
 *
 * One at a time: there is a single async slot in net.c, and this needs it for
 * the lookup and then again for the cover.
 *
 * ss_run_step returns 1 while it is working, 0 when it has finished, and -1
 * when it could not - no account, no answer, a name that did not survive the
 * check, or no cover on offer. -1 is the ordinary case for a game they do not
 * have, and it is the caller's signal to fall through to libretro. */
bool ss_run_begin(const char *folder, const char *file, const char *stem,
                  const char *rom_dir, const char *exts);
int  ss_run_step(void);
void ss_run_cancel(void);

/* What the run learned, valid once ss_run_step has returned 0. */
const ss_result *ss_run_result(void);

/* Where it got to, for a screen to say. Never a URL: that carries the
 * account. */
const char *ss_run_where(void);

#endif
