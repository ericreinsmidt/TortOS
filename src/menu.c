/* The one piece of menu behavior that is pure enough to live away from the
 * renderer: where the cursor goes next. See src/menu.h. */
#include "menu.h"

int menu_step_sel(const menu_row *rows, int n, int sel, int dir)
{
	int i, k = sel;

	if (n <= 0) return 0;
	for (i = 0; i < n; i++) {
		k = (k + dir + n) % n;
		if (rows[k].live) return k;
	}
	return sel;
}
