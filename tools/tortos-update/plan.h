/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TORTOS_UPDATE_PLAN_H
#define TORTOS_UPDATE_PLAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What an update of this card from this image would write, or why it can't.
 *
 * The rules are TortOS Installer's Update (its src-tauri/src/writer.rs,
 * plan_update), so a card the installer would update in place is a card this
 * updates, and one it refuses this refuses: the image's partitions exactly
 * where the card has them, the card's own others (the games partition the
 * first boot made) wholly past them, and the table itself never written. One
 * difference: the installer writes the whole root partition, this only the
 * root's squashfs, as far as its superblock says it runs. The rest of the
 * partition is never read.
 *
 * No I/O here: it is handed each disk's first sectors and the image's root
 * superblock, so make check runs it on bytes it makes up. */

#define UP_SECTOR 512
/* Enough of a disk's start for either table: an MBR is sector 0, a GPT the
 * protective MBR, its header and 128 entries of 128 bytes, as genimage writes
 * them */
#define UP_HEAD (34 * UP_SECTOR)
#define UP_PARTS 128
/* What the root's superblock is read for: magic and bytes_used */
#define UP_SUPER 48

typedef struct {
	bool used;
	uint64_t start, sectors;	/* in sectors, as the table has them */
} up_part;

typedef struct {
	bool gpt;
	int count;			/* slots read, used or not */
	up_part part[UP_PARTS];
} up_table;

/* Bytes from..to, the same offsets on the image and the card */
typedef struct {
	uint64_t from, to;
} up_span;

typedef struct {
	up_span loader;		/* the boot loader: from `skip` to the first partition */
	up_span boot;		/* partition 1 */
	up_span root;		/* partition 2, to the end of its squashfs */
} up_plan;

/* A disk's table from its first `len` bytes (UP_HEAD for a GPT). False when
 * there is none, or it is not one this can read. */
bool up_read_table(const uint8_t *head, size_t len, up_table *out);

/* The plan, or false with the reason in `why`. `skip` is where the device's
 * boot loader starts (32 KiB on the GKD Pixel 2, 128 KiB on the Brick, as
 * each one's boot ROM looks there); `root_super` is the image's first
 * UP_SUPER bytes of partition 2. */
bool up_plan_update(const uint8_t *image_head, uint64_t image_len,
                    const uint8_t *card_head, const uint8_t *root_super,
                    uint64_t skip, up_plan *out, char *why, size_t whyn);

#endif
