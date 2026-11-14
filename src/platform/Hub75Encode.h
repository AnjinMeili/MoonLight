// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/Hub75Bus.h"   // Hub75Layout, Hub75Geometry: the bus contract the encode builds

#include <cstddef>   // size_t
#include <cstdint>

namespace mm {

/// HUB75 encode: the frame the driver hands a port, named for the wire unit it builds (one pixel clock is one SLOT). A pure data transform on the bus contract (platform/Hub75Bus.h); sibling of ParallelSlots.h.
///
/// It lives in platform/ beside the contract, not in light/ beside the bus output, because two sides of the include boundary encode: the ESP32 bus output (light/) and the Pi 5 PIO engine (platform/rpi/), whose refresh thread encodes the RGB frame the seam's push handed it. `light/drivers/Hub75Slots.h` includes this header, so the bus output's include is unchanged.
///
/// @attribution mrcodetastic, hzeller, ESPHome
///
/// @technical
///
/// ## The wire layout
///
/// One encoded frame, outermost first:
///
///   for each bit plane p (0 = least significant)
///     for each scan row r
///       hold words, then one word per column x -> one bus word: the six color bits for
///                                     (x, r) and (x, r + rows), addressed to
///                                     the row in the latch; the last column's
///                                     word also latches row r, dark
///
/// Every word on the bus is a clock, so the latch rides the last column's word: a word of its own would shift one pixel more than the panel holds. The hold words lead the row's words only where the row is narrow (see below); a wide row holds none, so its group is its columns exactly.
///
/// ## The address lags the data by one row
///
/// A panel lights where output-enable, the addressed row and the latched row meet, and the latch holds the row last strobed. So row r's words name row r - 1, the latch word moves the address to r in the dark, and row 0's words name the last row.
///
/// The lit window follows the data in the latch: row 0's words light the last row of the plane before, so they take that plane's window, and plane 0's take the top plane's, where the buffer wraps.
///
/// ## Brightness is time
///
/// A panel's chips only switch a column on or off, so plane p lights 2^p as long as plane 0 and brightness shortens every plane's window.
///
/// Below about a quarter brightness, the lowest plane of a 64-column row rounds to nothing. At 8 bits the same quarter-brightness threshold applies to the wall's 256-wide rows: below brightness 64 the lowest plane drops out and the picture is 7-bit, smooth, not an artefact — and on a stretched row, where the lowest plane is exactly one word at full brightness, it drops out below half brightness.
///
/// ## Hold words
///
/// A plane's lit window is a fraction of its row's words — `(rowWords−1)/2^(bitDepth−1)` for the lowest — so on a narrow row at 8 bits the lowest plane is half a word and never lights. The encoder stretches the row instead of flooring the plane: where the lowest plane would round to nothing, each row group leads with hold words — extra clocks with no data (colour 0, pushed out of the shift register by the row's own words before the latch), addressed to the latched row, lit inside the window like any other word — until the pass is `2^(bitDepth−1) + 1` words, where the lowest plane is exactly one word and every plane doubles exactly. The stretch is uniform across depths, with no depth-gated branch: it engages only where a plane would otherwise be dead, so a row whose lowest plane already lights by rounding keeps the rounding, and at 6 bits it is a no-op for rows of 17 words and up. The cost is refresh on narrow rows: the frame grows by the stretch (a 64-column row at 8 bits doubles, 64 to 129 words a pass; a 16-column row goes 16 to 129), and the rate prediction counts it (Hub75Canvas.h). Wide rows hold nothing: the wall's 256-word rows are untouched, and 8-bit there is 8/6 the 6-bit frame. The pass is the slower of the shift and the lowest plane's one word — the `max(S, 2^b × L)` slot of docs/reference/hardware/hub75-refresh.md, with the slot uniform across planes, which exact binary weights require: a plane's window is a fraction of the slot, so slots that differed per plane would break the ratios.

/// Encode one RGB frame into @xref{the-wire-layout|bit planes}, @xref{the-address-lags-the-data-by-one-row|the address a row late}, @xref{brightness-is-time|brightness as time}; bytes written, or 0 on a bad geometry: a dark frame beats half a frame.
inline size_t hub75Encode(const uint8_t* rgb, uint8_t* out,
                          const Hub75Geometry& geo, const Hub75Layout& lay = {}) {
    if (!rgb || !out) return 0;
    if (!geo.valid()) return 0;

    const uint16_t rows = geo.scanRows();
    const uint16_t pairs = geo.rowsPerScan() / 2;   // color passes per address step
    const uint16_t pairSpan = geo.height / 2;       // upper row pairs with row + span
    // Only the first `addrBits` are read, so a 1/8 panel never touches D or E.
    const uint8_t addr[5] = {lay.a, lay.b, lay.c, lay.d, lay.e};
    uint8_t addrBits = 3;                       // 1/8 scan
    if (geo.scanRate > 8) addrBits = 4;         // 1/16
    if (geo.scanRate > 16) addrBits = 5;        // 1/32

    // A row's address lines as one mask, built once per scan row rather than bit by bit on every word.
    auto addrMask = [&](uint16_t row) {
        uint16_t m = 0;
        for (uint8_t bit = 0; bit < addrBits; bit++)
            if ((row >> bit) & 1) m |= static_cast<uint16_t>(1u << addr[bit]);
        return m;
    };
    const size_t holdWords = geo.holdWords();
    const size_t passWords = geo.passWords();

    size_t w = 0;
    for (uint8_t plane = 0; plane < geo.bitDepth; plane++) {
        // Depth < 8 keeps the HIGH bits: dropping those would dim every bright pixel.
        const uint8_t shift = static_cast<uint8_t>(8 - geo.bitDepth + plane);
        for (uint16_t r = 0; r < rows; r++) {
            // The row in the LATCH while these words clock in: the previous one, and for row 0 the last row of the previous plane or frame.
            const uint16_t latched = static_cast<uint16_t>((r + rows - 1) % rows);
            // Lit words name the latched row; the latch word strobes row r and moves the address to it, in the dark.
            const uint16_t darkWord = addrMask(latched);
            const uint16_t litWord = static_cast<uint16_t>(darkWord | (1u << lay.oe));
            const uint16_t latchWord = static_cast<uint16_t>(addrMask(r) | (1u << lay.lat));
            // The plane whose data the latch holds while these words clock in, the previous one for row 0: its window is the one these words light.
            const uint8_t shownPlane = r == 0 ? static_cast<uint8_t>((plane + geo.bitDepth - 1) % geo.bitDepth) : plane;
            // Lit words: the top plane fills the pass less its latch, each lower plane half the one above; the numerator is 64-bit, since 32 bits overflow above ~131,000 row words at 8 bits.
            const uint32_t span = 255u << (geo.bitDepth - 1);
            const uint32_t litWords =
                static_cast<uint32_t>((static_cast<uint64_t>(passWords - 1) * geo.brightness * (1u << shownPlane) + span / 2) / span);
            // The hold words lead the pass: no data shifts in yet (colour 0, pushed out by the row's own words before the latch), the address names the latched row, and OE follows the window like any other word of the pass. Wide rows hold none, so this loop is empty and the pass is the row, as before.
            for (size_t i = 0; i < holdWords; i++) {
                const uint16_t word = i < litWords ? litWord : darkWord;
                // Little-endian, matching how the peripheral latches a 16-bit bus word.
                out[w++] = static_cast<uint8_t>(word & 0xFF);
                out[w++] = static_cast<uint8_t>((word >> 8) & 0xFF);
            }
            for (uint16_t pair = 0; pair < pairs; pair++)
            for (uint16_t x = 0; x < geo.width; x++) {
                // Pair p of step r is row r + p*scanRate, paired half a panel below.
                const uint16_t upRow = static_cast<uint16_t>(r + pair * geo.scanRate);
                const size_t up = (static_cast<size_t>(upRow) * geo.width + x) * 3;
                const size_t lo = (static_cast<size_t>(upRow + pairSpan) * geo.width + x) * 3;
                uint16_t word = 0;
                if ((rgb[up + 0] >> shift) & 1) word |= static_cast<uint16_t>(1u << lay.r1);
                if ((rgb[up + 1] >> shift) & 1) word |= static_cast<uint16_t>(1u << lay.g1);
                if ((rgb[up + 2] >> shift) & 1) word |= static_cast<uint16_t>(1u << lay.b1);
                if ((rgb[lo + 0] >> shift) & 1) word |= static_cast<uint16_t>(1u << lay.r2);
                if ((rgb[lo + 1] >> shift) & 1) word |= static_cast<uint16_t>(1u << lay.g2);
                if ((rgb[lo + 2] >> shift) & 1) word |= static_cast<uint16_t>(1u << lay.b2);
                // The last word latches; the rest are lit inside this plane's window and dark after it. The index counts from the pass start, hold words first, so the window spans both.
                const size_t i = holdWords + static_cast<size_t>(pair) * geo.width + x;
                word |= (i == passWords - 1) ? latchWord : (i < litWords ? litWord : darkWord);
                // Little-endian, matching how the peripheral latches a 16-bit bus word.
                out[w++] = static_cast<uint8_t>(word & 0xFF);
                out[w++] = static_cast<uint8_t>((word >> 8) & 0xFF);
            }
        }
    }
    // The frame ends on the last row's latch word, dark, and row 0's data words name that last row: the loop closes with every row lit once.
    return w;
}

}  // namespace mm
