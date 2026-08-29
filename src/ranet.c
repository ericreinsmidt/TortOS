/* SPDX-License-Identifier: 0BSD */
/* See ranet.h for why this shells out to curl and why the request is a file. */
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ranet.h"

#define RA_URL "https://retroachievements.org/dorequest.php"

static char g_ca[512];

void ra_set_ca_path(const char *path)
{
	snprintf(g_ca, sizeof g_ca, "%s", path ? path : "");
}

static const char *curl_bin(void)
{
	static const char *tried[] = { "/usr/bin/curl", "/bin/curl", "curl" };
	static const char *found;
	size_t i;

	if (found) return found;
	for (i = 0; i < sizeof tried / sizeof tried[0]; i++) {
		if (access(tried[i], X_OK) == 0) { found = tried[i]; return found; }
	}
	return NULL;
}

/* Quote for curl's config-file syntax: values are double quoted, and inside
 * them only backslash and quote need escaping. Nothing here is attacker
 * controlled today - hashes, ids and a token - but a ROM title will end up in
 * one of these eventually and this is the cheap moment to be right. */
static void cfg_quote(FILE *f, const char *v)
{
	fputc('"', f);
	for (; *v; v++) {
		if (*v == '"' || *v == '\\') fputc('\\', f);
		fputc(*v, f);
	}
	fputc('"', f);
}

static bool write_config(const char *path, const ra_field *fl, int n,
                         int timeout_s)
{
	FILE *f;
	int fd, i;

	/* 0600 from the moment it exists: it carries the account token, and a
	 * file created readable and chmod'ed after has a window. */
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) return false;
	f = fdopen(fd, "w");
	if (!f) { close(fd); return false; }

	fprintf(f, "silent\nshow-error\nfail\n");
	fprintf(f, "max-time = %d\n", timeout_s);
	/* Verification stays ON. The device has no trust store of its own, which
	 * is the whole reason this file ships; -k would be the other way to make
	 * the handshake succeed and is not on the table for a request carrying a
	 * token. */
	if (g_ca[0]) { fprintf(f, "cacert = "); cfg_quote(f, g_ca); fputc('\n', f); }
	/* RA refuses curl's default agent with `unsupported_client`, which reads
	 * like a permissions problem and is not one. */
	fprintf(f, "user-agent = "); cfg_quote(f, "TortOS/" TORTOS_VERSION); fputc('\n', f);

	for (i = 0; i < n; i++) {
		char kv[1024];
		snprintf(kv, sizeof kv, "%s=%s", fl[i].k, fl[i].v ? fl[i].v : "");
		fprintf(f, "data-urlencode = ");
		cfg_quote(f, kv);
		fputc('\n', f);
	}
	fprintf(f, "url = "); cfg_quote(f, RA_URL); fputc('\n', f);
	fclose(f);
	return true;
}

/* Run curl with stdout on `out_fd`. Returns its exit status, or -1. */
static int run_curl(const char *cfg, int out_fd)
{
	const char *bin = curl_bin();
	pid_t pid;
	int st = -1;

	if (!bin) {
		fprintf(stderr, "ra: no curl on this device\n");
		return -1;
	}

	pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		char *argv[4];
		int devnull = open("/dev/null", O_RDONLY);

		if (devnull >= 0) { dup2(devnull, 0); close(devnull); }
		dup2(out_fd, 1);
		argv[0] = (char *)bin;
		argv[1] = (char *)"-K";
		argv[2] = (char *)cfg;
		argv[3] = NULL;
		execv(bin, argv);
		_exit(127);
	}
	if (waitpid(pid, &st, 0) != pid) return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void tmp_config(char *out, size_t n)
{
	snprintf(out, n, "/tmp/tortos-ra-%ld.curl", (long)getpid());
}

long ra_post_buf(const ra_field *f, int n, char *out, size_t outn, int timeout_s)
{
	char cfg[128], tmp[160];
	int fd, rc;
	long got = -1;

	if (!out || outn == 0) return -1;
	out[0] = '\0';
	tmp_config(cfg, sizeof cfg);
	snprintf(tmp, sizeof tmp, "%s.body", cfg);
	if (!write_config(cfg, f, n, timeout_s)) return -1;

	fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) { unlink(cfg); return -1; }

	rc = run_curl(cfg, fd);
	unlink(cfg);
	if (rc == 0) {
		ssize_t r;
		lseek(fd, 0, SEEK_SET);
		r = read(fd, out, outn - 1);
		if (r >= 0) { out[r] = '\0'; got = (long)r; }
	} else {
		fprintf(stderr, "ra: request failed (curl exit %d)\n", rc);
	}
	close(fd);
	unlink(tmp);
	return got;
}

bool ra_post_file(const ra_field *f, int n, const char *path, int timeout_s)
{
	char cfg[128], tmp[1024];
	struct stat st;
	int fd, rc;

	tmp_config(cfg, sizeof cfg);
	if (!write_config(cfg, f, n, timeout_s)) return false;

	/* Through a temporary in the destination directory, then rename. A set
	 * truncated by a dropped connection would otherwise sit in the cache
	 * looking complete, and the next launch would watch half a game. */
	snprintf(tmp, sizeof tmp, "%s.part", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) { unlink(cfg); return false; }

	rc = run_curl(cfg, fd);
	close(fd);
	unlink(cfg);

	if (rc != 0 || stat(tmp, &st) != 0 || st.st_size == 0) {
		if (rc != 0) fprintf(stderr, "ra: fetch failed (curl exit %d)\n", rc);
		unlink(tmp);
		return false;
	}
	if (rename(tmp, path) != 0) { unlink(tmp); return false; }
	return true;
}

bool ra_online(void)
{
	struct ifaddrs *ifa, *p;
	bool up = false;

	if (!curl_bin()) return false;
	if (getifaddrs(&ifa) != 0) return false;
	for (p = ifa; p; p = p->ifa_next) {
		if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
		if (!(p->ifa_flags & IFF_UP)) continue;
		if (p->ifa_flags & IFF_LOOPBACK) continue;
		up = true;
		break;
	}
	freeifaddrs(ifa);
	return up;
}
