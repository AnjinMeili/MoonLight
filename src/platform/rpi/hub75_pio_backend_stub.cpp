// hub75_pio_backend_stub.cpp -- PioBackend with no hardware, for compile checks,
// timing simulation and dry runs on a machine with no Pi attached.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// submitFrame() sleeps for the time the frame would take on the wire at
// the configured clock divider, so the refresh thread paces itself the
// way it would on hardware and the swap handshake is exercised for real.
// It also keeps a copy of the last submitted frame, so an engine test can
// read back the wide words without hardware (see hub75_pio_backend_stub.h).
// Nothing here touches /dev/pio0.

#include "platform/rpi/hub75_pio_backend.h"
#include "platform/rpi/hub75_pio_backend_stub.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace mm::platform {

namespace {

// The capture: one frame's worth of wide words, process-wide, since the
// engine owns one backend at a time. A wall frame is 196 KiB; the cap
// holds the largest frame the engine can build (32 chains are not a
// thing here: 3 chains of 512x64 at 6-bit is under 600 KiB).
constexpr size_t kCaptureMaxBytes = 1024 * 1024;
std::mutex captureMu_;
std::vector<uint32_t> capture_;
uint64_t submitted_ = 0;

class StubBackend : public PioBackend {
  public:
    bool open(std::string& /*error*/) override {
        opened_ = true;
        return true;
    }

    bool start(const uint16_t* /*programInstructions*/, int /*programLength*/,
               int /*wrapTarget*/, int /*wrap*/, uint16_t clkdivInt,
               uint8_t clkdivFrac, uint32_t /*xferBufSize*/,
               uint32_t /*xferBufCount*/, std::string& /*error*/) override {
        // SM clock = 200 MHz / divider; one word per 5 SM cycles.
        const double divider = static_cast<double>(clkdivInt) +
                               static_cast<double>(clkdivFrac) / 256.0;
        secondsPerWord_ = (divider > 0.0)
                              ? (kCyclesPerPixelClock * divider /
                                 static_cast<double>(kRp1PioClockHz))
                              : 0.0;
        started_ = true;
        return true;
    }

    int submitFrame(const void* data, uint32_t bytes) override {
        if (!started_) return -1;
        {
            std::lock_guard<std::mutex> lock(captureMu_);
            submitted_++;
            const size_t keep = std::min<size_t>(bytes, kCaptureMaxBytes);
            capture_.assign(static_cast<size_t>(keep) / 4, 0);
            if (data != nullptr && keep > 0)
                std::memcpy(capture_.data(), data, keep & ~size_t{3});
        }
        const uint32_t words = bytes / kBytesPerPixelClock;
        const double seconds = static_cast<double>(words) * secondsPerWord_;
        if (seconds > 0.0)
            std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
        return 0;
    }

    bool blankAndStop(const void* blankData, uint32_t blankBytes,
                      std::string& /*error*/) override {
        // Simulate the blank frame's wire time, then stop.
        if (started_) (void)submitFrame(blankData, blankBytes);
        started_ = false;
        return true;
    }

    void close() override {
        started_ = false;
        opened_ = false;
    }

    const char* name() const override { return "stub (no hardware)"; }

  private:
    bool opened_ = false;
    bool started_ = false;
    double secondsPerWord_ = 0.0;
};

}  // namespace

PioBackend* createStubBackend() { return new StubBackend(); }

uint64_t stubBackendSubmittedFrames() {
    std::lock_guard<std::mutex> lock(captureMu_);
    return submitted_;
}

size_t stubBackendCopyLastFrame(uint32_t* out, size_t capWords) {
    std::lock_guard<std::mutex> lock(captureMu_);
    if (!out || capture_.empty()) return 0;
    const size_t n = capture_.size() < capWords ? capture_.size() : capWords;
    std::memcpy(out, capture_.data(), n * 4);
    return n;
}

void stubBackendResetCapture() {
    std::lock_guard<std::mutex> lock(captureMu_);
    capture_.clear();
    submitted_ = 0;
}

}  // namespace mm::platform
