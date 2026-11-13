// hub75_pio_engine.h -- HUB75 PIO engine: RGB frames -> RP1 PIO -> panels.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pipeline (one frame):
//
//   RGB virtual frame, published by the PIO output through the seam
//     -> triple-buffered RGB slot (lock-free publish, newest wins)
//     -> refresh thread: per chain, one contiguous strip copy out of the
//        virtual frame (chains stack vertically, so each strip is one
//        memcpy; NO gamma -- MoonLight's correction already applied it)
//     -> MoonLight hub75Encode() into 16-bit slot words (bitplanes, with
//        address / latch / OE baked in, one word per pixel clock)
//     -> repack: three chains' slot words -> one 32-bit wide word each
//     -> optional latch blanking (ESP32-style)
//     -> PIO backend: one synchronous DMA transfer (pio_sm_xfer_data)
//        into the SM's TX FIFO; the SM shifts it out at the pixel clock.
//
// Threading: the render thread publishes RGB and never waits -- publishRgb
// is one memcpy plus one atomic exchange, so it is safe from MoonLight's
// MM_NONBLOCKING tick. The refresh thread owns the encode and the submit:
// it takes the freshest RGB frame, encodes it into the encoded buffer the
// submit is NOT reading, swaps, and submits. The live encoded buffer is
// never rewritten (the LEDscape failure mode). If the producer is slower
// than the display, the refresh thread simply re-submits the current
// frame, so the panel keeps refreshing at the pixel-clock rate regardless.
// If the producer is faster, a publish overwrites the RGB frame still
// waiting -- never the one being encoded, never by waiting.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "platform/Hub75Bus.h"   // Hub75Geometry: the per-chain encode contract
#include "platform/nonblocking.h"   // MM_NONBLOCKING: publishRgb is tick-safe
#include "platform/platform.h"   // Hub75PioOptions
#include "platform/rpi/hub75_pio_backend.h"
#include "platform/rpi/hub75_pio_repack.h"

namespace mm::platform {

// Whether `o` is a wall the engine can drive: the panel counts, the scan
// rate, the depth, the clock and the per-chain geometry the encoder needs.
// The open refuses with `error` when it is not; the test recorder calls
// this too, so it refuses what the engine would refuse.
bool hub75PioCheckOptions(const Hub75PioOptions& o, std::string& error);

class Hub75PioEngine {
  public:
    Hub75PioEngine() = default;
    ~Hub75PioEngine() { shutdown(); }

    Hub75PioEngine(const Hub75PioEngine&) = delete;
    Hub75PioEngine& operator=(const Hub75PioEngine&) = delete;

    // Validate the options, allocate buffers, open the PIO backend, load
    // the program, start the SM and the refresh thread. On failure fills
    // `error` and returns false; nothing is left running. `useStubBackend`
    // is the engine test's dry run: production always passes false, and a
    // build without piolib fails here with that reason.
    bool open(const Hub75PioOptions& o, std::string& error, bool useStubBackend = false);

    // Blank-first shutdown: stop the refresh thread, submit a blank
    // frame, wait for the FIFO to drain, disable the SM, release
    // everything. Idempotent and safe to call after a failed open().
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
    int width() const { return virtWidth_; }
    int height() const { return virtHeight_; }
    int chains() const { return chains_; }
    // Pixel clocks per frame (== FIFO words per frame).
    size_t frameSlots() const { return frameSlots_; }
    // Actual pixel clock after clock-divider rounding.
    uint32_t pixelClockHz() const { return pixelClockActualHz_; }
    // Refresh rate in Hz (pixel clock / slots per frame).
    double refreshHz() const {
        return frameSlots_ ? static_cast<double>(pixelClockActualHz_) /
                                 static_cast<double>(frameSlots_)
                           : 0.0;
    }
    // DMA bandwidth in bytes/s at the current pixel clock.
    uint32_t dmaBytesPerSecond() const {
        return pixelClockActualHz_ * kBytesPerPixelClock;
    }
    const char* backendName() const {
        return backend_ ? backend_->name() : "(none)";
    }

    // -- stats (thread-safe) --
    uint64_t framesSubmitted() const { return framesSubmitted_.load(); }
    uint64_t framesEncoded() const { return framesEncoded_.load(); }
    uint64_t framesPublished() const { return framesPublished_.load(); }
    uint64_t submitErrors() const { return submitErrors_.load(); }
    // Last encode duration in microseconds (encode runs on the refresh thread).
    uint64_t lastEncodeUs() const { return lastEncodeUs_.load(); }

  private:
    void refreshLoop();
    // Encode the RGB frame in `rgb` into encoded_[index].
    bool encodeFrame(const uint8_t* rgb, int index);
    // Encode one chain's strip into slotWords (scratch, per-chain).
    bool encodeChain(const uint8_t* rgb, int chain, uint16_t* slotWords);

    Hub75PioOptions options_{};
    int chains_ = 0;
    int chainWidth_ = 0;
    int chainHeight_ = 0;
    int virtWidth_ = 0;
    int virtHeight_ = 0;
    size_t frameSlots_ = 0;      // pixel clocks per frame, per chain
    size_t rowWords_ = 0;        // slots per (plane, row) group
    uint32_t pixelClockActualHz_ = 0;
    uint16_t clkdivInt_ = 1;
    uint8_t clkdivFrac_ = 0;

    RepackTables tables_{};

    // The RGB triple buffer, lock-free with one producer, the render
    // thread: ready_ holds a slot index in its low bits and kFresh when the
    // producer has filled it since the refresh thread last took it. Same
    // shape as the rgbMatrix seam's recorder.
    static constexpr uint8_t kFresh = 4;
    std::vector<uint8_t> rgb_[3];
    std::atomic<uint8_t> rgbReady_{0};
    uint8_t rgbWriteSlot_ = 1;   // the producer's slot; the refresh thread never touches it
    uint8_t rgbReadSlot_ = 2;    // the refresh thread's slot; the producer never touches it

    std::vector<uint8_t> strip_;              // one chain's strip, RGB24
    std::vector<uint16_t> slots_[kMaxChains]; // one chain's slot stream
    std::vector<uint32_t> encoded_[2];        // wide-word frames
    std::vector<uint32_t> blank_;             // all-dark wide-word frame

    std::unique_ptr<PioBackend> backend_;
    std::thread refreshThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    int current_ = 0;    // encoded_ index being submitted (refresh thread only)
    std::atomic<uint8_t> brightness_{255};
    std::atomic<uint16_t> fps_{0};

    std::atomic<uint64_t> framesSubmitted_{0};
    std::atomic<uint64_t> framesEncoded_{0};
    std::atomic<uint64_t> framesPublished_{0};
    std::atomic<uint64_t> submitErrors_{0};
    std::atomic<uint64_t> lastEncodeUs_{0};
};

}  // namespace mm::platform
