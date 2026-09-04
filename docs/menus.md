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
screen looks broken. Use the `do { } while (!rows[sel].live)` form - see
`wifi_screen`. This was wrong everywhere until 2026-09-04 and is only fixed in
the Wi-Fi screen; **the others have not been audited.**

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

## Known gap: there is no menu loop

Every screen writes the same skeleton by hand - poll input, check quit, check
power and idle, check back, move the cursor, act on accept, draw, delay. That is
five or six copies, and it is why the rules above are conventions a screen can
forget rather than behavior it gets for free.

The fix is a loop helper: screens hand over rows and a handler and get the
skeleton, including cursor-skipping and the power and idle checks. That would
make the first rule in this document impossible to break instead of documented.
It is a refactor across every screen and has not been done.

Until it is, this file is the substitute, and it is a weaker one - a convention
enforced by reading is not enforced.
