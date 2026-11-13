// hub75_gpio_backend_stub.cpp -- GpioBackend with no hardware, for compile checks,
// timing simulation and dry runs on a machine with no Pi attached.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// submitFrame() runs the shared dump loop (hub75_gpio_dump.h) against a
// recording sink, so the unit test reads back the GPIO writes and pulser
// steps the hardware backend would have driven -- the same loop, not a
// copy -- and sleeps for the time the frame would take on the wire at
// the configured column time, so the refresh thread paces itself the
// way it would on hardware and the swap handshake is exercised for real.
// It also keeps a copy of the last submitted frame and the setup-step
// log, so an engine test can read back the GPIO words and pin the
// dark-first setup order without hardware (see hub75_gpio_backend_stub.h).
// Nothing here touches /dev/mem.
//
// The setup log mirrors the hardware backend's open()/start() step for
// step: open() logs dark-values then directions; start() logs
// pwm-config then oe-mux. The order is the NOTE.md rule 1 order
// (values, then directions, then the mux), and the engine test asserts
// the logged string.

#include "platform/rpi/hub75_gpio_backend.h"
#include "platform/rpi/hub75_gpio_backend_stub.h"
#include "platform/rpi/hub75_gpio_dump.h"   // the shared dump loop + GpioDumpEvent
#include "platform/rpi/hub75_gpio_pwm.h"   // kGpioPwmLsbMinNs/MaxNs: the LSB bounds start() validates

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mm::platform {

namespace {

// The capture: one frame's worth of GPIO words and one frame's dump
// stream, process-wide, since the engine owns one backend at a time.
// A wall frame is 196 KiB; the cap holds the largest frame the engine
// can build (3 chains of 512x64 at 6-bit is under 600 KiB). The dump
// stream is ~3 events a column plus the per-group steps -- a wall frame
// is under 600k events, 8 bytes each.
constexpr size_t kCaptureMaxBytes = 1024 * 1024;
std::mutex captureMu_;
std::vector<uint32_t> capture_;
std::vector<GpioDumpEvent> stream_;
uint64_t submitted_ = 0;
std::string setupLog_;

// The dump loop's recording sink: every write and pulser step lands in
// the stream, in order. The LEV reads are the slowdown delays --
// no-ops here, and not recorded (they are timing, not signal).
struct RecordSink {
    std::vector<GpioDumpEvent>* stream = nullptr;
    void writeSet(uint32_t mask) { stream->push_back({GpioDumpEvent::Kind::Set, mask}); }
    void writeClr(uint32_t mask) { stream->push_back({GpioDumpEvent::Kind::Clr, mask}); }
    uint32_t readLev() { return 0; }
    bool waitPulseFinished() {
        stream->push_back({GpioDumpEvent::Kind::PulseWait, 0});
        return true;
    }
    void sendPulse(uint8_t plane) {
        stream->push_back({GpioDumpEvent::Kind::PulseFire, plane});
    }
};

void logStep(const char* step) {
    std::lock_guard<std::mutex> lock(captureMu_);
    if (!setupLog_.empty()) setupLog_ += ",";
    setupLog_ += step;
}

class StubBackend : public GpioBackend {
  public:
    bool open(std::string& /*error*/) override {
        // The hardware backend's open(): dark levels first, then the
        // pins made outputs. Logged, not driven.
        logStep("dark-values");
        logStep("directions");
        opened_ = true;
        return true;
    }

    bool start(const GpioSubmitConfig& config, std::string& error) override {
        if (!opened_) {
            error = "GPIO backend not open";
            return false;
        }
        if (config.chainWidth == 0 || config.scanRows == 0 || config.pairs == 0) {
            error = "GPIO submit config has a zero dimension";
            return false;
        }
        if (config.bitDepth < 2 || config.bitDepth > 6) {
            error = "GPIO bitDepth must be 2..6";
            return false;
        }
        if (config.lsbNs < kGpioPwmLsbMinNs || config.lsbNs > kGpioPwmLsbMaxNs) {
            error = "GPIO LSB time must be 50..3000 ns";
            return false;
        }
        if (config.slowdown < -1 || config.slowdown > 4) {
            error = "GPIO slowdown must be -1 (auto) or 0..4";
            return false;
        }
        // The hardware backend's start(): the PWM configured (idle
        // high), then OE muxed to it.
        logStep("pwm-config");
        logStep("oe-mux");
        config_ = config;
        slowdown_ = hub75GpioResolveSlowdown(config.slowdown, Hub75GpioModel::Unknown);   // the stub has no model: auto is 1
        const uint64_t columnNs = hub75GpioColumnTimeNs(slowdown_);
        const uint64_t groupWords =
            static_cast<uint64_t>(config.chainWidth) * config.pairs;
        // Wire time per frame: columns at the column time, plus the
        // dark interval per row-group (0.5 us: the address + latch
        // writes), plus the OE time where it outlasts a row's shift
        // (only at large LSB times). The same model the engine's
        // predicted refresh uses.
        uint64_t frameNs = 0;
        for (uint8_t p = 0; p < config.bitDepth; p++) {
            const uint64_t shiftNs = groupWords * columnNs + 500;
            const uint64_t oeNs =
                static_cast<uint64_t>(config.lsbNs) * (1u << p);
            frameNs += static_cast<uint64_t>(config.scanRows) *
                       (oeNs > shiftNs ? oeNs : shiftNs);
        }
        secondsPerFrame_ = static_cast<double>(frameNs) / 1e9;
        started_ = true;
        return true;
    }

    int submitFrame(const uint32_t* words, size_t wordCount) override {
        if (!started_) return -1;
        // Run the shared dump loop against the recorder first, outside
        // the lock: the stream is built locally and swapped in whole,
        // so a test reading it always sees one complete frame.
        std::vector<GpioDumpEvent> stream;
        if (words != nullptr) {
            RecordSink sink{&stream};
            uint64_t timeouts = 0;   // the recorder never times out
            (void)hub75GpioDumpFrame(sink, config_, words, wordCount, slowdown_, timeouts);
        }
        {
            std::lock_guard<std::mutex> lock(captureMu_);
            submitted_++;
            const size_t keep =
                std::min<size_t>(wordCount * 4, kCaptureMaxBytes) / 4;
            capture_.assign(keep, 0);
            if (words != nullptr && keep > 0)
                std::memcpy(capture_.data(), words, keep * 4);
            stream_.swap(stream);
        }
        if (secondsPerFrame_ > 0.0)
            std::this_thread::sleep_for(
                std::chrono::duration<double>(secondsPerFrame_));
        return 0;
    }

    bool blankAndStop(const uint32_t* blankWords, size_t blankWordCount,
                      std::string& /*error*/) override {
        // Simulate the blank frame's wire time, then stop.
        if (started_) (void)submitFrame(blankWords, blankWordCount);
        started_ = false;
        return true;
    }

    void close() override {
        started_ = false;
        opened_ = false;
    }

    const char* name() const override { return "stub (no hardware)"; }

    Hub75GpioModel model() const override { return Hub75GpioModel::Unknown; }
    int resolvedSlowdown() const override { return slowdown_; }
    uint64_t pulseTimeouts() const override { return 0; }

  private:
    bool opened_ = false;
    bool started_ = false;
    GpioSubmitConfig config_{};
    int slowdown_ = 1;
    double secondsPerFrame_ = 0.0;
};

}  // namespace

GpioBackend* createGpioStubBackend() { return new StubBackend(); }

uint64_t gpioStubSubmittedFrames() {
    std::lock_guard<std::mutex> lock(captureMu_);
    return submitted_;
}

size_t gpioStubCopyLastFrame(uint32_t* out, size_t capWords) {
    std::lock_guard<std::mutex> lock(captureMu_);
    if (!out || capture_.empty()) return 0;
    const size_t n = capture_.size() < capWords ? capture_.size() : capWords;
    std::memcpy(out, capture_.data(), n * 4);
    return n;
}

std::string gpioStubSetupLog() {
    std::lock_guard<std::mutex> lock(captureMu_);
    return setupLog_;
}

size_t gpioStubCopyLastDumpStream(GpioDumpEvent* out, size_t capEvents) {
    std::lock_guard<std::mutex> lock(captureMu_);
    if (!out || stream_.empty()) return 0;
    const size_t n = stream_.size() < capEvents ? stream_.size() : capEvents;
    std::memcpy(out, stream_.data(), n * sizeof(GpioDumpEvent));
    return n;
}

void gpioStubResetCapture() {
    std::lock_guard<std::mutex> lock(captureMu_);
    capture_.clear();
    stream_.clear();
    submitted_ = 0;
    setupLog_.clear();
}

}  // namespace mm::platform
