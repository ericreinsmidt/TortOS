# Text menus

Every list-shaped screen in TortOS is drawn by one function, `menu_draw` in
`src/main.c`. This is what a menu is, what the pieces mean, and which rules a
new screen has to follow. It exists because on 2026-09-04 a single evening
turned up five defects that were all the same defect: `menu_draw` abstracts
*drawing* a menu and nothing abstracts *being* one, so each screen re-implements
the rules by hand and each can forget them independently.

## Row kinds

A `menu_row` is `{ label, value, live }`. Four kinds are built from that, and a
screen should not invent a fifth without adding it here.

| Kind | How | Reads as |
|---|---|---|
| **Item** | `{ "Wi-Fi", "on", true }` | something you can do |
| **Placeholder** | `{ "No networks found", NULL, false }` | a real place on the list that does nothing *yet* |
| **Rule** | `MENU_RULE` | a divider; the list ends here |
| **Note** | `MENU_NOTE("Y: rescan")` | text *about* the list, not in it |

A placeholder is still a row: it highlights, one step quieter than a live row.
That quietness is the whole signal that it does nothing, so do not use a
placeholder for text that is not a would-be item - use a note.

A rule reuses the exact bar `menu_draw` puts under the heading, at the same
alpha and width. That is why it lives in `menu_draw` and not in a caller: a line
painted by a screen would drift from the heading's the first time either was
retuned, which is the same reason `menu_draw` exists at all.

A note is centered, because it describes the list rather than being an item in
it. Left-aligned it reads as one more row you have failed to be able to select.

## Rules a screen must follow

**Step over dead rows.** `live == false` means "does nothing". Navigation that
walks every row lets the cursor rest on one, and then A does nothing and the
screen looks broken. Screens on `menu_run` get this for free.

Audited 2026-09-04, and this file previously overstated it. Most `menu_draw`
callers pass `sel = -1`: they are panels with no cursor, and the bug cannot
reach them. Only four screens move a cursor over a row list - `game_info_screen`,
`tortos_menu`, `cheevos_screen` and `game_menu` - and they still walk every row
with `(sel + 1) % n`. They are only actually wrong where they have a dead row to
land on, which is not all of them.

Do not write the walk by hand in a new screen. Use `menu_run`, where the step is
`menu_step_sel` - a **bounded** walk, not a `do/while`. A menu can legitimately
have no live rows at all ("No networks found" under a rule is exactly that) and
the unbounded form spins forever on one.

**Say which buttons do anything.** If a screen binds a key that is not A or B,
it needs a note saying so. `Y` rescanned the Wi-Fi list from the day that screen
was written and nothing on screen ever said so, which means it was a feature
available only to people who already knew about it.

**Confirm anything destructive, and default to Cancel.** `confirm_panel` takes a
question and a verb and returns a bool. It starts on Cancel so a stray press of
the button that opened it cannot also answer it, and it handles power and idle
itself, because a confirm can sit on screen indefinitely and that is exactly
where a device gets put down.

**Name the consequence in the confirm, do not refuse the action.** Forgetting
the Wi-Fi network you are connected to drops the device off the LAN. The screen
allows it and says so, rather than making the button silently do nothing on that
one row - a control that appears broken is worse than one that warns.

**Never paint menu chrome in a screen.** Panels, rules, plates, text fitting and
truncation all belong to `menu_draw`. Callers used to call `ui_fit_text`
themselves, or forget to, and rows drew through each other. A rule about how a
menu looks belongs in the thing that draws menus.

## `menu_measure` is a second copy, and must agree

`menu_measure` duplicates `menu_draw`'s width pass so a caller can size a panel
without drawing it. It is maintained separately and the two must know the same
row kinds. The first use of `MENU_RULE` would have called `ui_text_width` on a
NULL label there; a panel sized by one set of rules and drawn by another is a
bug waiting whatever the symptom turns out to be.

**If you add a row kind, change both.**

## The runner, and what is still on the old shape

`menu_run` owns the loop: polling, quit, power, idle, back, the cursor and the
frame. A screen supplies `build` and `on_key` and cannot forget any of it. See
[ADR-0001](decisions/0001-screens-declare-menus.md).

`build` must be callable with no renderer and no device. That is a requirement
rather than a nicety: it is what lets `make check` ask what a menu contains, and
ADR-0001 says the decision has failed if it stops being true. The Wi-Fi screen's
pure half lives in `src/wifi_menu.c` for exactly this reason, and
`tools/menu-check.c` links it without SDL.

**Migrated:** the Wi-Fi screen.

**Not yet:** `game_info_screen`, `tortos_menu`, `cheevos_screen`, `game_menu`.
Each still writes its own loop and its own cursor walk. Migrate one at a time,
with a check for its rows written first - the check is what makes a
behavior-preserving refactor verifiable by something other than looking at a
handheld.
