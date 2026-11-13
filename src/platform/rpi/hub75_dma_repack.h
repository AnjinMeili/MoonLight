// hub75_dma_repack.h -- pack three MoonLight slot streams into GPIO data words.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Each chain's frame is encoded separately by MoonLight's hub75Encode()
// (platform/Hub75Encode.h) into 16-bit little-endian slot words. The
// three chains share address, latch and OE, and run identical geometry
// in lockstep, so at any slot index the three slot words differ only in
// their six colour bits. The repack merges them into one 32-bit GPIO
// data word per pixel clock for the CPU dump loop:
//
//   bit (gpio) = level for that GPIO's colour line.
//
// Only the colour bits survive the repack. The slot word's address,
// latch and OE bits are stripped: the dump loop drives the address and
// the latch itself in the dark interval between rows, and the hardware
// PWM pulser drives OE -- that is the point of the hybrid design (the
// encoder's OE bits describe a timed pulse the PWM now owns, so they
// must not reach the GPIO stream).
//
// The repack assumes MoonLight's DEFAULT Hub75Layout (r1=0, g1=1, b1=2,
// r2=3, g2=4, b2=5, a=8, b=9, c=10, d=11, e=12, lat=13, oe=14). The
// engine always encodes with the default layout; a remapped layout
// would need the tables rebuilt from it (not implemented -- the board
// wiring is fixed, so the default layout is the right one). The .cc pins
// the default with a static_assert, so a future layout change fails at
// compile time instead of scrambling the repack.
//
// Speed: the scatter is done with lookup tables built once from the pin
// map -- three 64-entry colour tables (one per chain, 6 colour bits in).
// The repack loop is three table lookups and two ORs per word.

#pragma once

#include <cstddef>
#include <cstdint>

namespace mm::platform {

// Number of parallel chains the active board carries.
constexpr int kDmaMaxChains = 3;

// Lookup tables for the repack. Built once by buildDmaRepackTables()
// from the pin map in hub75_dma_pins.h, so a wiring change is a data
// change, not a code change.
struct DmaRepackTables {
    // colour[chain][6 colour bits as in the slot word] -> GPIO mask for
    // that chain's six colour pins.
    uint32_t colour[kDmaMaxChains][64];
};

// Build the tables from the pin map. Pure function of hub75_dma_pins.h.
DmaRepackTables buildDmaRepackTables();

// Repack `slots` 16-bit slot words per chain into `slots` 32-bit GPIO
// data words. `chainSlots[c]` points at chain c's slot stream
// (little-endian uint16_t values, as hub75Encode writes them). Chains
// beyond `chains` are treated as all-zero (their panels, if connected,
// stay dark). The address / latch / OE bits of the slot words are
// ignored -- the dump loop and the PWM pulser own those lines.
void repackSlotsToGpio(const uint16_t* const chainSlots[kDmaMaxChains], int chains,
                       size_t slots, const DmaRepackTables& tables, uint32_t* gpioOut);

// Reference implementation of one word's pack, written out bit by bit
// with no tables. Used by the unit test to check the tables; not used on
// the hot path.
uint32_t packGpioWordReference(uint16_t slot0, uint16_t slot1, uint16_t slot2,
                               int chains);

}  // namespace mm::platform
