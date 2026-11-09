/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TORTOS_UPDATE_H
#define TORTOS_UPDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* TortOS's half of updating itself over Wi-Fi (#69): is there a newer
 * release, and exactly which file is it. The updater that writes it onto the
 * card is tools/tortos-update; between the two goes .update/request.
 *
 * The answer comes from GitHub's API for the latest release, which names
 * every file with its size and SHA-256. This device's image is
 * TortOS-v<version>-<device>.img.xz, as the release build names it and the
 * updater insists on. SHA256SUMS is taken too, so the download can be checked
 * against two sources.
 *
 * Nothing here draws or knows which device it runs on: the device's name
 * comes in from the caller, so all of it builds and is checked on the Mac
 * (make check-release). */

/* Where the answer comes from. A development build can point it elsewhere
 * (-DUPDATE_FEED=...), to try an update against a release served locally, and
 * may then download over plain http. Nothing on the card can change it: the
 * feed names the image, and the image is written over the system. */
#ifdef UPDATE_FEED
#define UPDATE_FEED_IS_DEV 1
#else
#define UPDATE_FEED "https://api.github.com/repos/ericreinsmidt/TortOS/releases/latest"
#define UPDATE_FEED_IS_DEV 0
#endif

/* Where the Down To The Wire page downloads a release's images: GitHub
 * Pages, which a workflow fills on each release, as a web page can't read a
 * release download itself (no CORS header there). The page asks UPDATE_FEED
 * which image and SHA-256, downloads it from here and sends it to the device,
 * which checks it against that SHA-256: this only says where the bytes come
 * from. A development build can point it elsewhere with UPDATE_FEED. */
#ifndef UPDATE_FILES
#define UPDATE_FILES "https://ericreinsmidt.github.io/TortOS/"
#endif

/* At most one check a day: a menu opened between checks shows the answer
 * kept on the card. */
#define UPDATE_CHECK_EVERY_S (24 * 60 * 60)

typedef struct {
	char      version[24];    /* "2.2.0": the release's tag, less its v; up to 23 by the rule */
	char      file[96];       /* TortOS-v2.2.0-brick.img.xz */
	char      url[512];       /* where to download it */
	long long size;           /* its size in bytes */
	char      sha256[65];     /* its SHA-256, as GitHub reports it */
	char      sums_url[512];  /* the release's SHA256SUMS */
	char      sums_sha256[65];
} update_release;

typedef enum {
	UPDATE_UNKNOWN = -1,   /* no answer: offline, refused, or a reply that won't read */
	UPDATE_CURRENT = 0,    /* the latest release is this version, or older */
	UPDATE_WAITING = 1,    /* a newer release, described in update_release */
} update_state;

/* Read a reply from UPDATE_FEED. True, with *out filled, when it is a release
 * (not a prerelease) whose tag is a version and which carries this device's
 * image and SHA256SUMS, each with a size, a download address and a
 * well-formed SHA-256. False otherwise, with the reason in why. Says nothing
 * about whether the release is newer. */
bool update_parse(const char *json, size_t len, const char *device,
                  update_release *out, char *why, size_t whyn);

/* Whether candidate is a later version than installed. A version is one to
 * four numbers joined by dots ("2.1.1"), compared number by number, so 2.10.0
 * is later than 2.9.0 and 2.1 is the same as 2.1.0. False when either isn't
 * a version, so a development build calling itself "local" is never offered
 * anything. */
bool update_is_newer(const char *candidate, const char *installed);

/* The answer kept on the card: when it was asked, for which installed
 * version, and what came back. key=value lines, replaced whole. */
bool update_cache_write(const char *path, time_t when, const char *installed,
                        update_state state, const update_release *r);

/* That answer, if it still stands: asked within the last day, by this same
 * installed version (after an update it no longer does), and readable whole.
 * A clock that has gone backwards past it makes it stale too. False when there
 * is no answer to show, and the check should be made. */
bool update_cache_read(const char *path, time_t now, const char *installed,
                       update_state *state, update_release *r);

/* The same answer whatever its age, for showing: "2.2.0 available" stays
 * true until a newer check says otherwise. Still only for this installed
 * version, and held to the same rules. */
bool update_cache_peek(const char *path, const char *installed,
                       update_state *state, update_release *r);

/* The whole check: ask UPDATE_FEED, read the reply, decide, and keep the
 * answer in cache_path. Blocks for up to timeout_s, so it's for a worker or a
 * screen that says it is checking, never a frame loop. An UPDATE_UNKNOWN
 * isn't kept, so a failed check is tried again next time rather than
 * tomorrow. */
update_state update_check(const char *device, const char *installed,
                          const char *cache_path, time_t now, int timeout_s,
                          update_release *out, char *why, size_t whyn);

/* Read a reply that has already arrived (the System Update screen fetches
 * it without blocking), decide, and keep the answer, as update_check does
 * after its fetch. */
update_state update_answer(const char *json, size_t len, const char *device,
                           const char *installed, const char *cache_path, time_t now,
                           update_release *out, char *why, size_t whyn);

/* Whether the release's SHA256SUMS gives `file` the SHA-256 `sha256`: one
 * line of sha256sum's own form for it, "<64 hex>  <file>" (or " *<file>"),
 * and no line giving it any other. The second source for a download, beside
 * GitHub's digest. */
bool update_sums_list(const char *sums, size_t len, const char *file, const char *sha256);

/* An update the Down To The Wire page offers (#69): it downloaded the
 * release itself and names the image as GitHub does, with GitHub's size and
 * SHA-256. True, with file, version, size and sha256 filled (no addresses),
 * when file is exactly this device's image for that version
 * (TortOS-v<version>-<device>.img.xz), the version is later than installed,
 * size is a plausible image's and sha256 is 64 lowercase hex digits. False
 * otherwise, with the reason in why. The image is still checked against the
 * SHA-256 once it is on the card: this only decides what may be sent. */
bool update_offer(const char *file, const char *version, const char *size,
                  const char *sha256, const char *device, const char *installed,
                  update_release *out, char *why, size_t whyn);

/* .update/request for the updater (tools/tortos-update/update.c reads it):
 * file, version, device, size and sha256 as key=value lines, replaced whole. */
bool update_request_write(const char *path, const char *device, const update_release *r);

/* The one line the updater leaves in .update/result. */
typedef enum {
	UPDATE_RESULT_NONE,        /* no line, or not one of the updater's */
	UPDATE_RESULT_DONE,        /* "updated to TortOS <version>" */
	UPDATE_RESULT_UNTOUCHED,   /* "not updated, nothing written: <why>" */
	UPDATE_RESULT_FAILED,      /* "FAILED after writing: ..." */
} update_result;

/* Which outcome a line says, with its detail (the version, or why) in
 * detail. */
update_result update_result_parse(const char *line, char *detail, size_t n);

/* Whether the battery will see an update through: on the charger, or above
 * the point the low-battery dot shows at. The drain runs (2026-10-28) left 22
 * minutes or more at 2% on both devices, and the writing takes 22 to 33 s. A
 * device whose battery can't be read is let through: there is none to run
 * out. */
#define UPDATE_BATTERY_MIN_PCT 10
bool update_battery_ok(bool readable, bool charging, int pct);

/* ---- The shelf's own check, and its toast (#69 step 4) ----
 *
 * The shelf asks UPDATE_FEED by itself, so an update shows up without anyone
 * opening System Update. Never more than once a day: what it checks against
 * is the answer kept on the card, which update_cache_read calls stale after a
 * day. These are the rules, apart from the shelf so check-release can step
 * through them. */
typedef struct {
	bool wifi;    /* Wi-Fi was connected at the last pass */
	bool armed;   /* a check may start; true from the launcher's start */
} update_bg;

/* One pass of the shelf. wifi: connected now. slot_free: nothing else holds
 * the one network request (src/net.h). Armed at the start and again whenever
 * Wi-Fi joins; the shelf also arms it after a game. True means look at the
 * kept answer and ask if it is stale, and either way it disarms, so a check
 * that fails or is given up waits for the next arming rather than asking
 * again every pass. */
bool update_bg_due(update_bg *bg, bool wifi, bool slot_free);

/* Whether the shelf shows "Update available": one waiting, Wi-Fi connected
 * (a device without it hears from the Wire page instead), and not shown
 * already since this boot. */
bool update_toast_due(update_state state, bool wifi, bool shown);

#endif
