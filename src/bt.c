/* SPDX-License-Identifier: MIT */
/* See bt.h. fork/execv rather than popen, for the reason wifi.c gives at
 * length: a device NAME is arbitrary bytes chosen by whoever owns the headset
 * and it arrives here over the air, into a process running as root. execv
 * passes a vector and never parses it, which removes the class rather than
 * escaping around it. A MAC is validated besides, because it is the one thing
 * here that is ever passed back out as an argument.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bt.h"

#define BLUETOOTHCTL "/usr/bin/bluetoothctl"
#define BONDS        "/etc/lib/bluetooth"

/* AA:BB:CC:DD:EE:FF and nothing else. Everything that reaches bluetoothctl as
 * an argument goes through this first: a name can be arbitrary, an address
 * cannot, and refusing what is not an address is cheaper than trusting the
 * source of one. */
/* The first 17 bytes only, so a line like "Device AA:.. Some Name" can be
 * validated where it sits without being copied out first. */
static bool mac_ok_prefix(const char *m)
{
	int i;

	if (!m) return false;
	for (i = 0; i < 17; i++) {
		if (!m[i]) return false;
		if (i % 3 == 2) { if (m[i] != ':') return false; continue; }
		if (!((m[i] >= '0' && m[i] <= '9') ||
		      (m[i] >= 'A' && m[i] <= 'F') ||
		      (m[i] >= 'a' && m[i] <= 'f'))) return false;
	}
	return true;
}

/* Exactly an address and nothing after it. Everything that reaches
 * bluetoothctl as an argument goes through this: a name can be arbitrary, an
 * address cannot, and refusing what is not one is cheaper than trusting where
 * it came from. */
bool bt_mac_valid(const char *m)
{
	return m && strlen(m) == 17 && mac_ok_prefix(m);
}

void bt_pcm_name(const char *mac, char *out, size_t n)
{
	size_t i;

	if (!out || !n) return;
	/* An address is 17 characters and the caller's buffer is sized for one.
	 * Bounded anyway: this is reached with whatever a bond directory was
	 * named, and a longer name should be cut rather than assumed away. */
	snprintf(out, n, "bt_%.17s", mac ? mac : "");
	for (i = 0; i < n && out[i]; i++)
		if (out[i] == ':') out[i] = '_';
}

/* "Device AA:BB:CC:DD:EE:FF Some Name" - one per line. */
int bt_parse_devices(const char *text, bt_device *out, int max)
{
	const char *p = text;
	int n = 0;

	while (p && *p && n < max) {
		const char *eol = strchr(p, '\n');
		size_t len = eol ? (size_t)(eol - p) : strlen(p);
		char line[256];

		snprintf(line, sizeof line, "%.*s", (int)(len < sizeof line ? len : sizeof line - 1), p);
		p = eol ? eol + 1 : NULL;

		if (strncmp(line, "Device ", 7)) continue;
		if (!mac_ok_prefix(line + 7)) continue;
		snprintf(out[n].mac, BT_MAC_MAX, "%.17s", line + 7);
		/* Truncated deliberately, and bounded so the device compiler can see
		 * it is: a headset may advertise a name far longer than a menu row. */
		snprintf(out[n].name, BT_NAME_MAX, "%.*s", BT_NAME_MAX - 1,
		         line[24] ? line + 25 : out[n].mac);
		out[n].bonded = false;
		out[n].connected = false;
		n++;
	}
	return n;
}

void bt_label(const bt_device *d, char *out, size_t n)
{
	if (!out || !n) return;
	if (!d)           { out[0] = '\0'; return; }
	if (d->connected)   snprintf(out, n, "connected");
	else if (d->bonded) snprintf(out, n, "paired");
	else                snprintf(out, n, "in range");
}

/* "\t> ACL A8:F5:E1:4A:93:71 handle 128 state 1 lm MASTER AUTH ENCRYPT" */
int bt_mark_connected(const char *text, bt_device *list, int n)
{
	const char *p = text;
	int marked = 0, i;

	if (!text || !list) return 0;
	for (i = 0; i < n; i++) list[i].connected = false;

	while (p && *p) {
		const char *eol = strchr(p, '\n');
		size_t len = eol ? (size_t)(eol - p) : strlen(p);
		char line[256];
		const char *acl;

		snprintf(line, sizeof line, "%.*s",
		         (int)(len < sizeof line ? len : sizeof line - 1), p);
		p = eol ? eol + 1 : NULL;

		/* ACL only. A SCO link is the headset's microphone channel and says
		 * nothing about whether audio is going out to it. */
		if (!(acl = strstr(line, "ACL "))) continue;
		if (!mac_ok_prefix(acl + 4)) continue;
		for (i = 0; i < n; i++)
			if (!strncasecmp(list[i].mac, acl + 4, 17)) {
				list[i].connected = true;
				marked++;
				break;
			}
	}
	return marked;
}

#ifdef __linux__

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int run(char *const argv[], char *out, size_t cap)
{
	int fd[2], status = -1;
	pid_t pid;
	size_t used = 0;

	if (out && cap) out[0] = '\0';
	if (pipe(fd) < 0) return -1;
	pid = fork();
	if (pid < 0) { close(fd[0]); close(fd[1]); return -1; }
	if (pid == 0) {
		close(fd[0]);
		dup2(fd[1], STDOUT_FILENO);
		dup2(fd[1], STDERR_FILENO);
		close(fd[1]);
		execv(argv[0], argv);
		_exit(127);
	}
	close(fd[1]);
	for (;;) {
		ssize_t n;
		char scratch[256];
		char *dst = (out && used + 1 < cap) ? out + used : scratch;
		size_t room = (out && used + 1 < cap) ? cap - used - 1 : sizeof scratch;
		n = read(fd[0], dst, room);
		if (n <= 0) break;
		if (dst != scratch) used += (size_t)n;
	}
	if (out && cap) out[used] = '\0';
	close(fd[0]);
	if (waitpid(pid, &status, 0) < 0) return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* bluetoothctl with the agent always supplied. A headset will not pair without
 * one, and bluetoothctl registers a default agent only in interactive mode -
 * which is the single fact that made pairing work at all. */
static int btctl(char *out, size_t cap, const char *a, const char *b)
{
	char *argv[8];
	int n = 0;

	argv[n++] = (char *)BLUETOOTHCTL;
	argv[n++] = (char *)"--agent";
	argv[n++] = (char *)"NoInputNoOutput";
	if (a) argv[n++] = (char *)a;
	if (b) argv[n++] = (char *)b;
	argv[n] = NULL;
	return run(argv, out, cap);
}

/* One call for all of them. See bt.h for the two cheaper-looking sources this
 * replaced and why each was wrong. */
int bt_mark_connected_now(bt_device *list, int n)
{
	char con[2048];
	char *argv[] = { (char *)"/usr/bin/hcitool", (char *)"con", NULL };

	if (run(argv, con, sizeof con) != 0) return 0;
	return bt_mark_connected(con, list, n);
}

bt_state bt_status(void)
{
	char out[512];

	if (access("/sys/class/bluetooth/hci0", F_OK) != 0) return BT_NO_ADAPTER;
	if (btctl(out, sizeof out, "show", NULL) != 0) return BT_NO_ADAPTER;
	return strstr(out, "Powered: yes") ? BT_READY : BT_POWERED_OFF;
}

/* Read Name= out of a bond's info file. */
static bool bond_name(const char *dir, const char *mac, char *out, size_t n)
{
	char path[600], line[256];
	FILE *f;
	bool trusted = false, named = false;

	snprintf(path, sizeof path, "%s/%s/info", dir, mac);
	if (!(f = fopen(path, "r"))) return false;
	snprintf(out, n, "%s", mac);
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (!strncmp(line, "Name=", 5) && line[5]) {
			/* Truncated on purpose: a headset can advertise a name far
			 * longer than a menu row, and a short one is what the screen
			 * wants anyway. Bounded explicitly so it is not a warning. */
			snprintf(out, n, "%.*s", (int)n - 1, line + 5);
			named = true;
		} else if (!strcmp(line, "Trusted=true")) {
			trusted = true;
		}
	}
	fclose(f);
	(void)named;
	return trusted;
}

int bt_bonded(bt_device *out, int max)
{
	DIR *ad;
	struct dirent *a;
	int n = 0;

	if (!out || max <= 0) return 0;
	if (!(ad = opendir(BONDS))) return 0;
	while ((a = readdir(ad)) && n < max) {
		char adapter[512];
		DIR *dd;
		struct dirent *d;

		if (a->d_name[0] == '.') continue;
		snprintf(adapter, sizeof adapter, "%s/%s", BONDS, a->d_name);
		if (!(dd = opendir(adapter))) continue;
		while ((d = readdir(dd)) && n < max) {
			if (!bt_mac_valid(d->d_name)) continue;
			if (!bond_name(adapter, d->d_name, out[n].name, BT_NAME_MAX))
				continue;                     /* bonded but not trusted */
			snprintf(out[n].mac, BT_MAC_MAX, "%s", d->d_name);
			out[n].bonded = true;
			out[n].connected = false;
			n++;
		}
		closedir(dd);
	}
	closedir(ad);
	return n;
}

int bt_visible(bt_device *out, int max)
{
	char text[4096];
	int n, i, j, nb;
	bt_device bond[BT_MAX];

	if (!out || max <= 0) return 0;
	if (btctl(text, sizeof text, "devices", NULL) != 0) return bt_bonded(out, max);
	n = bt_parse_devices(text, out, max);

	/* Mark the ones that are bonded, so the screen can say which will come
	 * back on their own and which are merely in range. */
	nb = bt_bonded(bond, BT_MAX);
	for (i = 0; i < n; i++)
		for (j = 0; j < nb; j++)
			if (!strcasecmp(out[i].mac, bond[j].mac)) {
				out[i].bonded = true;
				if (bond[j].name[0]) snprintf(out[i].name, BT_NAME_MAX, "%s", bond[j].name);
				break;
			}

	bt_mark_connected_now(out, n);

	/* A bonded device out of range does not appear in `devices` at all, and
	 * leaving it out would make forgetting one impossible. */
	for (j = 0; j < nb && n < max; j++) {
		for (i = 0; i < n; i++)
			if (!strcasecmp(out[i].mac, bond[j].mac)) break;
		if (i == n) out[n++] = bond[j];
	}
	return n;
}

bool bt_scan(int secs)
{
	pid_t pid;
	char timeout[16];

	if (secs < 1) secs = 1;
	snprintf(timeout, sizeof timeout, "%d", secs);

	/* Double-forked and never waited on: `scan on` runs for the timeout and
	 * the UI must not stop for it. The same shape wifi.c uses for its own
	 * slow calls. */
	pid = fork();
	if (pid < 0) return false;
	if (pid == 0) {
		if (fork() == 0) {
			char *argv[] = { (char *)BLUETOOTHCTL, (char *)"--timeout",
			                 timeout, (char *)"scan", (char *)"on", NULL };
			int null = open("/dev/null", O_RDWR);
			if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); }
			execv(argv[0], argv);
			_exit(127);
		}
		_exit(0);
	}
	waitpid(pid, NULL, 0);
	return true;
}

/* `connect` reports Failed for a2dp even when the link came up, so success is
 * read from `info` rather than from the exit status. That is the single most
 * expensive thing anyone learned about this stack. */
static bool info_says(const char *mac, const char *needle)
{
	char out[1024];

	if (!bt_mac_valid(mac)) return false;
	if (btctl(out, sizeof out, "info", mac) < 0) return false;
	return strstr(out, needle) != NULL;
}

bool bt_pair(const char *mac, char *err, size_t n)
{
	char out[1024];

	if (err && n) err[0] = '\0';
	if (!bt_mac_valid(mac)) { if (err) snprintf(err, n, "not an address"); return false; }

	btctl(out, sizeof out, "pair", mac);
	if (!info_says(mac, "Paired: yes")) {
		if (err) snprintf(err, n, "%s", strstr(out, "AuthenticationFailed")
		                  ? "the headset refused the pairing"
		                  : "pairing did not complete");
		return false;
	}
	/* Trusted, or it will not reconnect on its own at the next boot - which
	 * is the whole reason the bond is worth having. */
	btctl(out, sizeof out, "trust", mac);
	return true;
}

bool bt_connect(const char *mac, char *err, size_t n)
{
	char out[1024];

	if (err && n) err[0] = '\0';
	if (!bt_mac_valid(mac)) { if (err) snprintf(err, n, "not an address"); return false; }
	btctl(out, sizeof out, "connect", mac);
	if (info_says(mac, "Connected: yes")) return true;
	if (err) snprintf(err, n, "it did not connect");
	return false;
}

bool bt_power(bool on)
{
	char out[512];
	return btctl(out, sizeof out, "power", on ? "on" : "off") == 0;
}

bool bt_disconnect(const char *mac)
{
	char out[512];
	if (!bt_mac_valid(mac)) return false;
	btctl(out, sizeof out, "disconnect", mac);
	return !info_says(mac, "Connected: yes");
}

bool bt_forget(const char *mac)
{
	char out[512];
	if (!bt_mac_valid(mac)) return false;
	btctl(out, sizeof out, "remove", mac);
	return !info_says(mac, "Paired: yes");
}

bool bt_asoundrc(const char *userdata_dir)
{
	char path[512], tmp[520], pcm[BT_MAC_MAX + 8];
	bt_device d[BT_MAX];
	int n, i;
	FILE *f;

	if (!userdata_dir || !*userdata_dir) return false;
	snprintf(path, sizeof path, "%s/.asoundrc", userdata_dir);
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	if (!(f = fopen(tmp, "w"))) return false;

	n = bt_bonded(d, BT_MAX);
	for (i = 0; i < n; i++) {
		bt_pcm_name(d[i].mac, pcm, sizeof pcm);
		fprintf(f, "pcm.%s {\n    type bluealsa\n    device \"%s\"\n"
		           "    profile \"a2dp\"\n}\n", pcm, d[i].mac);
	}
	/* `default` is deliberately not redefined: ALSA reads this in addition to
	 * /etc/asound.conf, and the speaker path must stay exactly as it was. */
	if (fclose(f) != 0) { unlink(tmp); return false; }
	if (rename(tmp, path) != 0) { unlink(tmp); return false; }
	return true;
}

#else   /* not __linux__ */

bt_state bt_status(void) { return BT_NO_ADAPTER; }
int  bt_bonded(bt_device *out, int max) { (void)out; (void)max; return 0; }
int  bt_visible(bt_device *out, int max) { (void)out; (void)max; return 0; }
bool bt_scan(int secs) { (void)secs; return false; }
bool bt_pair(const char *m, char *e, size_t n) { (void)m; if (e && n) e[0] = 0; return false; }
bool bt_connect(const char *m, char *e, size_t n) { (void)m; if (e && n) e[0] = 0; return false; }
int  bt_mark_connected_now(bt_device *l, int n) { (void)l; (void)n; return 0; }
bool bt_power(bool on) { (void)on; return false; }
bool bt_disconnect(const char *m) { (void)m; return false; }
bool bt_forget(const char *m) { (void)m; return false; }
bool bt_asoundrc(const char *d) { (void)d; return false; }

#endif
