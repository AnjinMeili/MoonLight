// hub75_pio_repack.h -- pack three MoonLight slot streams into wide PIO words.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Each chain's frame is encoded separately by MoonLight's hub75Encode()
// (platform/Hub75Encode.h) into 16-bit little-endian slot words. The
// three chains share address, latch and OE, and run identical geometry in
// lockstep, so at any slot index the three slot words differ only in their
// six colour bits. The repack merges them into one 32-bit wide word per
// pixel clock for the PIO program (see hub75.pio and hub75_pio_pins.h):
//
//   wide bit (gpio - 2) = level for that GPIO; OE inverted (set = blanked).
//
// The repack assumes MoonLight's DEFAULT Hub75Layout (r1=0, g1=1, b1=2,
// r2=3, g2=4, b2=5, a=8, b=9, c=10, d=11, e=12, lat=13, oe=14). The engine
// always encodes with the default layout; a remapped layout would need
// the tables rebuilt from it (not implemented -- the board wiring is
// fixed, so the default layout is the right one). The .cc pins the default
// with a static_assert, so a future layout change fails at compile time
// instead of scrambling the repack.
//
// Speed: the scatter is done with lookup tables built once from the pin
// map -- three 64-entry colour tables (one per chain, 6 colour bits in)
// and one 128-entry control table (7 control bits in). The repack loop is
// four table lookups and three ORs per word.

#pragma once

#include <cstddef>
#include <cstdint>

namespace mm::platform {

// Number of parallel chains the active board carries.
constexpr int kMaxChains = 3;

// Lookup tables for the repack. Built once by buildRepackTables() from the
// pin map in hub75_pio_pins.h, so a wiring change is a data change, not a
// code change.
struct RepackTables {
    // colour[chain][6 colour bits as in the slot word] -> wide-word mask
    // for that chain's six colour pins.
    uint32_t colour[kMaxChains][64];
    // control[7 control bits as in the slot word, bits 8..14] ->
    // wide-word mask for A-E, LAT and OE (OE inverted: the table sets the
    // OE pin bit when the slot OE bit is CLEAR).
    uint32_t control[128];
};

// Build the tables from the pin map. Pure function of hub75_pio_pins.h.
RepackTables buildRepackTables();

// Repack `slots` 16-bit slot words per chain into `slots` 32-bit wide
// words. `chainSlots[c]` points at chain c's slot stream (little-endian
// uint16_t values, as hub75Encode writes them). Chains beyond `chains`
// are treated as all-zero (their panels, if connected, stay dark).
// The control bits (address / latch / OE) are taken from chain 0's word;
// the three chains' control bits are identical by construction (same
// geometry, same brightness), and debug builds assert that on the first
// word of each row.
void repackSlots(const uint16_t* const chainSlots[kMaxChains], int chains,
                 size_t slots, const RepackTables& tables, uint32_t* wideOut);

// Optional latch blanking, ESP32-style: clear the lit state (set the OE
// pin bit = blank) on the `blankWords` words before each row's latch word.
// `rowWords` is the number of slots per (plane, row) group
// (chainWidth * pairs). A value of 0 leaves the encoder's output
// untouched. This only affects the top plane(s) at high brightness --
// lower planes are already dark at the end of their rows -- and it
// shortens the top plane's lit window by `blankWords` pixel clocks.
void applyLatchBlanking(uint32_t* wide, size_t slots, size_t rowWords,
                        int blankWords);

// Reference implementation of one word's pack, written out bit by bit
// with no tables. Used by the unit test to check the tables; not used on
// the hot path.
uint32_t packWideWordReference(uint16_t slot0, uint16_t slot1, uint16_t slot2,
                               int chains);

}  // namespace mm::platform
