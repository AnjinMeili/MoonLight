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
// sustains ~104 ns per column at slowdown 1 (~192 Hz computed on the
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
// sign (BENCH-V5), not anchored on a published rate anymore.
//
// What the old model missed. It priced a column at 66 + 12 ns a
// slowdown unit -- 78 ns at slowdown 1, 102 ns at slowdown 3 -- and
// predicted ~1,537 Hz on the sign; the sign read 595-596 fps at
// slowdown 3, off by 2.6x. The miss was the slowdown delays: the loop
// runs TWO of them a column (after the data write and after the clock
// edge, hub75_gpio_dump.h), each `slowdown` dummy GPLEV0 reads, and
// the old 12 ns a unit priced each read at ~6 ns. A GPLEV0 read is an
// uncached peripheral read -- the CPU stalls until the peripheral
// answers -- and 6 ns was never plausible for it. At slowdown 3 the
// delays are six reads a column, and they dominate the column; the
// three GPIO writes are posted and cheap beside them.
//
// The calibration, from two bench readings on the same sign at the
// same slowdown 3. v4 read 1,099 fps (909,918 ns a frame) on the
// plane-major loop with ONE delay a column; v5 read 596 fps
// (1,677,852 ns a frame) on the row-major loop with TWO. The one
// per-column change between the loops is three extra GPLEV0 reads a
// column -- 18,432 across the frame -- plus ~5 reads and a few writes
// a group in the dark interval (the address setup delay, the strobe
// delay, the PWM disable/re-enable), 240 reads and ~64 writes across
// the frame. The row-major reorder itself costs nothing measurable:
// the address writes fall from 48 to 8 a frame, and every OE pulse
// (5.12 us at most, 160 ns LSB) is far shorter than a group's shift,
// so the pulse wait never stalls in either order. The frame-time
// delta, 767,934 ns over 18,672 reads, prices one delay iteration
// (one GPLEV0 read plus its loop step) at ~41 ns. v4's absolute rate
// then prices the rest of the column -- three posted writes plus the
// loop overhead -- at ~22 ns: (909,918/48 - ~310 ns of dark interval)
// / 128 columns - 3 x 41 ns. So a column is 22 ns + 82 ns a slowdown
// unit (two delays of `slowdown` reads at ~41 ns each), and the sign
// predicts 48 x (128 x 268 + 500) = 1,670,592 ns, ~599 Hz -- 0.4%
// above the measured 595-596 fps.
//
// The dark interval stays 0.5 us a group: the per-group dark ops
// (the colour clear, the pulse wait and its PWM disable, the address
// setup and strobe delays, the pulse fire and its PWM re-enable)
// count to ~0.5-0.6 us at slowdown 3, inside the estimating error of
// the 0.5 us already modelled -- the column was the miss, not the
// dark. The OE-bound branch (a long LSB outlasting the shift) is
// unchanged and unmeasured on the sign: only the 160 ns LSB,
// shift-bound case has a bench reading behind it.
//
// Tolerance: +-5% on the sign's predicted rate. Same-shape bench runs
// differed 3% (v3 1,067 fps, v4 1,099 fps), and the per-op estimates
// carry ~1-2%; the unit test pins the prediction against the measured
// 595-596 fps within that band. Pi 4-anchored: a Pi 3's slower CPU
// reads lower, and no Pi 3 bench exists -- the old 130 ns/column Pi 3
// estimate is retired, not re-based. Slowdown 2 is not yet measured
// on the sign (hzeller ran it at 2): the model interpolates it at
// 186 ns a column, and the lab reading is the check.
// ---------------------------------------------------------------------------
inline uint16_t hub75GpioColumnTimeNs(int slowdown) {
    if (slowdown < 0) slowdown = 0;
    if (slowdown > 4) slowdown = 4;
    return static_cast<uint16_t>(22 + slowdown * 82);   // 104 ns at slowdown 1, 268 ns at slowdown 3
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
    uint8_t bitDepth = 6;     ///< bit planes per frame, 2..6
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
