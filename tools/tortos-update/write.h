/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TORTOS_UPDATE_WRITE_H
#define TORTOS_UPDATE_WRITE_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "plan.h"

/* Writing a plan (plan.h) onto the card, and reading it back.
 *
 * The root first, then the boot partition, then the boot loader, each chunk
 * compared with what the card holds and written only where it differs: an
 * update whose boot loader hasn't changed never writes it, and a run cut
 * short and started again writes only what it hadn't. Then everything is
 * read back from the card, past its cache, against a second pass of the
 * image: no checksum to trust, and a mismatch says where it is. The
 * installer's Update does the same second pass (TortOS-Installer, writer.rs).
 *
 * The image is read front to back, as an .xz can only be, so the boot loader
 * and boot partition, which come before the root, are held in memory until
 * their turn: about 80 MiB on the GKD Pixel 2 and 68 on the Brick, against
 * 1 GB on each, with nothing else running. */

/* The image from its first byte, as many times as asked: `open` starts it
 * over, `read` fills `buf` and is short only at the image's end, or -1 */
typedef struct {
	void *ctx;
	bool (*open)(void *ctx);
	ssize_t (*read)(void *ctx, uint8_t *buf, size_t n);
} up_image;

enum { UP_WRITING, UP_CHECKING };

/* Called after each chunk, `done` of `total` bytes of the phase */
typedef void (*up_progress)(void *ctx, int phase, uint64_t done, uint64_t total);

typedef struct {
	bool touched;		/* anything written: a failure now needs the installer */
	uint64_t written;	/* bytes that differed and went on the card */
	char why[256];		/* what failed, for the log */
} up_result;

/* A partition the update writes, claimed: opened exclusively, which the
 * kernel refuses (EBUSY) while any filesystem holds it, and which keeps
 * anything from mounting it for as long as the claim is held. So the root
 * the system ran from is proven let go of before a byte is written. The
 * card itself can't be claimed: its games partition is mounted, for the
 * image and the log, and the kernel won't give out a disk while one of its
 * partitions is in use (found on the Brick, 2026-11-04). -1 with the reason
 * in `why`. */
int up_claim(const char *partition, char *why, size_t whyn);

/* The card, for writing: the whole disk, as the boot loader lies in front of
 * every partition. Claim the partitions first. -1 with the reason in `why`. */
int up_open_card(const char *path, char *why, size_t whyn);

/* The plan onto the open card. False with r->why on any failure; r->touched
 * says whether the card was written to before it. */
bool up_write(int card, const up_image *image, const up_plan *plan,
              up_progress progress, void *progress_ctx, up_result *r);

#endif
