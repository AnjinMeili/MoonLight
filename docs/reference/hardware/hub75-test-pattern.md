# HUB75 test pattern

A MoonLive script, `hub75-test-pattern.mle` (in `moonlive/effects/basics/`,
in the catalog after a rebuild), that checks the panel's row mapping with a
photo and a stopwatch. It exists because the Stripe mapping in
`hub75_gpio_multiplex.h` is implemented from hzeller's Stripe algorithm,
not measured on a panel: this pattern is how the lab verifies or corrects
it. The pattern was **not** run on v3 — the Stripe shape (below) explained
the picture, so the lab went straight to the fix; the pattern is the check
for the fixed mapping.

## The mapping it checks

Stripe drives a 32x64 panel as 16 rows x 128 columns (stretch factor 2):
logical rows 0..7 and 16..23 fold to driven columns 64..127, rows 8..15
and 24..31 stay at columns 0..63, and the address is
`(y / 16) * 8 + y % 8` — 8 address steps, each lighting four logical
rows. If the mapping is right, every lit row and column lands exactly
where the table says. If it is wrong, the photo *is* the correction (see
[The diagnostic](#the-diagnostic)).

## The scheme

The loop is self-synchronizing, so a photo can be matched to a step:

1. **Seconds 0–1: full-panel white flash** (2 s). The loop starts here;
   unmissable, and the stopwatch starts here.
2. **Then one red row at a time, 1 s each, top to bottom** (rows 0..height-1).
3. **Then one green column at a time, 1 s each, left to right**
   (columns 0..width-1).
4. Repeat.

The general rule: the loop is `2 + height + width` seconds. At second `s`
(counted from the flash):

- `s = 0..1`: the flash.
- `2 <= s < 2 + height`: row `s - 2` lit red.
- `2 + height <= s < 2 + height + width`: column `s - 2 - height` lit green.

## The sign's canvas (64×96, unrotated)

Rows 0..95, columns 0..63; the loop is 2 + 96 + 64 = **162 s**. The
frame is 64 wide by 96 high **unrotated** — Rotate:90 is the layout's,
not the driver's — so on the physical sign the pattern reads rotated:
a lit frame **row** shows as a **vertical line**, and a lit frame
**column** as a **horizontal line**.

| Second | What should be lit |
| ------ | ------------------ |
| 0–1    | full-panel white flash |
| 2      | row 0, red — a vertical line at one edge of the sign |
| 3      | row 1, red — the vertical line one step across |
| 4      | row 2, red |
| …      | … (one vertical line a second, sweeping across the sign) |
| 97     | row 95, red — the vertical line at the far edge |
| 98     | column 0, green — a horizontal line at one edge of the sign |
| 99     | column 1, green — the horizontal line one step down/across |
| …      | … (one horizontal line a second, sweeping the sign's height) |
| 161    | column 63, green — the horizontal line at the far edge |
| 162    | flash again: the next loop |

What to look for: during the row steps, exactly one unbroken vertical
line, moving one position a second across the sign; during the column
steps, exactly one unbroken horizontal line, moving one position a
second down the sign's height. (Which edge the sweep starts at depends
on the layout's rotation sense, not the mapping — the sweep running
the other way is not a mapping fault. What matters is one line per
step, moving monotonically, with no split, doubling or jump.)

## The diagnostic

Run it with the output's `multiplexing` set to **Stripe**. If the mapping is
right, each lit line lands exactly where the table says.

If a lit line lands in the **wrong place**, or a step shows **two
half-lines** instead of one whole line, the mapping is wrong — and the
photo *is* the correction: record which physical line lit at which step
(e.g. "at second 3, the line that should be row 1 lit as two halves at
the row-4 and row-12 positions") and send that table back. The true
mapping is a one-function change in `hub75_gpio_multiplex.h`. A line
landing whole but at the wrong step points at the address formula; a
line split into halves points at the column fold.

For reference, with `multiplexing` set to **normal** on Stripe panels the
picture is the washed, shearing blend the bench saw on v2 and v3: the
engine sends 64 columns a row where the panel's driven row holds 128,
so each lit row lands as a half-width segment on the wrong row. (An
earlier version of this note described the scatter as "stripes of 4";
that described the previous, wrong permutation, not the panel — it has
been removed.) If the pattern shows the blend with Stripe set, the
mapping is not being applied (check the control actually reached the
engine: the recorded options should show `multiplexing` 1). If it shows
a *different* scatter, that scatter is the panel's true mapping —
record it as above.

Within each half of a driven row the columns run in order: a green
column landing in the wrong half is the fold, a column landing out of
order is a column problem (wiring, chain order), not the row mapping.
