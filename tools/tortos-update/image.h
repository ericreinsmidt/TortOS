/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TORTOS_UPDATE_IMAGE_H
#define TORTOS_UPDATE_IMAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "write.h"

/* A release's card image, TortOS-v<version>-<device>.img.xz, unpacked as it
 * is read: the up_image write.c reads, by XZ Embedded (vendor/xz-embedded,
 * fetched by mk/fetch-vendor.sh). Nothing is unpacked onto the card.
 *
 * A release is made with xz -9 (plastron's scripts/release.sh), whose
 * dictionary is 64 MiB: that much memory, held while it runs. */

typedef struct xz_image xz_image;

/* The image at `path`, or NULL with the reason in `why` */
xz_image *xz_image_open(const char *path, char *why, size_t whyn);
void xz_image_close(xz_image *x);

/* As write.c reads it */
up_image xz_image_source(xz_image *x);

/* The image's first `n` bytes from `at`, unpacked into `out`, read from the
 * start: for its table and its root's superblock, before the plan */
bool xz_image_peek(xz_image *x, uint64_t at, uint8_t *out, size_t n);

#endif
