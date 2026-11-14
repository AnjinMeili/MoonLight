// hub75_gpio_engine.h -- HUB75 GPIO engine: RGB frames -> GPIO+PWM -> panels.
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
//
// Pipeline (one frame):
//
//   RGB virtual frame, published by the GPIO output through the seam
//     -> triple-buffered RGB slot (lock-free publish, newest wins)
//     -> refresh thread: per chain, one contiguous strip copy out of the
//        virtual frame (chains stack vertically, so each strip is one
//        memcpy; NO gamma -- MoonLight's correction already applied it).
//        Under Stripe the strip is remapped first: the logical panel is
//        reshaped into the driven panel (half the rows, double the
//        columns, hub75_gpio_multiplex.h), row by row, panel by panel
//        along the chain; the normal path keeps its single memcpy.
//     -> MoonLight hub75Encode() into 16-bit slot words (bitplanes, with
//        address / latch / OE baked in, one word per pixel clock)
//     -> repack: three chains' slot words -> one 32-bit GPIO data word
//        each (colour bits only; the address / latch / OE bits are
//        stripped -- the dump loop and the PWM pulser own those lines)
//     -> GpioBackend::submitFrame: the refresh thread clocks the GPIO
//        words out through GPSET0/GPCLR0, three writes a column, rows
//        outer and planes inner (hub75_gpio_dump.h), while the PWM
//        pulser times OE.
//
// Threading: the render thread publishes RGB and never waits -- publishRgb
// is one memcpy plus one atomic exchange, so it is safe from MoonLight's
// MM_NONBLOCKING tick. The refresh thread owns the encode and the submit:
// it takes the freshest RGB frame, encodes it into the encoded buffer the
// submit is NOT reading, swaps, and submits. The live encoded buffer is
// never rewritten (the LEDscape failure mode). If the producer is slower
// than the display, the refresh thread simply re-submits the current
// frame, so the panel keeps refreshing at the dump rate regardless.
// If the producer is faster, a publish overwrites the RGB frame still
// waiting -- never the one being encoded, never by waiting.
//
// The refresh thread asks for realtime scheduling (SCHED_FIFO, priority
// 99, pinned to the last CPU) at loop start, best effort: without it,
// scheduling jitter lands in the column timing as visible tearing. It
// needs root (or CAP_SYS_NICE), which /dev/mem access needs anyway, so
// a working backend usually has it; when the request fails the engine
// runs on regardless and the seam reports it as a warning.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "platform/Hub75Bus.h"   // Hub75Geometry: the per-chain encode contract
#include "platform/nonblocking.h"   // MM_NONBLOCKING: publishRgb is tick-safe
#include "platform/platform.h"   // Hub75GpioOptions
#include "platform/rpi/hub75_gpio_backend.h"
#include "platform/rpi/hub75_gpio_repack.h"

namespace mm::platform {

// Whether `o` is a wall the engine can drive: the panel counts, the scan
// rate, the depth, the LSB time, the slowdown and the per-chain geometry
// the encoder needs. The open refuses with `error` when it is not; the
// test recorder calls this too, so it refuses what the engine would refuse.
bool hub75GpioCheckOptions(const Hub75GpioOptions& o, std::string& error);

// One frame's predicted wire time in ns, COMPUTED -- never a measurement
// -- from the column-time model (hub75GpioColumnTimeNs, bench-calibrated
// on the Pi 4 sign -- see that function's comment for the calibration
// and its +-3% tolerance): per (plane, row-group), the longer of the
// group's shift (columns at the column time, plus the 0.5 us dark
// interval for the address + latch) and the plane's OE time
// (lsbNs x 2^p). At 6-bit and 130 ns LSB every plane is shift-bound and
// the frame is the shift alone; at large LSB times the top planes go
// OE-bound. The stub backend's wire-time sleep uses the same model, so
// host timing and the prediction agree by construction.
uint64_t hub75GpioPredictedFrameNs(uint16_t scanRows, uint16_t columnsPerGroup,
                                  uint8_t bitDepth, uint16_t lsbNs, uint16_t columnNs);

class Hub75GpioEngine {
  public:
    Hub75GpioEngine() = default;
    ~Hub75GpioEngine() { shutdown(); }

    Hub75GpioEngine(const Hub75GpioEngine&) = delete;
    Hub75GpioEngine& operator=(const Hub75GpioEngine&) = delete;

    // Validate the options, allocate buffers, open the backend, start
    // the PWM pulser and the refresh thread. On failure fills `error`
    // and returns false; nothing is left running. `useStubBackend`
    // is the engine test's dry run: production always passes false, and
    // a host without Pi hardware fails here with that reason.
    bool open(const Hub75GpioOptions& o, std::string& error, bool useStubBackend = false);

    // Blank-first shutdown: stop the refresh thread, submit a blank
    // frame, wait for it to drain, mux OE back to a GPIO output driving
    // high (dark), stop the PWM, release everything. Idempotent and
    // safe to call after a failed open(). A second open() after a
    // shutdown re-acquires the hardware from scratch (NOTE.md rule 3).
    void shutdown();

    bool isRunning() const { return running_.load(); }

    // Publish one RGB frame of width()*height()*3 bytes. Never blocks and
    // never allocates: one memcpy into the next triple-buffer slot plus one
    // atomic exchange. A frame published while the refresh thread still
    // holds an earlier one supersedes it; the panel shows the newest.
    void publishRgb(const uint8_t* rgb) MM_NONBLOCKING;

    // Brightness for subsequently encoded frames (0..255), as lit time.
    // Takes effect on the next encode; the frame currently displaying is
    // untouched.
    void setBrightness(uint8_t brightness) MM_NONBLOCKING { brightness_.store(brightness); }

    // Measured refresh in Hz, from the submit counter over a ~1 s window.
    uint16_t fps() const MM_NONBLOCKING { return fps_.load(); }

    // -- geometry / timing (valid after open) --
    // The logical canvas the output publishes: cols*chain by rows*parallel.
    int width() const { return virtWidth_; }
    int height() const { return virtHeight_; }
    int chains() const { return chains_; }
    // The driven chain's size: the logical chain under normal, half the
    // rows and double the columns under Stripe (the same pixel count).
    int drivenWidth() const { return drivenWidth_; }
    int drivenHeight() const { return drivenHeight_; }
    // GPIO data words per frame (== pixel clocks per frame, per chain).
    size_t frameSlots() const { return frameSlots_; }
    // The column time the prediction assumes, after slowdown resolution.
    uint16_t columnTimeNs() const { return columnTimeNs_; }
    // The LSB time the PWM actually produces (divider quantisation).
    uint16_t lsbActualNs() const { return lsbActualNs_; }
    // Predicted refresh rate in Hz (COMPUTED from the column-time
    // model -- see hub75GpioPredictedFrameNs -- never a measurement).
    double refreshHz() const {
        const uint64_t ns = predictedFrameNs_;
        return ns ? 1e9 / static_cast<double>(ns) : 0.0;
    }
    // Whether the refresh thread holds realtime scheduling (meaningful
    // once the thread has started; false until it reports).
    bool realtimeFailed() const { return realtimeState_.load() == 2; }
    const char* backendName() const {
        return backend_ ? backend_->name() : "(none)";
    }

    // -- stats (thread-safe) --
    uint64_t framesSubmitted() const { return framesSubmitted_.load(); }
    uint64_t framesEncoded() const { return framesEncoded_.load(); }
    uint64_t framesPublished() const { return framesPublished_.load(); }
    uint64_t submitErrors() const { return submitErrors_.load(); }
    uint64_t pulseTimeouts() const { return pulseTimeouts_.load(); }
    // Last encode duration in microseconds (encode runs on the refresh thread).
    uint64_t lastEncodeUs() const { return lastEncodeUs_.load(); }
    // Last submit duration in microseconds: the dump time of one frame,
    // the number the lab reads to check the column-time model.
    uint64_t lastSubmitUs() const { return lastSubmitUs_.load(); }

  private:
    void refreshLoop();
    // Encode the RGB frame in `rgb` into encoded_[index].
    bool encodeFrame(const uint8_t* rgb, int index);
    // Encode one chain's strip into slotWords (scratch, per-chain).
    bool encodeChain(const uint8_t* rgb, int chain, uint16_t* slotWords);

    Hub75GpioOptions options_{};
    int chains_ = 0;
    int chainWidth_ = 0;    // one chain's logical width: cols*chain
    int chainHeight_ = 0;   // one chain's logical height: rows
    int drivenWidth_ = 0;   // one chain's driven width: chainWidth_, doubled under Stripe
    int drivenHeight_ = 0;  // one chain's driven height: chainHeight_, halved under Stripe
    int virtWidth_ = 0;
    int virtHeight_ = 0;
    size_t frameSlots_ = 0;      // pixel clocks per frame, per chain
    size_t groupWords_ = 0;      // slots per (plane, row) group
    uint16_t columnTimeNs_ = 0;
    uint16_t lsbActualNs_ = 0;
    uint64_t predictedFrameNs_ = 0;

    GpioRepackTables tables_{};

    // The RGB triple buffer, lock-free with one producer, the render
    // thread: ready_ holds a slot index in its low bits and kFresh when the
    // producer has filled it since the refresh thread last took it. Same
    // shape as the PIO engine's and the rgbMatrix seam's recorder.
    static constexpr uint8_t kFresh = 4;
    std::vector<uint8_t> rgb_[3];
    std::atomic<uint8_t> rgbReady_{0};
    uint8_t rgbWriteSlot_ = 1;   // the producer's slot; the refresh thread never touches it
    uint8_t rgbReadSlot_ = 2;    // the refresh thread's slot; the producer never touches it

    std::vector<uint8_t> strip_;              // one chain's strip, RGB24
    std::vector<uint16_t> slots_[kGpioMaxChains];  // one chain's slot stream
    std::vector<uint32_t> encoded_[2];        // GPIO data-word frames
    std::vector<uint32_t> blank_;             // all-dark GPIO data-word frame

    std::unique_ptr<GpioBackend> backend_;
    std::thread refreshThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    int current_ = 0;    // encoded_ index being submitted (refresh thread only)
    std::atomic<uint8_t> brightness_{255};
    std::atomic<uint16_t> fps_{0};
    std::atomic<int> realtimeState_{0};   // 0 unknown, 1 realtime held, 2 request failed

    std::atomic<uint64_t> framesSubmitted_{0};
    std::atomic<uint64_t> framesEncoded_{0};
    std::atomic<uint64_t> framesPublished_{0};
    std::atomic<uint64_t> submitErrors_{0};
    std::atomic<uint64_t> pulseTimeouts_{0};
    std::atomic<uint64_t> lastEncodeUs_{0};
    std::atomic<uint64_t> lastSubmitUs_{0};
};

}  // namespace mm::platform
