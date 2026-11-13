// hub75_dma_engine.cc -- see hub75_dma_engine.h.
//
// SPDX-License-Identifier: GPL-3.0-or-later

// _GNU_SOURCE first: sched_setaffinity / CPU_SET need it, and the define
// must precede every system header this TU includes.
#define _GNU_SOURCE 1

#include "platform/rpi/hub75_dma_engine.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#ifdef __linux__
#include <sched.h>
#include <unistd.h>
#endif

#include "platform/Hub75Encode.h"   // hub75Encode: the in-tree encoder
#include "platform/rpi/hub75_dma_pwm.h"   // hub75DmaPwmTiming: the actual LSB time

namespace mm::platform {

namespace {

// The longest chain the card offers (Hub75Canvas.h's kChainMax, which this
// file cannot include: the platform layer compiles without light/). The
// engine refuses longer ones rather than allocating a frame for them.
constexpr uint8_t kDmaChainMax = 32;

// The dark interval per row-group in ns: the address + latch writes the
// dump loop makes while OE is off. The prediction and the stub's
// wire-time sleep share this value.
constexpr uint16_t kDarkIntervalNs = 500;

#ifdef __linux__
// Ask for realtime scheduling on the calling (refresh) thread:
// SCHED_FIFO at priority 99, pinned to the last CPU. Best effort --
// without CAP_SYS_NICE the calls fail and the engine runs on
// regardless, recording the failure for the seam's warning. Priority
// 99 and the last-CPU pin mirror the reference design's thread setup
// (priority 99, pinned to CPU 3, isolcpus recommended).
void setupRealtime(std::atomic<int>& state) {
    sched_param param{};
    param.sched_priority = 99;
    if (::sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
        state.store(2);
        return;
    }
    const long ncpu = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu > 1) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(static_cast<int>(ncpu - 1), &set);
        (void)::sched_setaffinity(0, sizeof(set), &set);   // best effort
    }
    state.store(1);
}
#endif

}  // namespace

uint64_t hub75DmaPredictedFrameNs(uint16_t scanRows, uint16_t columnsPerGroup,
                                  uint8_t bitDepth, uint16_t lsbNs, uint16_t columnNs) {
    uint64_t frameNs = 0;
    for (uint8_t p = 0; p < bitDepth; p++) {
        const uint64_t shiftNs =
            static_cast<uint64_t>(columnsPerGroup) * columnNs + kDarkIntervalNs;
        const uint64_t oeNs = static_cast<uint64_t>(lsbNs) * (1u << p);
        frameNs += static_cast<uint64_t>(scanRows) * (oeNs > shiftNs ? oeNs : shiftNs);
    }
    return frameNs;
}

bool hub75DmaCheckOptions(const Hub75DmaOptions& o, std::string& error) {
    if (o.parallel < 1 || o.parallel > kDmaMaxChains) {
        error = "parallel must be 1..3 (the board drives 3 chains)";
        return false;
    }
    if (o.chain < 1 || o.chain > kDmaChainMax) {
        error = "chain must be 1..32 panels";
        return false;
    }
    if (o.rows == 0 || o.cols == 0) {
        error = "rows and cols must be non-zero";
        return false;
    }
    if (o.scanRate != 8 && o.scanRate != 16 && o.scanRate != 32) {
        error = "scanRate must be 8, 16 or 32 (1/64 is not supported: the "
                "encoder has no 6th address line and the board no F line)";
        return false;
    }
    if (o.bitDepth < 2 || o.bitDepth > 6) {
        error = "bitDepth must be 2..6 (MoonLight encoder limit)";
        return false;
    }
    if (o.pwmLsbNs < kDmaPwmLsbMinNs || o.pwmLsbNs > kDmaPwmLsbMaxNs) {
        error = "pwmLsbNs must be 50..3000 ns";
        return false;
    }
    if (o.slowdown < -1 || o.slowdown > 4) {
        error = "slowdown must be -1 (auto) or 0..4";
        return false;
    }
    const int chainWidth = static_cast<int>(o.cols) * o.chain;
    const int virtHeight = static_cast<int>(o.rows) * o.parallel;
    if (chainWidth > 65535 || virtHeight > 65535) {
        error = "the panels form a canvas wider or taller than 65,535";
        return false;
    }

    // The encoder validates the per-chain geometry: height must be a
    // multiple of scanRate with an even rows-per-scan (each address step
    // drives an upper and a lower row together). A 32-high panel at 1/32
    // scan fails here -- use 1/16 for 32-high panels.
    Hub75Geometry geo;
    geo.width = static_cast<uint16_t>(chainWidth);
    geo.height = o.rows;
    geo.scanRate = o.scanRate;
    geo.bitDepth = o.bitDepth;
    geo.brightness = 255;
    if (!geo.valid()) {
        error = "invalid chain geometry: height must be a multiple of the "
                "scan rate with an even rows-per-scan (e.g. 64-high at "
                "1/32, 32-high at 1/16)";
        return false;
    }
    return true;
}

bool Hub75DmaEngine::open(const Hub75DmaOptions& o, std::string& error, bool useStubBackend) {
    if (running_) {
        error = "DMA engine already running";
        return false;
    }
    if (!hub75DmaCheckOptions(o, error)) return false;
    options_ = o;

    chains_ = options_.parallel;
    chainWidth_ = static_cast<int>(options_.cols) * options_.chain;
    chainHeight_ = options_.rows;
    virtWidth_ = chainWidth_;
    virtHeight_ = chainHeight_ * chains_;

    Hub75Geometry geo;
    geo.width = static_cast<uint16_t>(chainWidth_);
    geo.height = static_cast<uint16_t>(chainHeight_);
    geo.scanRate = options_.scanRate;
    geo.bitDepth = options_.bitDepth;
    geo.brightness = 255;
    frameSlots_ = geo.frameSlots();
    groupWords_ = static_cast<size_t>(chainWidth_) * (geo.rowsPerScan() / 2);

    tables_ = buildDmaRepackTables();
    lsbActualNs_ = hub75DmaPwmTiming(options_.pwmLsbNs).lsbActualNs;

    const size_t rgbBytes = static_cast<size_t>(virtWidth_) * virtHeight_ * 3;
    for (auto& slot : rgb_) slot.assign(rgbBytes, 0);
    rgbReady_.store(0);
    rgbWriteSlot_ = 1;
    rgbReadSlot_ = 2;
    strip_.resize(static_cast<size_t>(chainWidth_) * chainHeight_ * 3);
    for (int c = 0; c < chains_; c++)
        slots_[c].resize(frameSlots_);
    for (int i = 0; i < 2; i++) encoded_[i].resize(frameSlots_);
    blank_.assign(frameSlots_, 0);   // all colour bits clear: the OE pulses light nothing

    // Encode the (black) RGB buffer into both frames so the refresh
    // thread has something valid to submit from the first iteration.
    if (!encodeFrame(rgb_[rgbReadSlot_].data(), 0) ||
        !encodeFrame(rgb_[rgbReadSlot_].data(), 1)) {
        error = "initial encode failed (encoder refused the geometry)";
        return false;
    }

    backend_.reset(createDmaBackend(useStubBackend));
    if (!backend_) {
        error = "no DMA backend in this build";
        return false;
    }
    if (!backend_->open(error)) {
        backend_.reset();
        return false;
    }
    DmaSubmitConfig config;
    config.chainWidth = static_cast<uint16_t>(chainWidth_);
    config.scanRows = options_.scanRate;
    config.pairs = geo.rowsPerScan() / 2;
    config.bitDepth = options_.bitDepth;
    config.lsbNs = options_.pwmLsbNs;
    config.slowdown = options_.slowdown;
    config.clockFallingEdge = options_.clockFallingEdge;
    if (!backend_->start(config, error)) {
        backend_->close();
        backend_.reset();
        return false;
    }
    // The prediction uses the resolved slowdown, so a Pi 4 on auto
    // quotes its slowdown-2 rate and a Pi 3 its slowdown-1 rate.
    columnTimeNs_ = hub75DmaColumnTimeNs(backend_->resolvedSlowdown());
    predictedFrameNs_ = hub75DmaPredictedFrameNs(
        options_.scanRate, static_cast<uint16_t>(groupWords_), options_.bitDepth,
        options_.pwmLsbNs, columnTimeNs_);

    stop_.store(false);
    current_ = 0;
    framesSubmitted_ = 0;
    framesEncoded_ = 0;
    framesPublished_ = 0;
    submitErrors_ = 0;
    pulseTimeouts_ = 0;
    lastSubmitUs_ = 0;
    realtimeState_.store(0);
    fps_.store(0);
    running_ = true;
    try {
        refreshThread_ = std::thread(&Hub75DmaEngine::refreshLoop, this);
    } catch (...) {
        running_ = false;
        backend_->close();
        backend_.reset();
        error = "failed to start the refresh thread";
        return false;
    }
    return true;
}

void Hub75DmaEngine::shutdown() {
    if (!running_ && !backend_) return;
    stop_.store(true);
    if (refreshThread_.joinable()) refreshThread_.join();
    running_ = false;
    if (backend_) {
        // Blank-first: the backend dumps the blank frame, waits for it
        // to drain, muxes OE back to a GPIO output driving high, and
        // only then stops the PWM. The panel is left dark. A failure
        // here has no caller to report to -- the seam's close returns
        // void, as the PIO seam's does -- so it is counted, not printed.
        std::string error;
        if (!backend_->blankAndStop(blank_.data(), blank_.size(), error)) {
            submitErrors_++;
        }
        backend_->close();
        backend_.reset();
    }
}

void Hub75DmaEngine::publishRgb(const uint8_t* rgb) MM_NONBLOCKING {
    if (!running_ || rgb == nullptr) return;
    const size_t bytes = rgb_[rgbWriteSlot_].size();
    if (bytes == 0) return;
    std::memcpy(rgb_[rgbWriteSlot_].data(), rgb, bytes);
    rgbWriteSlot_ = rgbReady_.exchange(static_cast<uint8_t>(rgbWriteSlot_ | kFresh),
                                       std::memory_order_acq_rel) &
                    3;
    framesPublished_++;
}

void Hub75DmaEngine::refreshLoop() {
#ifdef __linux__
    setupRealtime(realtimeState_);
#else
    realtimeState_.store(2);   // no realtime scheduling off Linux; the engine runs on regardless
#endif
    uint32_t windowCount = 0;
    auto windowStart = std::chrono::steady_clock::now();
    while (!stop_.load()) {
        // Take the freshest RGB frame, if the producer published one since
        // the last take; otherwise re-encode nothing and re-submit.
        bool fresh = false;
        if (rgbReady_.load(std::memory_order_acquire) & kFresh) {
            rgbReadSlot_ = rgbReady_.exchange(rgbReadSlot_, std::memory_order_acq_rel) & 3;
            fresh = true;
        }
        if (fresh) {
            const int back = 1 - current_;
            if (encodeFrame(rgb_[rgbReadSlot_].data(), back)) current_ = back;
            // A failed encode keeps the last good frame submitting: a
            // stale picture beats a torn one, and the geometry was
            // validated at open, so this is unreachable in practice.
        }
        const auto t0 = std::chrono::steady_clock::now();
        const int rc = backend_->submitFrame(
            encoded_[current_].data(), encoded_[current_].size());
        lastSubmitUs_ = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0)
                .count());
        pulseTimeouts_ = backend_->pulseTimeouts();
        if (rc == 0) {
            framesSubmitted_++;
            windowCount++;
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - windowStart);
            if (elapsed.count() >= 1000) {
                fps_.store(static_cast<uint16_t>(windowCount * 1000 / elapsed.count()));
                windowCount = 0;
                windowStart = now;
            }
        } else {
            // Don't spin on a wedged backend: count it, pause briefly, retry.
            submitErrors_++;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}

bool Hub75DmaEngine::encodeFrame(const uint8_t* rgb, int index) {
    const auto t0 = std::chrono::steady_clock::now();
    for (int c = 0; c < chains_; c++) {
        if (!encodeChain(rgb, c, slots_[c].data())) return false;
    }
    const uint16_t* chainSlots[kDmaMaxChains] = {slots_[0].data(),
                                                 chains_ > 1 ? slots_[1].data() : nullptr,
                                                 chains_ > 2 ? slots_[2].data() : nullptr};
    repackSlotsToGpio(chainSlots, chains_, frameSlots_, tables_,
                      encoded_[index].data());
    lastEncodeUs_ = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0)
            .count());
    framesEncoded_++;
    return true;
}

bool Hub75DmaEngine::encodeChain(const uint8_t* rgb, int chain, uint16_t* slotWords) {
    // Chains stack vertically and span the full canvas width, so a chain's
    // strip is one contiguous run of the virtual frame -- one memcpy, no
    // per-pixel walk -- and no gamma: MoonLight's correction already
    // applied the curve before the frame reached the seam.
    const size_t stripBytes = static_cast<size_t>(chainWidth_) * chainHeight_ * 3;
    std::memcpy(strip_.data(),
                rgb + static_cast<size_t>(chain) * stripBytes,
                stripBytes);
    Hub75Geometry geo;
    geo.width = static_cast<uint16_t>(chainWidth_);
    geo.height = static_cast<uint16_t>(chainHeight_);
    geo.scanRate = options_.scanRate;
    geo.bitDepth = options_.bitDepth;
    geo.brightness = brightness_.load();
    // Default layout (the repack assumes it; see hub75_dma_repack.h).
    const size_t bytes = hub75Encode(
        strip_.data(), reinterpret_cast<uint8_t*>(slotWords), geo);
    return bytes == geo.frameBytes();
}

}  // namespace mm::platform
