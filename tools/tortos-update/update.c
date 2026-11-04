/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * tortos-update: writes a new TortOS release onto the card the device is
 * running from, after everything else has stopped (TortOS#69).
 *
 * TortOS downloads the release's card image into .update/ on the games
 * partition, checks it, writes .update/request and sends init SIGQUIT. Busybox
 * init then runs its ::shutdown: lines (TortOS and Diatom stopped, the game
 * saved, the card unmounted, umount -a -r), kills everything left, and
 * replaces itself with this, as inittab's ::restart: line says. So this runs
 * as PID 1, from the root, the one place the shutdown leaves alone.
 *
 * First it gets off the root: it copies itself into a fresh tmpfs, runs the
 * copy, pivot_roots into the tmpfs and detaches the old root, so nothing
 * reads p2 while it is written. Proven on both devices on 2026-11-03 before
 * any of this was written: pivot_root works over rootswitch's root on the
 * Brick and over the kernel's own on the GKD Pixel 2, and the root's
 * partition can then be opened exclusively, which the kernel refuses while
 * anything holds it. The boot and root partitions are claimed that way, and
 * held, before the card is opened (write.h).
 *
 * Then: the request read and checked against the file (its SHA-256 and that
 * it is this device's), the plan (plan.c, the installer's rules), and the
 * root, boot partition and boot loader written where they differ and read
 * back (write.c). Every step goes in .update/update.log, and the outcome in
 * .update/result for TortOS to say at its next start.
 *
 *   - Done: the request and the image removed, and a restart into the new
 *     version.
 *   - Refused or failed before anything was written: a restart into the
 *     version that was there, as it was.
 *   - Failed after writing began: the card needs TortOS Installer's Update,
 *     which keeps games and saves. The device powers off, as starting it
 *     again would only find a system half written.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "image.h"
#include "plan.h"
#include "sha256.h"
#include "write.h"

#define SELF "/usr/sbin/tortos-update"
#define CARD "/dev/mmcblk0"

/* Where each device's boot ROM looks for the boot loader, as the installer
 * has it, and where the first boot put the games partition */
#if defined(UPDATE_BRICK)
#define DEVICE "brick"
#define SKIP (128 * 1024)
#define GAMES CARD "p4"
#elif defined(UPDATE_PIXEL2)
#define DEVICE "pixel2"
#define SKIP (32 * 1024)
#define GAMES CARD "p3"
#else
#error "build with -DUPDATE_BRICK or -DUPDATE_PIXEL2"
#endif

#define DIR_ "/games/.update"
#define REQUEST DIR_ "/request"
#define LOG DIR_ "/update.log"
#define RESULT DIR_ "/result"

static char log_text[32768];
static size_t log_used;
static int log_fd = -1;
static double started;

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

/* A line for the log: held in memory until the games partition is mounted,
 * then written as it comes, so a failure leaves everything up to it */
static void note(const char *format, ...)
{
	char line[512];
	va_list args;
	int n;

	n = snprintf(line, sizeof line, "%8.1f s  ", now() - started);
	va_start(args, format);
	vsnprintf(line + n, sizeof line - n - 1, format, args);
	va_end(args);
	strcat(line, "\n");
	n = strlen(line);
	if (log_fd >= 0) {
		write(log_fd, line, n);
		fsync(log_fd);
	} else if (log_used + n < sizeof log_text) {
		memcpy(log_text + log_used, line, n);
		log_used += n;
	}
}

/* The outcome for TortOS, one line, then off the card and the restart or the
 * power off. Never returns. */
static void finish(const char *result, int how)
{
	int fd;

	note("%s", result);
	fd = open(RESULT, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		dprintf(fd, "%s\n", result);
		fsync(fd);
		close(fd);
	}
	if (log_fd >= 0)
		close(log_fd);
	umount("/games");
	sync();
	reboot(how);
	for (;;)
		pause();
}

/* Before the games partition is mounted there is nowhere to say anything;
 * the restart leaves the old system as it was */
static void give_up(const char *what)
{
	note("%s: %s", what, strerror(errno));
	sync();
	reboot(RB_AUTOBOOT);
	for (;;)
		pause();
}

static int copy_file(const char *from, const char *to)
{
	char buffer[65536];
	ssize_t n;
	int in = open(from, O_RDONLY), out;

	if (in < 0)
		return -1;
	out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (out < 0) {
		close(in);
		return -1;
	}
	while ((n = read(in, buffer, sizeof buffer)) > 0)
		if (write(out, buffer, n) != n) {
			n = -1;
			break;
		}
	close(in);
	close(out);
	return n < 0 ? -1 : 0;
}

/* From the root: a tmpfs, this program copied into it, the copy run */
static void off_the_root(void)
{
	char start[32];

	/* Our own mounts stay ours: pivot_root refuses a shared root */
	mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
	if (mount("tmpfs", "/tmp", "tmpfs", 0, "mode=0755,size=16m") != 0)
		give_up("mounting a tmpfs");
	if (copy_file(SELF, "/tmp/tortos-update") != 0)
		give_up("copying the updater into the tmpfs");
	snprintf(start, sizeof start, "%.9f", started);
	execl("/tmp/tortos-update", "tortos-update", "2", start, (char *)NULL);
	give_up("running the copy");
}

/* From the copy: into the tmpfs, our own /proc, /dev and /sys, and the old
 * root let go of */
static void into_the_tmpfs(void)
{
	if (chdir("/tmp") != 0)
		give_up("entering the tmpfs");
	mkdir("/tmp/old", 0755);
	if (syscall(SYS_pivot_root, "/tmp", "/tmp/old") != 0)
		give_up("pivot_root into the tmpfs");
	if (chdir("/") != 0)
		give_up("entering the new root");
	mkdir("/proc", 0755);
	mkdir("/dev", 0755);
	mkdir("/sys", 0755);
	mkdir("/games", 0755);
	if (mount("proc", "/proc", "proc", 0, NULL) != 0 ||
	    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) != 0 ||
	    mount("sysfs", "/sys", "sysfs", 0, NULL) != 0)
		give_up("mounting /proc, /dev and /sys");
	if (umount2("/old", MNT_DETACH) != 0)
		give_up("detaching the old root");
}

typedef struct {
	char file[128], version[32], device[32], sha256[65];
	unsigned long long size;
} request;

/* .update/request: key=value lines, written by TortOS (src/update.c). False
 * with the reason in `why` for anything missing or malformed. */
static bool read_request(request *r, char *why, size_t whyn)
{
	char line[256];
	FILE *f = fopen(REQUEST, "r");

	memset(r, 0, sizeof *r);
	if (!f) {
		snprintf(why, whyn, "no request: %s", strerror(errno));
		return false;
	}
	while (fgets(line, sizeof line, f)) {
		char *eq = strchr(line, '='), *value;

		line[strcspn(line, "\r\n")] = 0;
		if (!eq)
			continue;
		*eq = 0;
		value = eq + 1;
		if (!strcmp(line, "file"))
			snprintf(r->file, sizeof r->file, "%s", value);
		else if (!strcmp(line, "version"))
			snprintf(r->version, sizeof r->version, "%s", value);
		else if (!strcmp(line, "device"))
			snprintf(r->device, sizeof r->device, "%s", value);
		else if (!strcmp(line, "sha256"))
			snprintf(r->sha256, sizeof r->sha256, "%s", value);
		else if (!strcmp(line, "size"))
			r->size = strtoull(value, NULL, 10);
	}
	fclose(f);
	if (!r->file[0] || !r->version[0] || !r->device[0] || strlen(r->sha256) != 64 || !r->size) {
		snprintf(why, whyn, "the request is missing something");
		return false;
	}
	return true;
}

/* The request names this device's image of its version, as a release names
 * it, and nothing that could reach outside .update/ */
static bool request_fits(const request *r, char *why, size_t whyn)
{
	char want[128];

	if (strcmp(r->device, DEVICE) != 0) {
		snprintf(why, whyn, "the request is for a %s, this is a %s", r->device, DEVICE);
		return false;
	}
	snprintf(want, sizeof want, "TortOS-v%s-%s.img.xz", r->version, DEVICE);
	if (strcmp(r->file, want) != 0 || strchr(r->version, '/')) {
		snprintf(why, whyn, "the request names %s, not %s", r->file, want);
		return false;
	}
	return true;
}

/* The file is the size and SHA-256 the request says, read whole */
static bool file_matches(const char *path, const request *r, char *why, size_t whyn)
{
	static uint8_t buffer[1 << 20];
	unsigned long long total = 0;
	uint8_t digest[32];
	char hex[65];
	ssize_t n;
	sha256 h;
	int fd = open(path, O_RDONLY);

	if (fd < 0) {
		snprintf(why, whyn, "can't open %s: %s", r->file, strerror(errno));
		return false;
	}
	sha256_start(&h);
	while ((n = read(fd, buffer, sizeof buffer)) > 0) {
		sha256_add(&h, buffer, n);
		total += n;
	}
	close(fd);
	if (n < 0) {
		snprintf(why, whyn, "reading %s: %s", r->file, strerror(errno));
		return false;
	}
	sha256_finish(&h, digest, hex);
	if (total != r->size) {
		snprintf(why, whyn, "the image is %llu bytes, the request says %llu", total, r->size);
		return false;
	}
	if (strcmp(hex, r->sha256) != 0) {
		snprintf(why, whyn, "the image's SHA-256 is %s, the request says %s", hex, r->sha256);
		return false;
	}
	return true;
}

/* A line in the log at each tenth of a phase */
static void progress(void *ctx, int phase, uint64_t done, uint64_t total)
{
	int *last = ctx, tenth = (int)(done * 10 / total);

	if (tenth == last[phase])
		return;
	last[phase] = tenth;
	note("%s %d%%", phase == UP_WRITING ? "writing" : "checking", tenth * 10);
}

static void update(void)
{
	static uint8_t image_head[UP_HEAD], card_head[UP_HEAD], super[UP_SUPER];
	char why[256], path[300], result[400];
	int card, boot, root, last[2] = { -1, -1 };
	up_table table;
	up_result written;
	up_image source;
	xz_image *image;
	up_plan plan;
	request r;

	/* The log from here on, with what came before it */
	if (mount(GAMES, "/games", "exfat", 0, NULL) != 0)
		give_up("mounting the games partition");
	mkdir(DIR_, 0755);
	log_fd = open(LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (log_fd >= 0) {
		write(log_fd, "\n", 1);
		write(log_fd, log_text, log_used);
	}

	if (!read_request(&r, why, sizeof why) || !request_fits(&r, why, sizeof why))
		goto untouched;
	note("request: TortOS %s for the %s, %s", r.version, r.device, r.file);
	snprintf(path, sizeof path, DIR_ "/%s", r.file);
	if (!file_matches(path, &r, why, sizeof why))
		goto untouched;
	note("the image matches its size and SHA-256");

	image = xz_image_open(path, why, sizeof why);
	if (!image)
		goto untouched;
	if (!xz_image_peek(image, 0, image_head, UP_HEAD) ||
	    !up_read_table(image_head, UP_HEAD, &table) || !table.part[1].used ||
	    !xz_image_peek(image, table.part[1].start * UP_SECTOR, super, UP_SUPER)) {
		snprintf(why, sizeof why, "the image's table or root can't be read");
		goto untouched;
	}

	/* Held to the end: nothing can mount them while the update runs */
	boot = up_claim(CARD "p1", why, sizeof why);
	if (boot < 0)
		goto untouched;
	root = up_claim(CARD "p2", why, sizeof why);
	if (root < 0)
		goto untouched;
	card = up_open_card(CARD, why, sizeof why);
	if (card < 0)
		goto untouched;
	note("the boot and root partitions are ours alone");
	if (pread(card, card_head, UP_HEAD, 0) != UP_HEAD) {
		snprintf(why, sizeof why, "reading the card's table: %s", strerror(errno));
		goto untouched;
	}
	/* The unpacked image's length, for the plan's check that it isn't cut
	 * short, is as far as the root's partition reaches: a release's image
	 * runs at least that far, and write.c reads no further */
	if (!up_plan_update(image_head, (table.part[1].start + table.part[1].sectors) * UP_SECTOR,
			    card_head, super, SKIP, &plan, why, sizeof why))
		goto untouched;
	note("plan: boot loader %llu-%llu, boot %llu-%llu, root %llu-%llu",
	     (unsigned long long)plan.loader.from, (unsigned long long)plan.loader.to,
	     (unsigned long long)plan.boot.from, (unsigned long long)plan.boot.to,
	     (unsigned long long)plan.root.from, (unsigned long long)plan.root.to);

	source = xz_image_source(image);
	if (!up_write(card, &source, &plan, progress, last, &written)) {
		snprintf(why, sizeof why, "%s", written.why);
		if (!written.touched)
			goto untouched;
		snprintf(result, sizeof result,
			 "FAILED after writing: %s. Use TortOS Installer's Update; games and saves are safe.",
			 why);
		finish(result, RB_POWER_OFF);
	}
	close(card);
	close(root);
	close(boot);
	xz_image_close(image);
	note("written and read back: %llu bytes changed", (unsigned long long)written.written);
	unlink(path);
	unlink(REQUEST);
	snprintf(result, sizeof result, "updated to TortOS %s", r.version);
	finish(result, RB_AUTOBOOT);

untouched:
	unlink(REQUEST);
	snprintf(result, sizeof result, "not updated, nothing written: %s", why);
	finish(result, RB_AUTOBOOT);
}

int main(int argc, char **argv)
{
	if (argc >= 3 && strcmp(argv[1], "2") == 0) {
		started = strtod(argv[2], NULL);
		into_the_tmpfs();
		note("tortos-update for the %s, off the root", DEVICE);
		update();
	}
	started = now();
	if (getpid() != 1) {
		fprintf(stderr, "tortos-update runs as init's restart action, never by hand\n");
		return 1;
	}
	off_the_root();
	return 0;
}
