// hub75_gpio_multiplex.h -- the row permutation for multiplexed HUB75 panels.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A direct panel lights, at address step s, the row the encoder placed
// at step s. Some outdoor panels wire their rows through an internal
// multiplexer instead, so the row lit at step s is a permuted row, and
// a frame sent straight comes out jumbled: hzeller's README puts it as
// "by default their output looks jumbled up" and "they require some
// pixel-mapping", and names the types (0 direct, 1 Stripe, 2 Checkered,
// 3 Spiral, 4 ZStripe, ...). The Pi 4 sign's panels need type 1,
// Stripe (--led-multiplexing=1 in its working hzeller card), and this
// file implements Stripe only.
//
// HOW THE PERMUTATION WAS DERIVED -- read this before trusting it.
//
// No public documentation found specifies Stripe's exact row
// permutation: hzeller's README and lib/README.md describe
// multiplexing only in words ("pixels are not mapped in a
// straight-forward way, but in a snake arrangement for instance") and
// point at the mapper source for the implementation, and a second
// source (RPi-LED-Sign-Controller's README) calls Stripe only
// "traditional line-by-line multiplexing". The mapper source itself
// was NOT read: hzeller's files carry GPL-2.0-only headers, which
// block copying, transliterating or adapting them, so this
// implementation is written from the documented behaviour alone.
//
// The behaviour the permutation is built from:
//
//   - A multiplexed panel has fewer address lines than its height
//     needs (a 32-high panel with ABC instead of ABCD), so the
//     address step selects a GROUP of rows and the panel's wiring
//     picks one row of the group per step.
//   - Stripe divides the rows into stripes of 4: the step's low bits
//     select the stripe, the remaining bits the row within the
//     stripe. At step s of a scan of S steps, the panel lights
//     physical row P(s) = (s % (S/4)) * 4 + s / (S/4).
//   - At 1/16 scan (S = 16) that lights rows
//     0,4,8,12,1,5,9,13,2,6,10,14,3,7,11,15 at steps 0..15.
//
// The engine inverts the panel: it places source row P(r) at strip
// row r, so the encoder's step-r data is the source row the panel
// lights at step r. At 1/16 the permutation is its own inverse (a
// 4x4 transpose), so the direction does not matter for the sign; at
// 1/32 it does, and the direction above is the derived one.
//
// UNVERIFIED ON A PANEL. The permutation above is a best reading of
// the documented behaviour, not a measured fact: the sign's photo (a
// washed, shearing blend without any mapping) is consistent with rows
// scattered in stripes of 4, but does not pin the exact order. The
// test pattern (docs/reference/hardware/hub75-test-pattern.md) exists
// to verify or correct it on the real panels: if the lit row lands
// elsewhere, the correction is a one-function change here.

#pragma once

#include <cstdint>

namespace mm::platform {

// The source row strip row `row` takes when the panel multiplexes its
// address steps: 0 (normal) is the identity, 1 (Stripe) the
// permutation above, applied inside each block of `scanRate` rows so
// the lower half permutes the same way as the upper. Any other
// multiplexing value is the identity; the options check refuses it
// before the engine runs.
inline uint8_t hub75GpioMultiplexRow(uint8_t multiplexing, uint8_t row, uint8_t scanRate) {
    if (multiplexing != 1 || scanRate < 4) return row;
    const uint8_t block = static_cast<uint8_t>(row / scanRate);
    const uint8_t step = static_cast<uint8_t>(row % scanRate);
    const uint8_t stripes = static_cast<uint8_t>(scanRate / 4);
    const uint8_t physical = static_cast<uint8_t>((step % stripes) * 4 + step / stripes);
    return static_cast<uint8_t>(block * scanRate + physical);
}

}  // namespace mm::platform
