/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TORTOS_NOTICE_H
#define TORTOS_NOTICE_H

#include <stdbool.h>
#include <stdint.h>

/* A line of text, rendered to the file Diatom composites over a running game.
 *
 * This exists because the display has exactly one owner at a time. While a
 * game runs, that is Diatom, and this process must not present - so it cannot
 * put anything on screen itself. Diatom can, and already does for the volume
 * bar, but it has no font and is not getting one. So the text is rendered
 * here, to pixels, and carried over.
 *
 * Safe to call mid-game: rendering to a surface touches no display. Only
 * SDL_RenderPresent would, and nothing here presents.
 *
 * Its own file rather than part of cheevos.c because that one is deliberately
 * SDL-free - `make check-cheevos` links it with no SDL at all, and the parsing
 * and filtering it checks should not need a window to be tested.
 */

/* Writes `path` in Diatom's overlay format: "DTOV", uint16 w, uint16 h, then
 * w*h BGRA rows. False if there is no font or the file cannot be written. */
bool notice_render(const char *heading, const char *body, const char *path);

/* The same notice as pixels, for the shelf, which draws its own (the update
 * toast) because no game is running to draw it: w*h BGRA rows, as
 * notice_render writes them, from malloc for the caller to free. NULL if
 * there is no font. */
uint8_t *notice_pixels(const char *heading, const char *body, int *w, int *h);

#endif
