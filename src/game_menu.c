/* The game info screen's rows. See src/game_menu.h. */
#include <stddef.h>

#include "game_menu.h"

int gi_rows(menu_row *out, const game_info *gi, bool net)
{
	int n = 0;

	out[n++] = (menu_row){ "File",     gi->file,    false };
	out[n++] = (menu_row){ "Size",     gi->size,    false };
	out[n++] = (menu_row){ "Saves",    gi->saves,   false };
	out[n++] = (menu_row){ "Cheevos",  gi->cheevos, false };
	out[n++] = (menu_row){ "Box Art",  gi->art,     false };
	/* The two live rows last, under the facts, because they act on them. */
	out[n++] = (menu_row){ gi->has_art ? "Replace Box Art" : "Get Box Art",
	                       net ? NULL : "needs Wi-Fi", net };
	out[n++] = (menu_row){ "Favorite", gi->favorite ? "yes" : "no", true };
	return n;
}
