# HUB75 test pattern

A MoonLive script, `hub75-test-pattern.mle` (in `moonlive/effects/basics/`,
in the catalog after a rebuild), that checks the panel's row mapping with a
photo and a stopwatch. It exists because the Stripe row permutation in
`hub75_gpio_multiplex.h` is reimplemented from documented behaviour, not
measured on a panel: this pattern is how the lab verifies or corrects it.

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

## The sign's canvas (64×96)

Rows 0..95, columns 0..63; the loop is 2 + 96 + 64 = **162 s**.

| Second | What should be lit |
| ------ | ------------------ |
| 0–1    | full-panel white flash |
| 2      | row 0, red |
| 3      | row 1, red |
| 4      | row 2, red |
| …      | … (one row a second, down the panel) |
| 97     | row 95, red |
| 98     | column 0, green |
| 99     | column 1, green |
| …      | … (one column a second, across the panel) |
| 161    | column 63, green |
| 162    | flash again: the next loop |

## The diagnostic

Run it with the output's `multiplexing` set to **Stripe**. If the mapping is
right, each lit row lands exactly where the table says.

If a lit row lands at a **different physical row** than the table says, the
permutation is wrong — and the photo *is* the correction: record which
physical row lit at which step (e.g. "at second 3, the row that should be
row 1 lit at physical row 4") and send that table back. The true
permutation is a one-function change in `hub75_gpio_multiplex.h`.

For reference, with `multiplexing` set to **normal** on Stripe panels the
rows scatter in stripes of 4: within each block of 16 rows, source row `y`
appears at physical row `(y % 4) * 4 + y / 4` — so source rows 0..15 appear
at 0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15. If the pattern shows
that scatter with Stripe set, the permutation is not being applied (check
the control actually reached the engine: the recorded options should show
`multiplexing` 1). If it shows a *different* scatter, that scatter is the
panel's true permutation — record it as above.

The columns are not permuted: a green column landing in the wrong place
means a column problem (wiring, chain order), not the row mapping.
