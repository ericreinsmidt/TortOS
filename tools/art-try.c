#include <stdio.h>
#include <unistd.h>
#include "artscrape.h"
#include "net.h"
int main(int argc, char **argv)
{
	art_progress p;
	systems_cfg sys;
	if (argc < 3) return 2;
	if (!cfg_load_systems(argv[1], &sys)) { fprintf(stderr, "no systems.cfg\n"); return 1; }
	net_set_ca_path(argc > 3 ? argv[3] : "");
	art_begin(&sys, argv[2]);
	for (;;) {
		int r = art_step();
		art_status(&p);
		if (r == 0) break;
		if (r < 0) { fprintf(stderr, "could not start\n"); return 1; }
		usleep(4000);
	}
	printf("systems %d/%d  found %d  missing %d  already %d\n",
	       p.systems_done, p.systems, p.found, p.missing, p.skipped);
	return 0;
}
