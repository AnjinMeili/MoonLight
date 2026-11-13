// hub75_gpio_dump.h -- the shared GPIO dump loop: encoded words -> the panel.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// @attribution rpi-rgb-led-matrix (the dump sequence: row-major traversal,
//   the dark address change, the pulse wait before the strobe)
//
// The dump loop is the one place that turns a GPIO word frame into the
// sequence the panel sees: clock, latch, address, OE. It lives here as a
// template over a register sink so the hardware backend and the stub
// backend run the SAME loop -- the hardware sink writes the MMIO
// registers, the stub sink records the writes -- and the unit test pins
// the sequence the hardware would drive, not a copy of it.
//
// The frame is plane-major in memory (hub75Encode emits plane p's groups
// before plane p+1's), but the panel is dumped ROW-major: one row's full
// PWM -- every plane -- before the next row. The reference design's
// comment states why: rows cannot be switched very quickly without
// ghosting, so the full PWM of one row is done before switching rows.
// The bench-v4 sign proved the point the hard way: dumped plane-major,
// the address changed every ~13 us, and the slow-decay outdoor 1/8-scan
// panels ghosted each row's data onto a dozen neighbours (BENCH-V4).
// The group for row r, plane p lives at frame index (p * scanRows + r)
// -- the dump walks rows outer, planes inner, indexing into the
// plane-major frame.
//
// Per (row, plane), the sequence is:
//
//   1. Clock the group's columns in while the previous OE pulse runs.
//      Per column: clear the 0-data bits and the clock, set the 1-data
//      bits, delay the slowdown (data setup), raise the clock -- the
//      rising edge clocks the data in -- and delay the slowdown again
//      (clock hold: the edge must be over before the data moves on).
//      Falling-edge panels invert the sequence: the falling edge
//      clocks the data in, so the data is set with the clock high and
//      the clock is dropped last, then held.
//   2. Clear the colour bits and the clock: the data lines idle dark
//      from here, so nothing stale can clock in while the row changes.
//   3. Wait for the previous OE pulse to finish. The wait disables the
//      PWM channel (hub75_gpio_pwm.h), so the panel is dark from here
//      -- the address change and the latch happen in the dark, whatever
//      the FIFO held.
//   4. Set the row address -- skipped when it has not changed (the
//      planes of one row share it) -- and delay the slowdown so the
//      address lines settle before the strobe.
//   5. Strobe: raise the latch, delay the slowdown, drop it. The
//      latched data is the row just clocked in, at the address just set.
//   6. Fire the OE pulse for this plane. The pulse re-enables the PWM
//      channel; the next group's clock-in runs while it is lit.
//
// The slowdown delays are dummy GPLEV0 reads on the hardware sink (a
// register read the hardware must serve, so the CPU cannot outrun the
// panel) and no-ops on the recording sink. The reference design delays
// after EVERY GPIO operation -- the data write, the clock edge, the
// address write, the strobe -- and so does this loop; a delay missing
// after the clock edge is a hold time the panel never gets, and one
// missing after the address write is an address the latch may catch
// mid-change.

#pragma once

#include <cstddef>
#include <cstdint>

#include "platform/rpi/hub75_gpio_backend.h"   // GpioSubmitConfig
#include "platform/rpi/hub75_gpio_pins.h"

namespace mm::platform {

// One recorded dump event, for the stub backend's stream (see
// hub75_gpio_backend_stub.h): a GPIO write the loop made, or a pulser
// step. `value` is the GPSET0/GPCLR0 mask for a write, the bitplane for
// a pulse fire, and unused for a pulse wait.
struct GpioDumpEvent {
    enum class Kind : uint8_t { Set, Clr, PulseWait, PulseFire };
    Kind kind = Kind::Set;
    uint32_t value = 0;
};

// The dump loop's register sink: what the loop needs from its backend.
// A sink is a small struct of inline methods (no virtual calls on the
// hot path):
//
//   void writeSet(uint32_t mask);   // GPSET0 = mask (or record it)
//   void writeClr(uint32_t mask);   // GPCLR0 = mask (or record it)
//   uint32_t readLev();             // GPLEV0, for the slowdown delays (or 0)
//   bool waitPulseFinished();       // the pulser wait (false on timeout)
//   void sendPulse(uint8_t plane);  // the pulser fire for bitplane p
//
// The hardware sink drives the mapped registers; the stub sink records
// every call into the dump stream the test reads back.
//
// Dumps one frame of `wordCount` GPIO words (one word per pixel clock,
// colour bits only) under `config`, with `slowdown` dummy reads per
// delay. Returns 0 on success, -1 when `words` is null. A frame shorter
// than the config expects is dumped group by group: groups past the
// end are skipped, not clocked -- the engine always submits full
// frames, so this is a defensive path only. `pulseTimeouts` counts the
// OE waits that timed out (the dump proceeds anyway).
template <typename Sink>
int hub75GpioDumpFrame(Sink& sink, const GpioSubmitConfig& config,
                       const uint32_t* words, size_t wordCount, int slowdown,
                       uint64_t& pulseTimeouts) {
    if (words == nullptr) return -1;
    const size_t groupWords = static_cast<size_t>(config.chainWidth) * config.pairs;
    const size_t groups = static_cast<size_t>(config.bitDepth) * config.scanRows;
    const size_t expect = groups * groupWords;
    const size_t total = wordCount < expect ? wordCount : expect;
    const uint32_t colourMask = kGpioColourMask;
    const uint32_t clkBit = kGpioClkBit;
    const bool falling = config.clockFallingEdge;
    const int delay = slowdown < 0 ? 0 : slowdown;

    // The address the lines currently hold, so a row's planes do not
    // rewrite it: -1 means "not yet written this frame" (the first
    // row always writes it, whatever the previous frame left behind).
    int lastRow = -1;

    for (int row = 0; row < config.scanRows; row++) {
        for (int plane = 0; plane < config.bitDepth; plane++) {
            // The frame is plane-major: group (plane, row) sits at
            // plane * scanRows + row.
            const size_t g = static_cast<size_t>(plane) * config.scanRows +
                             static_cast<size_t>(row);
            const size_t base = g * groupWords;
            if (base >= total) continue;   // a short frame: skip the group
            const size_t n = groupWords < total - base ? groupWords : total - base;
            // 1. Clock this group's columns in while the previous OE
            //    pulse runs. Three GPIO writes per column, with a
            //    slowdown delay after the data write (setup) and after
            //    the clock edge (hold).
            if (!falling) {
                for (size_t i = 0; i < n; i++) {
                    const uint32_t data = words[base + i] & colourMask;
                    sink.writeClr((~data & colourMask) | clkBit);
                    sink.writeSet(data);
                    for (int s = 0; s < delay; s++) (void)sink.readLev();
                    sink.writeSet(clkBit);   // rising edge clocks the data in
                    for (int s = 0; s < delay; s++) (void)sink.readLev();
                }
            } else {
                for (size_t i = 0; i < n; i++) {
                    const uint32_t data = words[base + i] & colourMask;
                    sink.writeSet(data | clkBit);
                    sink.writeClr(~data & colourMask);
                    for (int s = 0; s < delay; s++) (void)sink.readLev();
                    sink.writeClr(clkBit);   // falling edge clocks the data in
                    for (int s = 0; s < delay; s++) (void)sink.readLev();
                }
            }
            // 2. The data lines idle dark while the row changes.
            sink.writeClr(colourMask | clkBit);
            // 3. The previous OE pulse must be finished -- and the PWM
            //    channel disabled, so the panel is dark -- before the
            //    address moves or the latch fires.
            if (!sink.waitPulseFinished()) pulseTimeouts++;
            // 4. The row address, when it changed; then let it settle.
            if (row != lastRow) {
                const uint32_t addr = gpioAddressMaskForRow(static_cast<uint8_t>(row));
                sink.writeClr(kGpioAddressMask & ~addr);
                sink.writeSet(addr);
                lastRow = row;
            }
            for (int s = 0; s < delay; s++) (void)sink.readLev();
            // 5. Strobe the row just clocked in, at the address just set.
            sink.writeSet(kGpioStbBit);
            for (int s = 0; s < delay; s++) (void)sink.readLev();
            sink.writeClr(kGpioStbBit);
            // 6. Fire the OE pulse for this plane; the next group's
            //    clock-in runs while it is lit.
            sink.sendPulse(static_cast<uint8_t>(plane));
        }
    }
    return 0;
}

}  // namespace mm::platform
