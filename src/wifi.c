/* SPDX-License-Identifier: 0BSD */
/* WiFi over the stock wpa_supplicant. See wifi.h for the position: none of
 * this is TortOS's own networking, it is a client of firmware that already
 * works.
 *
 * Everything runs through fork/execv rather than popen, and that is not
 * fastidiousness. An SSID is 32 arbitrary octets chosen by whoever owns the
 * access point, it arrives here from the air, and this process is root. A
 * network named `"; rm -rf / #` handed to a shell is a remote command
 * execution from anyone standing near the device. execv passes arguments as
 * a vector and never parses them, which removes the whole class rather than
 * escaping around it -- and it fixes the ordinary bugs too, since SSIDs with
 * spaces and quotes in them are common and legal.
 *
 * The passphrase still reaches wpa_cli as an argument, so it is briefly
 * visible in /proc/<pid>/cmdline. Closing that means speaking the control
 * protocol over its unix socket directly instead of shelling out at all.
 * Worth doing if this device ever runs anything untrusted; today the only
 * processes on it are the launcher, the emulator and the stock firmware, and
 * anything that could read that cmdline is already root.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wifi.h"

#ifdef __linux__

#include <sys/wait.h>
#include <unistd.h>

#define WPA_CLI  "/usr/sbin/wpa_cli"
#define WPA_SOCK "/etc/wifi/sockets"
#define WLAN     "wlan0"

/* Run a command, capture stdout, return its exit status (-1 if it could not
 * be run at all). `out` is always NUL-terminated. */
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

/* wpa_cli with the socket and interface always supplied, because forgetting
 * either makes it talk to a default path that does not exist here and fail in
 * a way that looks like "no wifi" rather than "wrong arguments". */
static int wpa(char *out, size_t cap, const char *a, const char *b,
               const char *c, const char *d)
{
	char *argv[10];
	int n = 0;

	argv[n++] = (char *)WPA_CLI;
	argv[n++] = (char *)"-p";
	argv[n++] = (char *)WPA_SOCK;
	argv[n++] = (char *)"-i";
	argv[n++] = (char *)WLAN;
	if (a) argv[n++] = (char *)a;
	if (b) argv[n++] = (char *)b;
	if (c) argv[n++] = (char *)c;
	if (d) argv[n++] = (char *)d;
	argv[n] = NULL;
	return run(argv, out, cap);
}

static bool wpa_ok(const char *a, const char *b, const char *c, const char *d)
{
	char out[128];
	return wpa(out, sizeof out, a, b, c, d) == 0 && strncmp(out, "OK", 2) == 0;
}

/* One value out of `key=value` output. */
static bool kv(const char *text, const char *key, char *out, int cap)
{
	size_t klen = strlen(key);
	const char *p = text;

	while (p && *p) {
		if (!strncmp(p, key, klen) && p[klen] == '=') {
			const char *v = p + klen + 1;
			const char *e = strchr(v, '\n');
			int n = e ? (int)(e - v) : (int)strlen(v);
			if (n >= cap) n = cap - 1;
			memcpy(out, v, (size_t)n);
			out[n] = '\0';
			return true;
		}
		p = strchr(p, '\n');
		if (p) p++;
	}
	return false;
}

bool wifi_up(void)
{
	char out[128];
	int i;

	if (wpa(out, sizeof out, "ping", NULL, NULL, NULL) == 0 &&
	    strstr(out, "PONG")) return true;

	{
		char *argv[] = { (char *)"/etc/init.d/wpa_supplicant",
		                 (char *)"start", NULL };
		run(argv, NULL, 0);
	}
	/* The stock start_service retries `ifconfig wlan0 up` five times with
	 * usleep 500000 between, so the socket is not there the instant this
	 * returns. Wait for the socket rather than for a guessed duration. */
	for (i = 0; i < 20; i++) {
		sleep(1);
		if (wpa(out, sizeof out, "ping", NULL, NULL, NULL) == 0 &&
		    strstr(out, "PONG")) return true;
	}
	return false;
}

void wifi_down(void)
{
	char *argv[] = { (char *)"/etc/init.d/wpa_supplicant",
	                 (char *)"stop", NULL };
	run(argv, NULL, 0);
}

/* Saved networks, so the list can say which ones are already known. */
static int known_ssids(char list[][WIFI_SSID_MAX], int max)
{
	char out[4096];
	char *line, *save;
	int n = 0;

	if (wpa(out, sizeof out, "list_networks", NULL, NULL, NULL) != 0) return 0;
	for (line = strtok_r(out, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char *tab = strchr(line, '\t');
		char *end;
		if (!tab) continue;                       /* the header has no tabs */
		if (!strncmp(line, "network id", 10)) continue;
		end = strchr(++tab, '\t');
		if (end) *end = '\0';
		if (n < max) snprintf(list[n++], WIFI_SSID_MAX, "%s", tab);
	}
	return n;
}

int wifi_scan(wifi_net *out, int max)
{
	char buf[8192];
	char known[WIFI_MAX_NETS][WIFI_SSID_MAX];
	char *line, *save;
	int n = 0, nknown, i;

	if (wpa(buf, sizeof buf, "ping", NULL, NULL, NULL) != 0 ||
	    !strstr(buf, "PONG")) return -1;

	wpa(buf, sizeof buf, "scan", NULL, NULL, NULL);

	/* Poll until the result count settles rather than sleeping a guessed
	 * duration. A scan fills in over several seconds and the weak entries
	 * arrive last: measured 2026-08-29, a flat 3-second wait returned one
	 * network where the radio could actually see two, and the one it dropped
	 * was the weaker of the pair -- exactly the network a user is most likely
	 * to be squinting at the list for. Two identical reads in a row is the
	 * signal that it has finished; the cap stops a radio that never settles
	 * from hanging the UI. */
	{
		int last = -1, stable = 0, t;
		for (t = 0; t < 9; t++) {
			int c = 0;
			const char *q;
			sleep(1);
			if (wpa(buf, sizeof buf, "scan_results", NULL, NULL, NULL) != 0)
				continue;
			for (q = buf; *q; q++) if (*q == '\n') c++;
			if (c > 0 && c == last) { if (++stable >= 2) break; }
			else { stable = 0; last = c; }
		}
		if (last <= 0) return 0;                  /* radio up, nothing heard */
	}
	nknown = known_ssids(known, WIFI_MAX_NETS);

	for (line = strtok_r(buf, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char *f[5], *p = line;
		int col = 0, sig;
		wifi_net e;

		if (!strncmp(line, "bssid", 5)) continue; /* header */
		/* bssid \t freq \t signal \t flags \t ssid, and the ssid is last
		 * because it may itself contain anything except a tab. */
		while (col < 5) {
			f[col++] = p;
			p = strchr(p, '\t');
			if (!p) break;
			*p++ = '\0';
		}
		if (col < 5) continue;                    /* hidden: no ssid column */
		if (!f[4][0]) continue;

		memset(&e, 0, sizeof e);
		snprintf(e.ssid, sizeof e.ssid, "%s", f[4]);
		sig = atoi(f[2]);
		e.signal = sig;
		e.secured = strstr(f[3], "WPA") || strstr(f[3], "WEP");
		for (i = 0; i < nknown; i++)
			if (!strcmp(known[i], e.ssid)) { e.known = true; break; }

		/* One row per SSID. An access point with two radios answers twice and
		 * a mesh answers once per node; showing the same name three times
		 * would be a list of hardware, not a list of networks. Strongest
		 * wins, because that is the one it will actually associate with. */
		for (i = 0; i < n; i++)
			if (!strcmp(out[i].ssid, e.ssid)) break;
		if (i < n) {
			if (e.signal > out[i].signal) out[i].signal = e.signal;
			continue;
		}
		if (n < max) out[n++] = e;
	}

	/* Strongest first. n is at most WIFI_MAX_NETS, so an insertion sort is
	 * the right amount of machinery. */
	for (i = 1; i < n; i++) {
		wifi_net t = out[i];
		int j = i - 1;
		while (j >= 0 && out[j].signal < t.signal) { out[j + 1] = out[j]; j--; }
		out[j + 1] = t;
	}
	return n;
}

wifi_state wifi_status(char *ssid, int ssid_cap, char *ip, int ip_cap)
{
	char out[2048], state[64];

	if (ssid && ssid_cap) ssid[0] = '\0';
	if (ip && ip_cap) ip[0] = '\0';

	if (wpa(out, sizeof out, "status", NULL, NULL, NULL) != 0) return WIFI_OFF;
	if (!kv(out, "wpa_state", state, sizeof state)) return WIFI_OFF;

	if (ssid && ssid_cap) kv(out, "ssid", ssid, ssid_cap);
	if (ip && ip_cap) kv(out, "ip_address", ip, ip_cap);

	if (!strcmp(state, "COMPLETED"))  return WIFI_CONNECTED;
	if (!strcmp(state, "SCANNING") || !strcmp(state, "DISCONNECTED") ||
	    !strcmp(state, "INACTIVE"))   return WIFI_IDLE;
	return WIFI_CONNECTING;
}

/* The id wpa_supplicant already has for this SSID, or -1. */
static int saved_id(const char *ssid, char *out_id, size_t cap)
{
	char out[4096];
	char *line, *save;

	if (wpa(out, sizeof out, "list_networks", NULL, NULL, NULL) != 0) return -1;
	for (line = strtok_r(out, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char *tab = strchr(line, '\t'), *end;
		if (!tab || !strncmp(line, "network id", 10)) continue;
		*tab = '\0';
		end = strchr(++tab, '\t');
		if (end) *end = '\0';
		if (strcmp(tab, ssid)) continue;
		snprintf(out_id, cap, "%.15s", line);
		return 0;
	}
	return -1;
}

bool wifi_connect(const char *ssid, const char *psk)
{
	char out[256], id[16], quoted[WIFI_SSID_MAX + 4], qpsk[80];
	bool reused = false;
	int i;

	if (!ssid || !*ssid) return false;
	if (!wifi_up()) return false;

	/* An already-saved network is joined by its id. Creating a second entry
	 * for it and handing that entry no passphrase produced a key_mgmt=NONE
	 * config - an OPEN network - pointed at a WPA2 access point, which cannot
	 * associate. That is what "connect to a saved network" did until
	 * 2026-08-29, and it failed every time. */
	if (psk && *psk) {
		/* A new passphrase for a known SSID replaces the old entry rather
		 * than sitting beside it, or the stale one competes to associate. */
		if (saved_id(ssid, id, sizeof id) == 0)
			wpa(out, sizeof out, "remove_network", id, NULL, NULL);
	} else if (saved_id(ssid, id, sizeof id) == 0) {
		reused = true;
	}

	if (!reused && wpa(out, sizeof out, "add_network", NULL, NULL, NULL) != 0)
		return false;
	if (!reused)
	{	/* the id is the last line, and there may be a status line above it */
		char *nl = strrchr(out, '\n');
		char *v = out;
		while (nl && (nl == out || nl[1] == '\0')) { *nl = '\0'; nl = strrchr(out, '\n'); }
		if (nl) v = nl + 1;
		snprintf(id, sizeof id, "%.15s", v);
	}
	if (!id[0] || id[0] < '0' || id[0] > '9') return false;

	/* wpa_supplicant wants the value quoted INSIDE the argument: the quotes
	 * are part of the config syntax, not shell quoting, and execv would never
	 * have stripped them anyway. */
	if (!reused) {
		snprintf(quoted, sizeof quoted, "\"%s\"", ssid);
		if (!wpa_ok("set_network", id, "ssid", quoted)) goto fail;

		if (psk && *psk) {
			snprintf(qpsk, sizeof qpsk, "\"%s\"", psk);
			if (!wpa_ok("set_network", id, "psk", qpsk)) goto fail;
		} else {
			/* Genuinely open, and only because the scan said so. */
			if (!wpa_ok("set_network", id, "key_mgmt", "NONE")) goto fail;
		}
	}

	if (!wpa_ok("enable_network", id, NULL, NULL)) goto fail;
	if (!wpa_ok("select_network", id, NULL, NULL)) goto fail;

	/* Association is not instant and a wrong passphrase looks exactly like a
	 * slow one until the supplicant gives up, so this waits rather than
	 * reporting a success the moment the command was accepted. */
	for (i = 0; i < 20; i++) {
		sleep(1);
		if (wifi_status(NULL, 0, NULL, 0) == WIFI_CONNECTED) break;
	}
	if (wifi_status(NULL, 0, NULL, 0) != WIFI_CONNECTED) goto fail;

	/* Only now is the credential worth keeping. Saving before association
	 * would fill the config with passwords that never worked. */
	wpa(out, sizeof out, "save_config", NULL, NULL, NULL);

	{	/* the vendor's own invocation, from /etc/wifi/udhcpc_wlan0 */
		char *argv[] = { (char *)"/sbin/udhcpc", (char *)"-i", (char *)WLAN,
		                 (char *)"-S", (char *)"-t", (char *)"5",
		                 (char *)"-T", (char *)"7", (char *)"-b",
		                 (char *)"-q", NULL };
		run(argv, NULL, 0);
	}
	return true;

fail:
	/* select_network disables every OTHER network, so a failed attempt used
	 * to leave the whole saved list switched off - one bad try and the device
	 * never rejoined anything again, silently, with wpa_state sitting at
	 * INACTIVE and the saved entry marked [DISABLED]. Put them back. */
	if (!reused) wpa(out, sizeof out, "remove_network", id, NULL, NULL);
	wpa(out, sizeof out, "enable_network", "all", NULL, NULL);
	return false;
}

bool wifi_forget(const char *ssid)
{
	char out[4096];
	char *line, *save;
	bool hit = false;

	if (wpa(out, sizeof out, "list_networks", NULL, NULL, NULL) != 0) return false;
	for (line = strtok_r(out, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char *tab = strchr(line, '\t');
		char *end, id[16];
		if (!tab || !strncmp(line, "network id", 10)) continue;
		*tab = '\0';
		snprintf(id, sizeof id, "%.15s", line);
		end = strchr(++tab, '\t');
		if (end) *end = '\0';
		if (strcmp(tab, ssid)) continue;
		if (wpa_ok("remove_network", id, NULL, NULL)) hit = true;
	}
	if (hit) {
		char tmp[128];
		wpa(tmp, sizeof tmp, "save_config", NULL, NULL, NULL);
	}
	return hit;
}

#else  /* host build: no radio, and the shelf still has to run */

bool wifi_up(void) { return false; }
void wifi_down(void) { }
int wifi_scan(wifi_net *out, int max) { (void)out; (void)max; return -1; }
bool wifi_connect(const char *ssid, const char *psk)
{
	(void)ssid; (void)psk; return false;
}
wifi_state wifi_status(char *ssid, int ssid_cap, char *ip, int ip_cap)
{
	if (ssid && ssid_cap) ssid[0] = '\0';
	if (ip && ip_cap) ip[0] = '\0';
	return WIFI_OFF;
}
bool wifi_forget(const char *ssid) { (void)ssid; return false; }

#endif
