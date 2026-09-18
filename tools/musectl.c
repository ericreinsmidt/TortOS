/* SPDX-License-Identifier: MIT */
/* Talk to Muse by hand, over ssh, with no screen in front of it.
 *
 *   musectl [-t SECONDS] 'PLAY<TAB>path=/mnt/SDCARD/Music/x.mp3' ...
 *
 * Each argument is sent as one line - a literal "\t" in an argument is turned
 * into a tab, since typing a real one through ssh and a shell is miserable -
 * and then whatever Muse says is printed for SECONDS (default 2).
 *
 * An instrument, like keyinject: the device's BusyBox nc cannot open a Unix
 * socket, and this is the only way to reach the daemon before the launcher
 * speaks to it. */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	double secs = 2.0;
	int fd, i = 1;
	time_t end;
	char buf[4096];

	if (argc > 2 && !strcmp(argv[1], "-t")) { secs = atof(argv[2]); i = 3; }
	snprintf(sa.sun_path, sizeof sa.sun_path, "/tmp/muse.sock");
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		perror("muse is not listening on /tmp/muse.sock");
		return 1;
	}
	for (; i < argc; i++) {
		char line[2048];
		size_t n = 0;
		const char *p;

		for (p = argv[i]; *p && n + 2 < sizeof line; p++) {
			if (p[0] == '\\' && p[1] == 't') { line[n++] = '\t'; p++; }
			else line[n++] = *p;
		}
		line[n++] = '\n';
		if (write(fd, line, n) < 0) { perror("write"); return 1; }
	}
	end = time(NULL) + (time_t)(secs + 0.999);
	while (time(NULL) < end) {
		struct pollfd p = { fd, POLLIN, 0 };
		ssize_t got;

		if (poll(&p, 1, 100) <= 0) continue;
		got = read(fd, buf, sizeof buf - 1);
		if (got <= 0) break;
		buf[got] = '\0';
		fputs(buf, stdout);
		fflush(stdout);
	}
	close(fd);
	return 0;
}
