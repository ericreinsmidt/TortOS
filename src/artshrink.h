/* SPDX-License-Identifier: MIT */
#ifndef TORTOS_ARTSHRINK_H
#define TORTOS_ARTSHRINK_H

/* Shrink a fetched cover in place to the largest size it is ever drawn at.
 *
 * Silent and best-effort: anything that fails leaves the file exactly as it
 * arrived, because art that is too large still works and a half-written PNG
 * does not. Never upscales. */
void art_shrink(const char *path);

#endif
