# HUB75 test pattern

A MoonLive script, `hub75-test.mle` (in `moonlive/effects/basics/`,
in the catalog after a rebuild; the short name fits the 15-character
module-name limit), that checks the panel's row mapping with a photo
and a stopwatch. It exists because the Stripe mapping in
`hub75_gpio_multiplex.h` is implemented from hzeller's Stripe algorithm,
not measured on a panel: this pattern is how the lab verifies or corrects
it. The pattern was **not** run on v3 — the Stripe shape (below) explained
the picture, so the lab went straight to the fix. Run on v4 it showed
spreading, not misplaced lines — one layer row lit the whole sign, one
layer column spread over a dozen rows — an engine fault, not a mapping
fault, so the mapping was still the thing this pattern checks once the
engine holds a line. Run on v5 it renders right on the sign — red rows
step top to bottom, green columns move left to right, held red layer
row 0 alone lights the top — so the mapping is now verified on a panel;
it is no longer unverified. The pattern stays as the check for any
future mapping change.

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

The loop runs on time since boot, so a watcher joining mid-loop sees
a mid-sweep step — the red sweep starting in the middle is normal,
not a fault.

## The two frames

The script counts in the **layer frame** — the 96×64 **canvas**
(the logical frame), 96 wide × 64 high, landscape like the sign as
viewed. Rows 0..63, columns 0..95; the loop is 2 + 64 + 96 = **162 s**.
In this frame — the one the photo shows — a red row step is a
**horizontal line** sweeping top to bottom, and a green column step is
a **vertical line** sweeping left to right.

The driver sees the same picture in the **panel frame** — its 64×96
**framebuffer**, 64 wide × 96 high, **unrotated**. The Rotate:90
mapper presents that framebuffer as the 96×64 canvas above, so in the
panel frame the same steps read the other way round: a lit layer row
is one panel **column**, a vertical line, and a lit layer column is
one panel **row**, a horizontal line. The two descriptions are the
same step seen before and after the mapper's rotation; do not mix
them when matching a photo to a second — the table below is in the
layer frame, the frame the script counts in. This is the panel's
established arrangement: the prior hzeller-based driver
(matrix-player) drove this sign — three outdoor 1/8-scan panels
(32 rows × 64 cols each) as one 96×64 display — with a 96×64 logical
frame, chain 1, parallel 3, multiplexing Stripe, and a Rotate:90
pixel mapper.

Two passes, in this order. First the driver drives the panel frame —
64×96, unrotated — and the driving, the Stripe mapping, and the
clock/latch/address sequence must be right in that frame first; that
is what this pattern checks. The 90-degree rotation to the 96×64
landscape frame happens only after the frame is assembled, and it is
MoonLight's layout engine's job, not the driver's: the GPIO output
and engine implement no rotation. Do not mix the rotation into the
driving or the mapping when reading a photo — a fault in the 64×96
frame is a driver fault; the rotation can only move a correct frame,
not fix a wrong one.

| Second | What should be lit |
| ------ | ------------------ |
| 0–1    | full-panel white flash |
| 2      | row 0, red — a horizontal line at the top of the sign |
| 3      | row 1, red — the horizontal line one step down |
| 4      | row 2, red |
| …      | … (one horizontal line a second, sweeping down the sign) |
| 65     | row 63, red — the horizontal line at the bottom edge |
| 66     | column 0, green — a vertical line at one edge of the sign |
| 67     | column 1, green — the vertical line one step across |
| …      | … (one vertical line a second, sweeping across the sign) |
| 161    | column 95, green — the vertical line at the far edge |
| 162    | flash again: the next loop |

What to look for: during the row steps, exactly one unbroken horizontal
line, moving one position a second down the sign; during the column
steps, exactly one unbroken vertical line, moving one position a second
across the sign. (Which edge the sweep starts at depends on the
layout's rotation sense, not the mapping — the sweep running the other
way is not a mapping fault. What matters is one line per step, moving
monotonically, with no split, doubling or jump.)

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

A line that **spreads** instead — one column filling every column, one
row lighting the whole sign — is not a mapping fault at all: a wrong
map moves whole lines or splits them in halves, it does not spread
them. Spreading points at the engine's clock/latch/address sequence,
before any mapping question.

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
