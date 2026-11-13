// hub75_gpio_multiplex.h -- the Stripe mapping for multiplexed HUB75 panels.
//
// SPDX-License-Identifier: GPL-3.0-or-later
// @attribution rpi-rgb-led-matrix (the Stripe algorithm below)
//
// A direct panel lights, at address step s, the rows the encoder placed
// at step s. Some outdoor panels wire their rows through an internal
// multiplexer instead, so the panel is driven as a SHORTER, WIDER panel
// than it looks: hzeller's README puts it as "by default their output
// looks jumbled up" and "they require some pixel-mapping", and names the
// types (0 direct, 1 Stripe, 2 Checkered, 3 Spiral, 4 ZStripe, ...).
// The Pi 4 sign's panels need type 1, Stripe (--led-multiplexing=1 in
// its working hzeller card), and this file implements Stripe only.
//
// THE MAPPING -- the algorithm of hzeller's StripeMultiplexMapper
// (lib/multiplex-mappers.cc, the "Stripe" entry), reimplemented here in
// new code from the algorithm, not copied: see the license caveat below.
//
// Stripe has stretch factor 2: a panel of R rows x C columns is driven
// as R/2 rows x 2C columns, and the scan is the driven panel's --
// R/4 address steps, so the sign's 32x64 panel is driven as 16 rows x
// 128 columns at 1/8 scan, on three address lines (ABC) instead of
// four. Logical pixel (x, y) lands at driven:
//
//   fold = (y % (R/2)) < R/4
//   driven_x = fold ? x + C : x
//   driven_y = (y / (R/2)) * (R/4) + y % (R/4)
//
// For the sign's 32x64 panel: rows 0..7 and 16..23 fold to columns
// 64..127, rows 8..15 and 24..31 stay at columns 0..63, and the
// address is (y/16)*8 + y%8. Each driven row holds TWO logical rows
// side by side -- driven row 0 is logical row 8 on columns 0..63 and
// logical row 0 on columns 64..127 -- and each address step lights
// driven rows s and s+8, so four logical rows a step. The driven frame
// holds the same pixel count as the logical one, only reshaped.
//
// LICENSE CAVEAT. hzeller's per-file headers are GPL-2.0-only despite
// its README saying "or any later version", so no hzeller source is
// copied, transliterated or adapted here: the mapping above is new
// code written from the algorithm, credited to rpi-rgb-led-matrix (see
// docs/reference/attributions.md). This applies to the Pi 4 GPIO side
// only; the Pi 5 PIO side stays clean-room and has no multiplexing.
//
// UNVERIFIED ON A PANEL. The mapping is the algorithm from the mapper
// the sign's hzeller build runs, but no panel has shown it from this
// engine yet: the test pattern (docs/reference/hardware/hub75-test-pattern.md)
// is the check, and the lab's photo is the correction if it disagrees.

#pragma once

#include <cstdint>

namespace mm::platform {

// One panel's driven position: the column in the doubled width, the row
// in the halved height.
struct Hub75StripePos {
    uint16_t x = 0;   ///< the driven column, 0..2*cols-1
    uint8_t  y = 0;   ///< the driven row, 0..rows/2-1
};

// Logical pixel (x, y) of a panel of `rows` x `cols` under Stripe: the
// fold puts the upper half of each 16-row group (for 32-row panels) at
// columns +cols, and the address folds the rows into half the height.
// `rows` must be a multiple of 4; a panel too short to fold maps to
// itself.
inline Hub75StripePos hub75GpioStripeMap(uint16_t x, uint8_t y, uint8_t rows, uint16_t cols) {
    if (rows < 4) return Hub75StripePos{x, y};
    const uint8_t half = static_cast<uint8_t>(rows / 2);
    const uint8_t quarter = static_cast<uint8_t>(rows / 4);
    const bool fold = (y % half) < quarter;
    Hub75StripePos p;
    p.x = static_cast<uint16_t>(fold ? x + cols : x);
    p.y = static_cast<uint8_t>((y / half) * quarter + y % quarter);
    return p;
}

// The scan a panel of `rows` wires: rows/2 direct, rows/4 under Stripe
// -- the driven panel's scan, since Stripe halves the rows first. A
// 32-row panel is 1/16 direct, 1/8 Stripe; a 64-row one 1/32 direct,
// 1/16 Stripe.
inline uint8_t hub75GpioMultiplexScanRate(uint8_t multiplexing, uint8_t rows) {
    if (multiplexing == 1) return rows > 3 ? static_cast<uint8_t>(rows / 4) : 1;
    return rows > 1 ? static_cast<uint8_t>(rows / 2) : 1;
}

// The driven panel's height: the rows, or half of them under Stripe.
inline uint8_t hub75GpioMultiplexDrivenRows(uint8_t multiplexing, uint8_t rows) {
    return multiplexing == 1 ? static_cast<uint8_t>(rows / 2) : rows;
}

// The driven panel's width: the columns, or double under Stripe.
inline uint16_t hub75GpioMultiplexDrivenCols(uint8_t multiplexing, uint16_t cols) {
    return multiplexing == 1 ? static_cast<uint16_t>(cols * 2) : cols;
}

}  // namespace mm::platform
