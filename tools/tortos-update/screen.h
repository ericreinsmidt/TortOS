/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TORTOS_UPDATE_SCREEN_H
#define TORTOS_UPDATE_SCREEN_H

#include <stdbool.h>
#include <stdint.h>

/* What the updater shows while it writes (TortOS#69): "Writing" and then
 * "Checking", each with its percentage and a bar, under "Update to <version>";
 * and, if writing fails partway, the screen that says so and waits for the
 * power button. Drawn as TortOS's menus are, from pictures TortOS made
 * (art.h), on the device's framebuffer: straight on the Brick, scaled and
 * turned onto the GKD Pixel 2's portrait panel.
 *
 * Best effort throughout. A device whose screen can't be opened updates just
 * the same, with nothing drawn: no step of the update waits on the screen. */

/* The framebuffer opened, unblanked and shown from its first buffer, and the
 * backlight lit. False when there is no screen to draw on. */
bool screen_open(void);

/* Writing (phase 0) or Checking (phase 1) at pct percent, 0 to 100. Drawn
 * only when something on it has changed. */
void screen_progress(const char *version, int phase, int pct);

/* "The update didn't finish", then a wait for the power button: up to
 * `seconds`, so a device nobody is holding doesn't sit there lit until the
 * battery is empty. */
void screen_failed(void);
void screen_wait_power(int seconds);

/* How many times the progress was drawn and how long that took, for the
 * updater's log. */
void screen_cost(int *n, double *seconds);

/* The 1024x768 picture as last composed, RGBA, and the same turned and
 * scaled into a w x h framebuffer's layout (XRGB), for make updater-shots to
 * look at on a computer. */
const uint8_t *screen_picture(void);
void screen_fit(uint32_t *out, int w, int h);

#endif
