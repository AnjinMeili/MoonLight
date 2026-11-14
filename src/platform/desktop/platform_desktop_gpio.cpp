// SPDX-License-Identifier: GPL-3.0-or-later
/// The Raspberry Pi 3/4 platform layer behind the GPIO seam (platform.h): a recorder for the tests on every host, and the GPIO engine where the host is a Pi 3 or 4.
///
/// A host that is not a Pi 3 or 4 refuses every open with that reason, so a GPIO output says why its panels stay dark. A Pi 3/4 opens the engine, which drives the panels from its own refresh thread (CPU bit-bang + PWM OE pulser; see hub75_gpio_engine.h for why the engine bit-bangs from the CPU instead of streaming by DMA).

#include "platform/platform.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "platform/rpi/hub75_gpio_backend.h"
#include "platform/rpi/hub75_gpio_engine.h"

namespace mm::platform {

namespace {

std::atomic<uint16_t> width_{0};
std::atomic<uint16_t> height_{0};
std::atomic<uint8_t>  brightness_{255};
std::string lastError_;
std::string warning_;   // what degrades the running panels; empty in this slice, kept for the clauses to come

// The triple buffer, lock-free with one producer, the driver: ready_ holds a slot index in its low bits and kFresh when the producer has filled it since the consumer last took it.
constexpr uint8_t kFresh = 4;
std::atomic<uint8_t> ready_{0};
uint8_t writeSlot_ = 1;

// The frame slots the consumer takes from, on the heap for the process's life, so a consumer on another thread never reads freed slots at exit.
struct Shared {
    std::vector<uint8_t> slots[3];
};
Shared* const shared_ = new Shared;

bool recording_ = false;
Hub75GpioOptions testOptions_;
size_t testFrames_ = 0;
uint8_t testReadSlot_ = 2;   // the test's consumer of the triple buffer

// The engine, one per process: the output strategy allows one GPIO driver,
// and the seam allows one open.
Hub75GpioEngine engine_;
bool realOpen_ = false;   // whether the engine, rather than the recorder, holds the panels

size_t frameBytes() MM_NONBLOCKING { return static_cast<size_t>(width_.load()) * height_.load() * 3; }

// Keep the options and stand in for panels of the size they wire, so a test reads what a driver opened and pushed.
bool recordOpen(const Hub75GpioOptions& o) {
    if (!hub75GpioCheckOptions(o, lastError_)) {
        warning_.clear();
        return false;
    }
    testOptions_ = o;
    width_ = static_cast<uint16_t>(o.cols * o.chain);
    height_ = static_cast<uint16_t>(o.rows * o.parallel);
    // The triple buffer a consumer takes from, so a test runs the whole handover.
    for (auto& s : shared_->slots) s.assign(frameBytes(), 0);
    ready_.store(0, std::memory_order_release);
    writeSlot_ = 1;
    testReadSlot_ = 2;
    testFrames_ = 0;
    return true;
}

}  // namespace

bool hub75GpioAvailable() {
    // Offered where the host is a Pi 3 or 4 -- the engine needs no
    // external library, so the model is the gate -- or where the
    // recorder runs. /dev/mem access and the audio-driver conflict are
    // checked at open(), so their reasons reach the status line
    // instead of hiding the backend.
    return recording_ || hub75GpioHostRefusal() == nullptr;
}

int hub75GpioResolvedSlowdown(int8_t slowdown) {
    return hub75GpioResolveSlowdown(slowdown, hub75GpioHostModel());
}

uint16_t hub75GpioPredictedColumnTimeNs(int slowdown) {
    return hub75GpioColumnTimeNs(slowdown);
}

bool hub75GpioOpen(const Hub75GpioOptions& o) {
    hub75GpioClose();
    lastError_.clear();
    warning_.clear();
    if (recording_) return recordOpen(o);
    if (!engine_.open(o, lastError_)) {
        warning_.clear();
        return false;
    }
    realOpen_ = true;
    width_ = static_cast<uint16_t>(engine_.width());
    height_ = static_cast<uint16_t>(engine_.height());
    return true;
}

const char* hub75GpioLastError() { return lastError_.empty() ? nullptr : lastError_.c_str(); }

const char* hub75GpioWarning() {
    // Live clause: the refresh thread asked for realtime scheduling at
    // loop start, and the request failed -- the panel still runs, but
    // scheduling jitter lands in the column timing as tearing.
    if (realOpen_ && engine_.realtimeFailed())
        return "refresh thread has no real-time priority: timing may jitter (run as root)";
    return warning_.empty() ? nullptr : warning_.c_str();
}

uint16_t hub75GpioWidth() MM_NONBLOCKING { return width_.load(); }

uint16_t hub75GpioHeight() MM_NONBLOCKING { return height_.load(); }

void hub75GpioPush(const uint8_t* rgb) MM_NONBLOCKING {
    if (realOpen_) {
        engine_.publishRgb(rgb);   // the refresh thread encodes and submits it; never blocks
        return;
    }
    const size_t bytes = frameBytes();
    if (!rgb || bytes == 0 || shared_->slots[writeSlot_].size() < bytes) return;   // a canvas with no slots yet, such as a late open's
    std::memcpy(shared_->slots[writeSlot_].data(), rgb, bytes);
    writeSlot_ = ready_.exchange(static_cast<uint8_t>(writeSlot_ | kFresh), std::memory_order_acq_rel) & 3;
    if (recording_) testFrames_++;
}

void hub75GpioCorrection(uint8_t brightness) MM_NONBLOCKING {
    brightness_.store(brightness, std::memory_order_relaxed);
    if (realOpen_) engine_.setBrightness(brightness);
}

uint16_t hub75GpioFps() MM_NONBLOCKING {
    return realOpen_ ? engine_.fps() : 0;
}

// A closed seam holds no memory: the frames go back, the recorded options to their defaults.
void hub75GpioClose() {
    engine_.shutdown();   // blank-first; harmless when it never opened
    realOpen_ = false;
    width_ = 0;
    height_ = 0;
    ready_.store(0, std::memory_order_release);   // the last push stays unshown: its slot goes below
    for (auto& s : shared_->slots) std::vector<uint8_t>().swap(s);
    testFrames_ = 0;
    testOptions_ = Hub75GpioOptions{};
    std::string().swap(warning_);
}

void setTestHub75Gpio(bool record) {
    hub75GpioClose();
    recording_ = record;
}

const Hub75GpioOptions& hub75GpioTestOptions() { return testOptions_; }

size_t hub75GpioTestFrameCount() { return testFrames_; }

// The consumer's side of the triple buffer: the newest frame pushed, always whole.
const uint8_t* hub75GpioTestFrame() {
    if (!testFrames_) return nullptr;
    if (ready_.load(std::memory_order_acquire) & kFresh)
        testReadSlot_ = ready_.exchange(testReadSlot_, std::memory_order_acq_rel) & 3;
    return shared_->slots[testReadSlot_].data();
}

uint8_t hub75GpioTestBrightness() { return brightness_.load(); }

}  // namespace mm::platform
