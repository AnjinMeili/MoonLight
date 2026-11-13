// hub75_pio_engine.cc -- see hub75_pio_engine.h.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/rpi/hub75_pio_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#include "platform/Hub75Encode.h"   // hub75Encode: the in-tree encoder
#include "platform/rpi/hub75_pio_pins.h"

// The pioasm-generated program data. PICO_NO_HARDWARE is defined by the
// build so this header only provides the instruction arrays and wrap
// defines -- no piolib types are needed here.
#define PICO_NO_HARDWARE 1
#include "platform/rpi/hub75.pio.h"

namespace mm::platform {

namespace {

// Kernel DMA bounce-buffer sizing (rp1-pio driver: at most 4 bounce
// buffers, buf_size a multiple of 4). 64 KiB x 4 covers a 256 KiB frame
// without mid-frame waits; smaller frames just use fewer/smaller chunks.
constexpr uint32_t kXferBufSize = 65536;
constexpr uint32_t kXferBufCount = 4;

// The longest chain the card offers (Hub75Canvas.h's kChainMax, which this
// file cannot include: the platform layer compiles without light/). The
// engine refuses longer ones rather than allocating a frame for them.
constexpr uint8_t kPioChainMax = 32;

}  // namespace

bool hub75PioCheckOptions(const Hub75PioOptions& o, std::string& error) {
    if (o.parallel < 1 || o.parallel > kMaxChains) {
        error = "parallel must be 1..3 (the board drives 3 chains)";
        return false;
    }
    if (o.chain < 1 || o.chain > kPioChainMax) {
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
    if (o.latchBlankWords > 8) {
        error = "latchBlankWords must be 0..8";
        return false;
    }
    if (o.pixelClockHz == 0) {
        error = "pixelClockHz must be non-zero";
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

bool Hub75PioEngine::open(const Hub75PioOptions& o, std::string& error, bool useStubBackend) {
    if (running_) {
        error = "PIO engine already running";
        return false;
    }
    if (!hub75PioCheckOptions(o, error)) return false;
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
    const uint16_t pairs = geo.rowsPerScan() / 2;
    rowWords_ = static_cast<size_t>(chainWidth_) * pairs;

    // Clock divider: pixel clock = 200 MHz / (5 * divider).
    double divider = static_cast<double>(kRp1PioClockHz) /
                     (static_cast<double>(kCyclesPerPixelClock) *
                      static_cast<double>(options_.pixelClockHz));
    if (divider < 1.0) divider = 1.0;  // pixel clock tops out at 40 MHz
    uint32_t divInt = static_cast<uint32_t>(std::floor(divider));
    uint32_t divFrac =
        static_cast<uint32_t>(std::lround((divider - divInt) * 256.0));
    if (divFrac >= 256) {
        divInt++;
        divFrac = 0;
    }
    if (divInt < 1) divInt = 1;
    if (divInt > 65535) divInt = 65535;
    clkdivInt_ = static_cast<uint16_t>(divInt);
    clkdivFrac_ = static_cast<uint8_t>(divFrac);
    const double dividerActual =
        static_cast<double>(clkdivInt_) + static_cast<double>(clkdivFrac_) / 256.0;
    pixelClockActualHz_ = static_cast<uint32_t>(std::lround(
        static_cast<double>(kRp1PioClockHz) /
        (static_cast<double>(kCyclesPerPixelClock) * dividerActual)));

    tables_ = buildRepackTables();

    const size_t rgbBytes = static_cast<size_t>(virtWidth_) * virtHeight_ * 3;
    for (auto& slot : rgb_) slot.assign(rgbBytes, 0);
    rgbReady_.store(0);
    rgbWriteSlot_ = 1;
    rgbReadSlot_ = 2;
    strip_.resize(static_cast<size_t>(chainWidth_) * chainHeight_ * 3);
    for (int c = 0; c < chains_; c++)
        slots_[c].resize(frameSlots_);
    for (int i = 0; i < 2; i++) encoded_[i].resize(frameSlots_);
    blank_.assign(frameSlots_, kBlankWideWord);

    // Encode the (black) RGB buffer into both frames so the refresh
    // thread has something valid to submit from the first iteration.
    if (!encodeFrame(rgb_[rgbReadSlot_].data(), 0) ||
        !encodeFrame(rgb_[rgbReadSlot_].data(), 1)) {
        error = "initial encode failed (encoder refused the geometry)";
        return false;
    }

    backend_.reset(createPioBackend(useStubBackend));
    if (!backend_) {
        error = "needs a Raspberry Pi build with piolib";
        return false;
    }
    if (!backend_->open(error)) {
        backend_.reset();
        return false;
    }
    const uint16_t* instructions = options_.clockFallingEdge
                                       ? hub75_wide_falling_program_instructions
                                       : hub75_wide_program_instructions;
    const int wrapTarget = options_.clockFallingEdge
                               ? hub75_wide_falling_wrap_target
                               : hub75_wide_wrap_target;
    const int wrap = options_.clockFallingEdge ? hub75_wide_falling_wrap
                                               : hub75_wide_wrap;
    const int programLength = wrap - wrapTarget + 1;
    const uint32_t xferBufSize =
        std::min(kXferBufSize,
                 static_cast<uint32_t>(frameSlots_ * kBytesPerPixelClock));
    if (!backend_->start(instructions, programLength, wrapTarget, wrap,
                         clkdivInt_, clkdivFrac_, xferBufSize, kXferBufCount,
                         error)) {
        backend_->close();
        backend_.reset();
        return false;
    }

    stop_.store(false);
    current_ = 0;
    framesSubmitted_ = 0;
    framesEncoded_ = 0;
    framesPublished_ = 0;
    submitErrors_ = 0;
    fps_.store(0);
    running_ = true;
    try {
        refreshThread_ = std::thread(&Hub75PioEngine::refreshLoop, this);
    } catch (...) {
        running_ = false;
        backend_->close();
        backend_.reset();
        error = "failed to start the refresh thread";
        return false;
    }
    return true;
}

void Hub75PioEngine::shutdown() {
    if (!running_ && !backend_) return;
    stop_.store(true);
    if (refreshThread_.joinable()) refreshThread_.join();
    running_ = false;
    if (backend_) {
        // Blank-first: the backend submits the blank frame, waits for the
        // FIFO to drain, and only then disables the SM. The panel is left
        // dark. A failure here has no caller to report to -- the seam's
        // close returns void, as the rgbMatrix seam's does -- so it is
        // counted, not printed.
        std::string error;
        if (!backend_->blankAndStop(blank_.data(),
                                    static_cast<uint32_t>(blank_.size() * 4),
                                    error)) {
            submitErrors_++;
        }
        backend_->close();
        backend_.reset();
    }
}

void Hub75PioEngine::publishRgb(const uint8_t* rgb) MM_NONBLOCKING {
    if (!running_ || rgb == nullptr) return;
    const size_t bytes = rgb_[rgbWriteSlot_].size();
    if (bytes == 0) return;
    std::memcpy(rgb_[rgbWriteSlot_].data(), rgb, bytes);
    rgbWriteSlot_ = rgbReady_.exchange(static_cast<uint8_t>(rgbWriteSlot_ | kFresh),
                                       std::memory_order_acq_rel) &
                    3;
    framesPublished_++;
}

void Hub75PioEngine::refreshLoop() {
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
        const int rc = backend_->submitFrame(
            encoded_[current_].data(),
            static_cast<uint32_t>(encoded_[current_].size() * 4));
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
            // Don't spin on a wedged SM: count it, pause briefly, retry.
            // A -ETIMEDOUT here means the SM is not draining the FIFO.
            submitErrors_++;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}

bool Hub75PioEngine::encodeFrame(const uint8_t* rgb, int index) {
    const auto t0 = std::chrono::steady_clock::now();
    for (int c = 0; c < chains_; c++) {
        if (!encodeChain(rgb, c, slots_[c].data())) return false;
    }
    const uint16_t* chainSlots[kMaxChains] = {slots_[0].data(),
                                              chains_ > 1 ? slots_[1].data() : nullptr,
                                              chains_ > 2 ? slots_[2].data() : nullptr};
    repackSlots(chainSlots, chains_, frameSlots_, tables_,
                encoded_[index].data());
    if (options_.latchBlankWords > 0)
        applyLatchBlanking(encoded_[index].data(), frameSlots_, rowWords_,
                           options_.latchBlankWords);
    lastEncodeUs_ = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0)
            .count());
    framesEncoded_++;
    return true;
}

bool Hub75PioEngine::encodeChain(const uint8_t* rgb, int chain, uint16_t* slotWords) {
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
    // Default layout (the repack assumes it; see hub75_pio_repack.h).
    const size_t bytes = hub75Encode(
        strip_.data(), reinterpret_cast<uint8_t*>(slotWords), geo);
    return bytes == geo.frameBytes();
}

}  // namespace mm::platform
