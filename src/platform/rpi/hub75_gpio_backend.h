// hub75_gpio_backend.h -- the seam between the GPIO engine and Pi 3/4 GPIO+PWM.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// DESIGN NOTE -- why no DMA: this engine drives the panel columns from
// the CPU and times OE with the hardware PWM pulser. Measured DMA GPIO
// rates on Pi 4
// (~1.5-1.8 MHz of GPIO operations, hzeller's rpi-gpio-dma-demo,
// oscilloscope-verified) put a DMA-streamed wall at 15-18 Hz computed --
// far below the 80-100 Hz flicker threshold -- while the CPU loop
// sustains ~262 ns per column at slowdown 1 (~77 Hz computed on the
// wall, bench-calibrated -- hub75GpioColumnTimeNs). See the
// design decision in MOONLIGHT-INTEGRATION.md ("Pi 3/4 (GPIO)" section).
// No DMA channel is claimed, no DMA control block is built, and the
// dma.dmachans mask is not consulted: there is nothing to validate
// against it. If hardware measurement ever overturns the DMA rate, the
// backend interface below takes a DMA submitter -- the engine does not
// care how submitFrame() moves bits.
//
// The engine talks to the Pi 3/4 hardware only through this interface.
// Two implementations exist:
//
//   - HwBackend (hub75_gpio_backend_hw.cpp): the real one, mapping the
//     GPIO / PWM / clock-manager registers through /dev/mem, driving
//     the columns from the refresh thread and timing OE with the PWM
//     pulser (hub75_gpio_pwm.h). Always compiled in -- it needs no
//     external library, only /dev/mem and root -- and its open() fails
//     with the reason on a host without Pi hardware.
//   - StubBackend (hub75_gpio_backend_stub.cpp): no hardware.
//     submitFrame() sleeps for the time the frame would take on the
//     wire, so the refresh thread, the swap handshake and the engine
//     all run -- and can be tested -- on a machine with no Pi attached.
//     Always built.
//
// The interface is deliberately narrow: open, start (configure the PWM
// clock and the pulser, mux OE to the PWM), submitFrame (blocking dump
// of one whole frame), blankAndStop (the blank-first shutdown), close.
// Everything hardware-specific -- register mapping, pin directions, the
// dark-first setup order, the dump loop -- lives in the backend, not
// the engine.
//
// Release semantics (NOTE.md rule 3): open() and close() are genuinely
// symmetric. close() releases every claim the open made -- the PWM
// channel is disabled, the OE pin is muxed back to a GPIO output
// driving high (dark), the register mappings are unmapped, /dev/mem is
// closed, and all state is reset -- so a second open() in the same
// process re-acquires everything from scratch. Nothing survives close()
// (there is no piolib-style IRQ thread here to leak a claim). The
// engine test pins this: open, shutdown, open again on the stub.
//
// Why submitFrame() is the frame-done signal: the dump is synchronous
// -- the call returns only after the last column of the last row-group
// has been clocked out (the last OE pulse may still be running, but it
// lights a row the next submit re-clocks before latching, exactly as
// the reference design's loop does). That is the safe swap (wait for
// proof before recycling a buffer), provided by the call's return
// rather than by an interrupt.

#pragma once

#include <cstdint>
#include <string>

namespace mm::platform {

// ---------------------------------------------------------------------------
// The Pi model, from /proc/device-tree/model with a /proc/cpuinfo
// fallback. Only Pi 3 (BCM2837) and Pi 4 (BCM2711) serve this backend:
// a Pi 5 has RP1 PIO and takes the PIO output instead.
// ---------------------------------------------------------------------------
enum class Hub75GpioModel : uint8_t { Unknown, Pi3, Pi4, Pi5 };

// Parse the model from a device-tree model string (e.g. "Raspberry Pi 4
// Model B Rev 1.2") and a cpuinfo Revision value (hex, e.g. "a02082"),
// either of which may be null. The device-tree string wins when it
// names a Pi; the revision's new-style type field (bits 4..11) is the
// fallback. Pure function of its inputs; the unit test pins the
// known revisions.
Hub75GpioModel hub75GpioDetectModel(const char* deviceTreeModel, const char* cpuinfoRevision);

// The peripheral base address the ARM sees for `model` (0 when the
// model is unknown or unserved): 0x3F000000 on Pi 3, 0xFE000000 on Pi 4.
uint32_t hub75GpioPeripheralBase(Hub75GpioModel model);

// Whether this host can serve the GPIO backend at all: a Pi 3 or 4 by
// model detection. Null when it can; otherwise the reason (not a Pi,
// or a Pi 5, which takes the PIO output). Read once and cached -- the
// answer cannot change under a running process. This is the model's
// answer only: /dev/mem access and the audio-driver conflict are
// checked at open(), so their reasons reach the status line instead of
// hiding the backend.
const char* hub75GpioHostRefusal();

// The model this host detected as, cached: the refusal and the slowdown
// resolver both read it, so the model is detected once.
Hub75GpioModel hub75GpioHostModel();

// ---------------------------------------------------------------------------
// The column-time model: how long one column step takes in the dump
// loop, in ns, at a resolved slowdown. Bench-calibrated on the Pi 4
// sign (BENCH-V6), not anchored on a published rate anymore. The
// value is the EFFECTIVE column time: the wire cost of the dump loop
// plus the refresh thread's encode, amortised over the frame (below),
// so the prediction predicts `shown` -- the submit rate the bench
// reads -- not pure wire time.
//
// What the v5 calibration missed. It priced a column at 22 + 82 ns
// a slowdown unit (268 ns at slowdown 3) and predicted ~599 Hz on
// the sign against 595-596 fps measured -- but it read the card's
// slowdown INDEX as the slowdown. The card stores the index
// (0=auto, 1="0", ..., 5="4") and slowdownRequested() returns
// index - 1 (Hub75GpioOutput.h), so the value 3 in
// pi4-drivers-v5.json was slowdown 2, hzeller's own setting for the
// sign -- and the v4 (1,099 fps, one delay a column) and v5 (596
// fps, two delays a column) readings the calibration priced were
// BOTH at true slowdown 2. The 82 ns a unit and its ~41 ns a
// GPLEV0 read are falsified by that mislabelling (BENCH-V6); the
// three-point re-derivation below replaces them. The card now also
// shows the resolved slowdown (`slowdownTrue`), so the next bench
// note quotes the slowdown the engine runs, not the stored index.
//
// The re-derivation, from three readings on the same sign at three
// TRUE slowdowns (BENCH-V6, three readings each):
//
//   slowdown 1: 608, 611, 616 fps -- 1,634,925 ns a frame (mean)
//   slowdown 2: 584, 585, 585 fps -- 1,710,377 ns a frame (mean)
//   slowdown 3: 555, 557, 560 fps -- 1,794,283 ns a frame (mean)
//
// The slope is 79,679 ns a slowdown unit. One unit adds two delay
// loops' worth of reads a column and two a group -- 2 x 6,144
// columns + 2 x 48 groups = 12,384 reads a frame -- so each
// additional GPLEV0 read costs ~6.4 ns: the reads pipeline, and the
// old ~6 ns price was right about the READ and wrong about the
// column. The slope a column is 2 x 6.4 = ~13 ns a unit.
//
// What dominates the frame is not the reads but the delay loops
// themselves. The v4->v5 delta at the same true slowdown 2 --
// 767,934 ns, for one added delay loop a column (6,192 more loops
// a frame) -- is ten times what 12,432 extra reads at ~6.4 ns
// explain (~80 us). The remainder prices each delay loop's START
// at ~107 ns: the loop's first GPLEV0 read follows the posted
// GPIO writes, and an uncached peripheral read stalls until the
// writes ahead of it have drained and the peripheral answers,
// while further reads in the same loop pipeline at ~6.4 ns. A
// delay loop costs ~107 ns + slowdown x 6.4 ns, and the frame's
// 12,384 loops (two a column, two a group) cost ~1.37 ms of the
// ~1.71 ms frame at slowdown 2 -- the loops dominate, not the
// reads and not the writes. The mechanism is inferred from the two
// independent bench deltas (the three-point slope and the v4->v5
// loop count), not traced on a scope; a lastSubmitUs read at
// slowdown 0/1 would cross it.
//
// The wire column is then ~15 ns of posted writes and loop
// overhead plus two loops: 15 + 2 x (107 + 6.4 x slowdown) = ~229
// + 13 ns a unit. The encode share rides on top: the refresh
// thread encodes a fresh frame before the submit
// (hub75_gpio_engine.cc refreshLoop), the render loop publishes
// at most every 4 ms (platform_desktop.cpp pauseLoop), so at ~585
// submits a second about 43% of submits are preceded by an encode
// of ~250-500 us on the Pi 4 (157-169 us measured for the same
// hub75Encode + repack work on an x86 host) -- ~100-200 us a
// frame, ~20 ns a column at the sign's geometry. The model absorbs
// the central ~20 ns, so a column is 249 + 13 ns a slowdown unit
// (262 ns at slowdown 1, 275 at 2, 288 at 3) and the sign predicts
// 612 / 584 / 558 Hz against 612 / 585 / 557 fps measured --
// within 0.2% on all three.
//
// The dark interval stays 0.5 us a group: the re-derived wire dark
// is ~0.39-0.44 us (a few writes, the pulse wait's poll, and its
// two delay loops), inside the estimating error of the 0.5 us
// already modelled, and its slowdown dependence (~13 ns a group a
// unit) is 0.6 us a frame a unit -- negligible. The OE-bound
// branch (a long LSB outlasting the shift) is unchanged and
// unmeasured on the sign: only the 160 ns LSB, shift-bound case
// has bench readings behind it.
//
// Slowdown 0 is the one discontinuity: no delay loop runs, so no
// first-read penalty is paid and the column is the ~15 ns of
// writes alone -- fitted, not measured, and the panel likely
// cannot latch at that setup/hold. The frame formula's OE-bound
// branch is what keeps the slowdown-0 prediction sane.
//
// Tolerance: +-3% on the sign's predicted rate. Same-shape bench
// runs differed 3% (v3 1,067 fps, v4 1,099 fps), the within-triple
// spread is ~0.7%, and the fit lands within 0.2% of all three v6
// means; the unit test pins the prediction against all three.
// Pi 4-anchored: a Pi 3's slower CPU reads lower, and no Pi 3 bench
// exists -- the old 130 ns/column Pi 3 estimate is retired, not
// re-based.
// ---------------------------------------------------------------------------
inline uint16_t hub75GpioColumnTimeNs(int slowdown) {
    if (slowdown < 0) slowdown = 0;
    if (slowdown > 4) slowdown = 4;
    if (slowdown == 0) return 15;   // no delay loop runs: the writes alone, unmeasured
    return static_cast<uint16_t>(249 + slowdown * 13);   // 262 ns at slowdown 1, 275 at 2, 288 at 3
}

// The slowdown the backend resolves an "auto" (-1) request to for its
// model: Pi 4 takes 2 (its faster GPIO needs the longer setup), Pi 3
// takes 1, anything else takes 1. Mirrors the reference design's
// per-model defaults.
inline int hub75GpioAutoSlowdown(Hub75GpioModel model) {
    return model == Hub75GpioModel::Pi4 ? 2 : 1;
}

// The one home for auto slowdown: resolve a requested slowdown (-1 for
// auto, else 0..4) for `model`. The backends resolve at start(), and
// the output's rate prediction resolves through the seam, so a Pi 4 on
// auto quotes its slowdown-2 rate in both places (bench v2: the output
// predicted slowdown 1 while the engine ran 2).
inline int hub75GpioResolveSlowdown(int8_t slowdown, Hub75GpioModel model) {
    return slowdown < 0 ? hub75GpioAutoSlowdown(model) : static_cast<int>(slowdown);
}

/// What one submitFrame() call needs to know about the frame it dumps:
/// the grouping the encoder produced and the timing the pulser runs.
// All values are fixed for the run, so the engine passes them once at
// start() and submitFrame() takes only the words.
struct GpioSubmitConfig {
    uint16_t chainWidth = 0;  ///< columns per chain (cols x chain)
    uint16_t scanRows = 0;    ///< address steps per plane (the panel's scan rate)
    uint16_t pairs = 0;       ///< colour passes per address step (rowsPerScan / 2)
    uint16_t holdWords = 0;   ///< hold words leading each group (Hub75Geometry::holdWords): the dump skips them, clocking only the shift words
    uint8_t bitDepth = 6;     ///< bit planes per frame, 2..8
    uint16_t lsbNs = 130;     ///< the shortest plane's lit time, 50..3000
    int8_t slowdown = -1;     ///< GPIO slowdown, -1 for auto by model, else 0..4
    bool clockFallingEdge = false;  ///< the panel samples on the falling clock edge
};

class GpioBackend {
  public:
    virtual ~GpioBackend() = default;

    // Probe the hardware, map the registers, and set the pins up dark:
    // dark levels written FIRST (OE high, colour and address 0, latch
    // low), then the pins made outputs, then -- at start() -- the PWM
    // configured and OE muxed to it. Fills `error` and returns false on
    // failure (not a Pi 3/4, no /dev/mem access, the audio driver owns
    // the PWM, a mapping failed); nothing is left claimed.
    virtual bool open(std::string& error) = 0;

    // Configure the PWM clock and the pulser for the config's LSB time,
    // resolve the slowdown, and mux OE to the PWM (the pin stays high
    // throughout: GPIO-driven high, then PWM idle high). Fills `error`
    // and returns false on failure.
    virtual bool start(const GpioSubmitConfig& config, std::string& error) = 0;

    // Dump one whole encoded frame to the panels: for each row, for
    // each plane (the frame is plane-major in memory; the dump walks
    // rows outer, planes inner -- hub75_gpio_dump.h), clock the
    // group's columns in while the previous OE pulse runs, wait for it
    // to finish (which blanks the panel), set the address and strobe in
    // the dark, and fire the next OE pulse. BLOCKS until the frame has
    // been clocked out, so it also paces the refresh loop at the
    // display rate. `words` is the GPIO data-word frame (one word per
    // pixel clock, colour bits only); `wordCount` is its length.
    // Returns 0 on success, or a negative errno-style code on failure.
    virtual int submitFrame(const uint32_t* words, size_t wordCount) = 0;

    // Blank-first shutdown: dump the blank frame (all colour bits
    // clear, so the OE pulses light nothing), wait for the last pulse
    // to finish, mux OE back to a GPIO output driving high (dark), and
    // stop the PWM. The pins are left driving the dark levels, so the
    // panel stays blanked after shutdown. Fills `error` and returns
    // false if any step fails (best effort continues through the
    // remaining steps).
    virtual bool blankAndStop(const uint32_t* blankWords, size_t blankWordCount,
                              std::string& error) = 0;

    // Release everything open() claimed (mappings, /dev/mem, PWM
    // state) and reset, so the next open() re-acquires from scratch.
    // Safe to call after a failed open/start (no-op for anything not
    // acquired). If the backend is still started, it drives the dark
    // levels first -- blank before stopping, even on the error path.
    virtual void close() = 0;

    virtual const char* name() const = 0;

    // The detected Pi model (Unknown on the stub, which has no
    // hardware). Valid after a successful open().
    virtual Hub75GpioModel model() const = 0;
    // The slowdown start() resolved (auto by model, else as asked).
    // Valid after a successful start().
    virtual int resolvedSlowdown() const = 0;
    // OE pulses whose finish wait timed out (the PWM wedged): the
    // dump proceeded anyway, so this counts a degraded panel, not a
    // hung one.
    virtual uint64_t pulseTimeouts() const = 0;
};

// Factory: the hardware backend, unless `preferStub` is set. The
// hardware backend is always linked in (it needs no external library),
// so the factory never returns null -- a host without Pi hardware
// fails at open() with that reason instead. The stub backend is the
// engine test's dry run; production never selects it.
GpioBackend* createGpioBackend(bool preferStub);

// Per-backend factories (implemented in the respective .cpp files).
GpioBackend* createGpioHwBackend();
GpioBackend* createGpioStubBackend();

}  // namespace mm::platform
