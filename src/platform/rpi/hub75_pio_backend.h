// hub75_pio_backend.h -- the seam between the PIO engine and RP1 PIO.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The engine talks to the PIO hardware only through this interface. Two
// implementations exist:
//
//   - PiolibBackend (hub75_pio_backend_piollib.cpp): the real one, using
//     Raspberry Pi's piolib (BSD-3-Clause) over /dev/pio0. Built only
//     when piolib is found; see the root CMakeLists.txt.
//   - StubBackend (hub75_pio_backend_stub.cpp): no hardware. submitFrame()
//     sleeps for the time the frame would take on the wire, so the
//     refresh thread, the swap handshake and the engine all run -- and can
//     be tested -- on a machine with no Pi attached. Always built.
//
// The interface is deliberately narrow: open, start (load program, claim
// and configure the SM, configure DMA, enable), submitFrame (blocking
// DMA transfer of one whole frame), blankAndStop (the blank-first
// shutdown), close. Everything PIO-specific -- pin directions, shift
// config, FIFO join, clock divider -- lives in the backend, not the
// engine.
//
// Why submitFrame() is the frame-done signal: piolib's pio_sm_xfer_data()
// is synchronous -- it copies the buffer into kernel DMA bounce
// buffers, submits the DMA, and blocks until every chunk's DMA
// completion callback has fired -- i.e. until the whole frame
// has been delivered to the SM's TX FIFO. The FIFO holds at most 8 words,
// so on return the SM has consumed all but a few pixel clocks of the
// frame. That is the S3-style safe swap (wait for proof before recycling
// a buffer), provided by the kernel driver rather than by a PIO IRQ --
// which matters, because PIO IRQ delivery to userspace is new in piolib
// and not present in every kernel (the kernel PIO driver on rpi-6.12.y
// has no IRQ path at all). The engine therefore does not depend on IRQs.

#pragma once

#include <cstdint>
#include <string>

namespace mm::platform {

// RP1 PIO system clock, Hz. clk_sys "typically runs at 200MHz" (RP1
// peripherals datasheet); piolib's rp1_clock_get_hz(clk_sys) returns
// 200 MHz. The pixel clock and refresh rate derive from this.
constexpr uint32_t kRp1PioClockHz = 200'000'000;

// PIO cycles per pixel clock for the hub75_wide program:
// pull (1) + out [1] (2) + nop [1] (2). See hub75.pio.
constexpr int kCyclesPerPixelClock = 5;

// One 32-bit FIFO word per pixel clock (the wide word).
constexpr int kBytesPerPixelClock = 4;

class PioBackend {
  public:
    virtual ~PioBackend() = default;

    // Open the PIO device and claim one state machine. Fills `error` and
    // returns false on failure.
    virtual bool open(std::string& error) = 0;

    // Load the program, configure the SM (pins, shift, FIFO join, clock
    // divider), configure the DMA transfer ring, clear the FIFOs and
    // enable the SM. `programInstructions`/`programLength` come from the
    // pioasm-generated hub75.pio.h (rising- or falling-edge variant, as
    // chosen by the engine). `wrapTarget`/`wrap` are the program's wrap
    // bounds. `clkdivInt`/`clkdivFrac` set the SM clock divider
    // (divider = int + frac/256, int >= 1). `xferBufSize`/`xferBufCount`
    // size the kernel DMA bounce buffers (count <= 4, the kernel limit).
    virtual bool start(const uint16_t* programInstructions, int programLength,
                       int wrapTarget, int wrap, uint16_t clkdivInt,
                       uint8_t clkdivFrac, uint32_t xferBufSize,
                       uint32_t xferBufCount, std::string& error) = 0;

    // Submit one whole frame for DMA into the SM's TX FIFO. BLOCKS until
    // the frame has been delivered (see the header note above), so it
    // also paces the refresh loop at the display rate. Returns 0 on
    // success, or a negative errno-style code on failure (notably
    // -ETIMEDOUT if the SM is not draining the FIFO, e.g. not enabled).
    virtual int submitFrame(const void* data, uint32_t bytes) = 0;

    // Blank-first shutdown: submit the blank frame, wait until the TX
    // FIFO is empty (the SM is then holding the dark word), disable the
    // SM, clear the FIFOs, remove the program and release the SM. The
    // pins are left in PIO function holding the dark state, so the panel
    // stays blanked after shutdown. Fills `error` and returns false if
    // any step fails (best effort continues through the remaining steps).
    virtual bool blankAndStop(const void* blankData, uint32_t blankBytes,
                              std::string& error) = 0;

    // Release the SM and close the device. Safe to call after a failed
    // open/start (no-op for anything not acquired).
    virtual void close() = 0;

    virtual const char* name() const = 0;
};

// Factory: the piolib backend, unless `preferStub` is set -- or null
// when this build has no piolib backend (see MM_HUB75_PIO_HAVE_PIOLIB),
// which the engine reports as needing a Raspberry Pi build with piolib.
// The stub backend is always linked in, so the engine tests run it on any
// host; production never selects it.
PioBackend* createPioBackend(bool preferStub);

// Per-backend factories (implemented in the respective .cpp files).
PioBackend* createStubBackend();
#ifdef MM_HUB75_PIO_HAVE_PIOLIB
PioBackend* createPiolibBackend();
#endif

}  // namespace mm::platform
