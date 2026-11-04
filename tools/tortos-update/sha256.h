/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TORTOS_UPDATE_SHA256_H
#define TORTOS_UPDATE_SHA256_H

#include <stddef.h>
#include <stdint.h>

/* SHA-256, as FIPS 180-4 defines it, for the updater to check the release it
 * was handed before it opens the card.
 *
 * Our own, about a hundred lines, rather than a library: the updater is one
 * static program with nothing else to lean on once the root is gone, and
 * this is integrity, not secrecy, so nothing here has to resist timing or
 * anything else an attacker could measure. That it is right is make
 * check-update's job: the standard's own test vectors. */

typedef struct {
	uint32_t state[8];
	uint64_t length;	/* bytes so far */
	uint8_t block[64];
	size_t used;		/* of block */
} sha256;

void sha256_start(sha256 *h);
void sha256_add(sha256 *h, const void *data, size_t n);
/* The digest, and the same as 64 lowercase hex digits and a NUL in `hex` when
 * it isn't NULL, as sha256sum and GitHub write it */
void sha256_finish(sha256 *h, uint8_t digest[32], char hex[65]);

#endif
