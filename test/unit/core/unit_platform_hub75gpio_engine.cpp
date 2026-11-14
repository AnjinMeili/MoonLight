/// @module platform

/// Pins the GPIO engine against the stub backend: the options it refuses, the PWM timing it derives, the GPIO words its refresh thread submits for a known frame, the dark-first setup order, and the blank frame its shutdown leaves. No hardware is touched; the stub captures every submit and logs every setup step.

#include "doctest.h"
#include "platform/platform.h"
#include "platform/rpi/hub75_gpio_backend.h"
#include "platform/rpi/hub75_gpio_backend_stub.h"
#include "platform/rpi/hub75_gpio_engine.h"
#include "platform/rpi/hub75_gpio_multiplex.h"
#include "platform/rpi/hub75_gpio_pins.h"
#include "platform/rpi/hub75_gpio_pwm.h"
#include "platform/rpi/hub75_gpio_repack.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

// One 32x64 panel at 1/16 scan, 2-bit: 2 planes x 16 rows x 64 columns, 2,048 slots a frame.
mm::platform::Hub75GpioOptions small() {
    mm::platform::Hub75GpioOptions o;
    o.rows = 32;
    o.cols = 64;
    o.chain = 1;
    o.parallel = 1;
    o.scanRate = 16;
    o.bitDepth = 2;
    o.pwmLsbNs = 130;
    o.slowdown = -1;   // auto: the stub resolves it to 1
    return o;
}

// Wait up to `ms` for the stub to have submitted a frame whose word `index` has `bit` set, or all of `expect` when `bit` is -1.
bool waitForWord(size_t index, int bit, uint32_t expect, int ms = 2000) {
    std::vector<uint32_t> frame(2048);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (mm::platform::gpioStubCopyLastFrame(frame.data(), frame.size()) > index) {
            if (bit >= 0 ? (frame[index] & (1u << bit)) != 0 : frame[index] == expect) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

// The sign's options (BENCH-V6): 3 parallel chains of one 32x64 panel,
// Stripe, 6-bit, 160 ns LSB, TRUE slowdown 2 -- the card's index 3,
// as found (the card stores the index; slowdown is index - 1, so the
// v4/v5 notes' "slowdown 3" was this setting misread). Frame 64x96,
// 6,144 words.
mm::platform::Hub75GpioOptions signOptions() {
    mm::platform::Hub75GpioOptions o;
    o.rows = 32;
    o.cols = 64;
    o.chain = 1;
    o.parallel = 3;
    o.scanRate = 8;
    o.multiplexing = 1;
    o.bitDepth = 6;
    o.pwmLsbNs = 160;
    o.slowdown = 2;
    return o;
}

// Wait until the engine has encoded a fresh frame and submitted after it.
bool waitFresh(mm::platform::Hub75GpioEngine& e, uint64_t encBefore, uint64_t subBefore) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (e.framesEncoded() > encBefore && e.framesSubmitted() > subBefore + 1) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

// The red bits (R1|R2) on all three chains, as one GPIO mask.
uint32_t redMaskAllChains() {
    uint32_t m = 0;
    for (int c = 0; c < 3; c++)
        m |= (1u << mm::platform::kGpioChainPins[c].r1) |
             (1u << mm::platform::kGpioChainPins[c].r2);
    return m;
}

// One decoded dump group: the data the panel saw clocked in, the address
// it was latched at, and where the pulse wait and fire sat around it.
struct DecodedGroup {
    std::vector<uint32_t> clocked;   // colour bits at each clock edge, in order
    int address = -1;                // the address shadow at the strobe
    int plane = -1;                  // the bitplane of the pulse fire
    size_t waitIdx = 0;              // stream index of the pulse wait
    size_t addrIdx = 0;              // stream index of the last address write (0 when unchanged)
    bool addrChanged = false;
    size_t strobeIdx = 0;            // stream index of the strobe's rising edge
    size_t fireIdx = 0;              // stream index of the pulse fire
};

// The row an address mask selects: A is bit 0, E is bit 4.
int rowFromAddressMask(uint32_t m) {
    int row = 0;
    if (m & (1u << mm::platform::gpio_shared::kA)) row |= 1;
    if (m & (1u << mm::platform::gpio_shared::kB)) row |= 2;
    if (m & (1u << mm::platform::gpio_shared::kC)) row |= 4;
    if (m & (1u << mm::platform::gpio_shared::kD)) row |= 8;
    if (m & (1u << mm::platform::gpio_shared::kE)) row |= 16;
    return row;
}

// Decode a dump stream into its groups, tracking a shadow of the GPIO
// outputs (CLR clears bits, SET sets them). A clock edge is a clock-bit
// TRANSITION -- SET carrying the clock bit while it was low (rising-edge
// panels) or CLR carrying it while it was high (falling-edge panels);
// the colour bits in the shadow at the edge are the data that clock
// clocked in. (The colour clear after each shift also carries the clock
// bit in its mask, but the clock is already at its resting level there,
// so it is not an edge.) Groups are delimited by the pulse fire that
// ends each one.
std::vector<DecodedGroup> decodeDumpStream(const std::vector<mm::platform::GpioDumpEvent>& ev,
                                           bool falling) {
    std::vector<DecodedGroup> groups;
    DecodedGroup cur;
    uint32_t shadow = 0;
    for (size_t i = 0; i < ev.size(); i++) {
        switch (ev[i].kind) {
            case mm::platform::GpioDumpEvent::Kind::Set: {
                const uint32_t m = ev[i].value;
                if (!falling && (m & mm::platform::kGpioClkBit) != 0 &&
                    (shadow & mm::platform::kGpioClkBit) == 0)
                    cur.clocked.push_back(shadow & mm::platform::kGpioColourMask);
                shadow |= m;
                if ((m & mm::platform::kGpioAddressMask) != 0) {
                    cur.addrIdx = i;
                    cur.addrChanged = true;
                }
                if ((m & mm::platform::kGpioStbBit) != 0) {
                    cur.strobeIdx = i;
                    cur.address = rowFromAddressMask(shadow & mm::platform::kGpioAddressMask);
                }
                break;
            }
            case mm::platform::GpioDumpEvent::Kind::Clr: {
                const uint32_t m = ev[i].value;
                if (falling && (m & mm::platform::kGpioClkBit) != 0 &&
                    (shadow & mm::platform::kGpioClkBit) != 0)
                    cur.clocked.push_back(shadow & mm::platform::kGpioColourMask);
                shadow &= ~m;
                if ((m & mm::platform::kGpioAddressMask) != 0) {
                    cur.addrIdx = i;
                    cur.addrChanged = true;
                }
                break;
            }
            case mm::platform::GpioDumpEvent::Kind::PulseWait:
                cur.waitIdx = i;
                break;
            case mm::platform::GpioDumpEvent::Kind::PulseFire:
                cur.plane = static_cast<int>(ev[i].value);
                cur.fireIdx = i;
                groups.push_back(std::move(cur));
                cur = DecodedGroup{};
                break;
        }
    }
    return groups;
}

}  // namespace

TEST_CASE("Hub75GpioEngine: the options are refused with reasons before anything opens") {
    std::string error;
    mm::platform::Hub75GpioOptions o = small();
    CHECK(mm::platform::hub75GpioCheckOptions(o, error));

    o = small(); o.parallel = 0;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = small(); o.parallel = 4;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = small(); o.chain = 0;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = small(); o.chain = 33;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = small(); o.rows = 0;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = small(); o.scanRate = 64;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = small(); o.bitDepth = 1;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = small(); o.bitDepth = 9;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    // 7 and 8 open: the GPIO output goes to 8 real planes, the
    // encoder's limit, like the PIO side.
    o = small(); o.bitDepth = 7;
    CHECK(mm::platform::hub75GpioCheckOptions(o, error));
    o = small(); o.bitDepth = 8;
    CHECK(mm::platform::hub75GpioCheckOptions(o, error));
    o = small(); o.pwmLsbNs = 49;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = small(); o.pwmLsbNs = 3001;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = small(); o.slowdown = -2;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = small(); o.slowdown = 5;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    o = small(); o.multiplexing = 2;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    // Stripe is the one mapper the engine runs -- at the driven scan,
    // rows/4: a 32-high Stripe panel is 1/8, and the direct 1/16 scan
    // with Stripe is refused.
    o = small(); o.multiplexing = 1; o.scanRate = 8;
    CHECK(mm::platform::hub75GpioCheckOptions(o, error));
    o = small(); o.multiplexing = 1;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    CHECK(error.find("Stripe") != std::string::npos);
    // A 32-high panel at 1/32 scan: the encoder needs an even rows-per-scan.
    o = small(); o.scanRate = 32;
    CHECK_FALSE(mm::platform::hub75GpioCheckOptions(o, error));
    CHECK(error.find("32-high at 1/16") != std::string::npos);

    // The engine refuses the same geometry on open, and nothing is left running.
    mm::platform::Hub75GpioEngine engine;
    CHECK_FALSE(engine.open(o, error, true));
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("Hub75GpioEngine: a frame publishes and submits, and the geometry reads back") {
    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    std::string error;
    REQUIRE(engine.open(small(), error, true));
    CHECK(engine.isRunning());
    CHECK(std::string(engine.backendName()) == "stub (no hardware)");
    CHECK(engine.width() == 64);
    CHECK(engine.height() == 32);
    CHECK(engine.chains() == 1);
    CHECK(engine.frameSlots() == 2048);
    CHECK(engine.columnTimeNs() == 262);   // auto slowdown resolves to 1 on the stub: 249 + 13 ns a column
    CHECK(engine.lsbActualNs() == 128);   // 130 ns asks divider 32: 128 ns actual
    // Predicted refresh, computed: 2 planes x 16 rows x (64 cols x 262 ns + 0.5 us dark) = 552,576 ns.
    CHECK(std::abs(engine.refreshHz() - 1e9 / 552576.0) < 1.0);

    std::vector<uint8_t> frame(64 * 32 * 3, 0);
    engine.publishRgb(frame.data());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (engine.framesSubmitted() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(engine.framesSubmitted() > 0);
    CHECK(engine.framesEncoded() > 0);
    CHECK(engine.framesPublished() == 1);
    CHECK(engine.submitErrors() == 0);
    CHECK(engine.pulseTimeouts() == 0);
    CHECK(engine.lastSubmitUs() > 0);   // the stub slept the wire time
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("Hub75GpioEngine: the PWM clocking quantises the LSB to a multiple of 4 ns") {
    const mm::platform::GpioPwmTiming fast = mm::platform::hub75GpioPwmTiming(50);
    CHECK(fast.divider == 12);
    CHECK(fast.tickNs == 24);
    CHECK(fast.lsbActualNs == 48);

    const mm::platform::GpioPwmTiming def = mm::platform::hub75GpioPwmTiming(130);
    CHECK(def.divider == 32);
    CHECK(def.tickNs == 64);
    CHECK(def.lsbActualNs == 128);

    const mm::platform::GpioPwmTiming slow = mm::platform::hub75GpioPwmTiming(3000);
    CHECK(slow.divider == 750);
    CHECK(slow.tickNs == 1500);
    CHECK(slow.lsbActualNs == 3000);

    // Truncation, not rounding: 51 ns takes divider 12, like 50 ns.
    CHECK(mm::platform::hub75GpioPwmTiming(51).divider == 12);
    // The divider clamps to the 12-bit field.
    CHECK(mm::platform::hub75GpioPwmTiming(3000).divider == 750);
    CHECK(mm::platform::hub75GpioPulseTicks(0) == 2);
    CHECK(mm::platform::hub75GpioPulseTicks(5) == 64);
    CHECK(mm::platform::hub75GpioPulseTicks(7) == 256);   // the top plane at 8-bit
}

TEST_CASE("Hub75GpioEngine: the pulse words split at 16 ticks and carry two zero sentinels") {
    // Plane 0: 2 ticks, one word holding the full range.
    const mm::platform::GpioPulseWords p0 = mm::platform::hub75GpioPulseWords(0);
    CHECK(p0.range == 2);
    CHECK(p0.count == 3);
    CHECK(p0.words[0] == 2);
    CHECK(p0.words[1] == 0);
    CHECK(p0.words[2] == 0);

    // Plane 2: 8 ticks, still one word.
    const mm::platform::GpioPulseWords p2 = mm::platform::hub75GpioPulseWords(2);
    CHECK(p2.range == 8);
    CHECK(p2.count == 3);

    // Plane 3: 16 ticks, the split boundary -- eight words of 2.
    const mm::platform::GpioPulseWords p3 = mm::platform::hub75GpioPulseWords(3);
    CHECK(p3.range == 2);
    CHECK(p3.count == 10);
    for (int i = 0; i < 8; i++) CHECK(p3.words[i] == 2);
    CHECK(p3.words[8] == 0);
    CHECK(p3.words[9] == 0);

    // Plane 5: 64 ticks -- eight words of 8.
    const mm::platform::GpioPulseWords p5 = mm::platform::hub75GpioPulseWords(5);
    CHECK(p5.range == 8);
    CHECK(p5.count == 10);
    uint16_t total = 0;
    for (int i = 0; i < 8; i++) total = static_cast<uint16_t>(total + p5.words[i]);
    CHECK(total == 64);

    // Plane 7: 256 ticks, the top plane at 8-bit -- eight words of 32, then the two sentinels.
    const mm::platform::GpioPulseWords p7 = mm::platform::hub75GpioPulseWords(7);
    CHECK(p7.range == 32);
    CHECK(p7.count == 10);
    for (int i = 0; i < 8; i++) CHECK(p7.words[i] == 32);
    total = 0;
    for (int i = 0; i < 8; i++) total = static_cast<uint16_t>(total + p7.words[i]);
    CHECK(total == 256);
    CHECK(p7.words[8] == 0);
    CHECK(p7.words[9] == 0);
}

TEST_CASE("Hub75GpioEngine: the column-time model is bench-calibrated at 249 ns plus 13 ns a slowdown unit") {
    // BENCH-V6: ~15 ns of posted writes and loop overhead, plus two
    // delay loops a column at ~107 ns for the first GPLEV0 read and
    // ~6.4 ns a further read, plus the amortised refresh-thread
    // encode (~20 ns a column at the sign's geometry) -- see
    // hub75GpioColumnTimeNs for the calibration. Slowdown 0 runs no
    // delay loop, so no first-read penalty is paid: the writes alone.
    CHECK(mm::platform::hub75GpioColumnTimeNs(0) == 15);
    CHECK(mm::platform::hub75GpioColumnTimeNs(1) == 262);
    CHECK(mm::platform::hub75GpioColumnTimeNs(2) == 275);
    CHECK(mm::platform::hub75GpioColumnTimeNs(3) == 288);
    CHECK(mm::platform::hub75GpioColumnTimeNs(4) == 301);
    CHECK(mm::platform::hub75GpioColumnTimeNs(-1) == 15);   // clamped
    CHECK(mm::platform::hub75GpioColumnTimeNs(9) == 301);   // clamped
    CHECK(mm::platform::hub75GpioAutoSlowdown(mm::platform::Hub75GpioModel::Pi4) == 2);
    CHECK(mm::platform::hub75GpioAutoSlowdown(mm::platform::Hub75GpioModel::Pi3) == 1);
    CHECK(mm::platform::hub75GpioAutoSlowdown(mm::platform::Hub75GpioModel::Unknown) == 1);
    // The one home for auto slowdown: the resolver the backends and the output's rate prediction share.
    CHECK(mm::platform::hub75GpioResolveSlowdown(-1, mm::platform::Hub75GpioModel::Pi4) == 2);
    CHECK(mm::platform::hub75GpioResolveSlowdown(-1, mm::platform::Hub75GpioModel::Pi3) == 1);
    CHECK(mm::platform::hub75GpioResolveSlowdown(-1, mm::platform::Hub75GpioModel::Unknown) == 1);
    CHECK(mm::platform::hub75GpioResolveSlowdown(3, mm::platform::Hub75GpioModel::Pi4) == 3);
    CHECK(mm::platform::hub75GpioResolveSlowdown(0, mm::platform::Hub75GpioModel::Pi4) == 0);
}

TEST_CASE("Hub75GpioEngine: the model detection reads the device tree first and the revision second") {
    using mm::platform::Hub75GpioModel;
    CHECK(mm::platform::hub75GpioDetectModel("Raspberry Pi 4 Model B Rev 1.2", nullptr) == Hub75GpioModel::Pi4);
    CHECK(mm::platform::hub75GpioDetectModel("Raspberry Pi 3 Model B Plus Rev 1.3", nullptr) == Hub75GpioModel::Pi3);
    CHECK(mm::platform::hub75GpioDetectModel("Raspberry Pi 5 Model B Rev 1.0", nullptr) == Hub75GpioModel::Pi5);
    CHECK(mm::platform::hub75GpioDetectModel("Raspberry Pi Compute Module 4 Rev 1.0", nullptr) == Hub75GpioModel::Pi4);
    CHECK(mm::platform::hub75GpioDetectModel("Raspberry Pi Compute Module 3+ Rev 1.0", nullptr) == Hub75GpioModel::Pi3);
    CHECK(mm::platform::hub75GpioDetectModel(nullptr, "a02082") == Hub75GpioModel::Pi3);   // 3B
    CHECK(mm::platform::hub75GpioDetectModel(nullptr, "a020d3") == Hub75GpioModel::Pi3);   // 3B+
    CHECK(mm::platform::hub75GpioDetectModel(nullptr, "a03140") == Hub75GpioModel::Pi4);   // CM4
    CHECK(mm::platform::hub75GpioDetectModel(nullptr, "c03130") == Hub75GpioModel::Pi4);   // 400
    CHECK(mm::platform::hub75GpioDetectModel(nullptr, "c04170") == Hub75GpioModel::Pi5);   // 5B
    CHECK(mm::platform::hub75GpioDetectModel(nullptr, "") == Hub75GpioModel::Unknown);
    CHECK(mm::platform::hub75GpioDetectModel(nullptr, nullptr) == Hub75GpioModel::Unknown);
    CHECK(mm::platform::hub75GpioDetectModel("something else entirely", nullptr) == Hub75GpioModel::Unknown);

    CHECK(mm::platform::hub75GpioPeripheralBase(Hub75GpioModel::Pi3) == 0x3F000000u);
    CHECK(mm::platform::hub75GpioPeripheralBase(Hub75GpioModel::Pi4) == 0xFE000000u);
    CHECK(mm::platform::hub75GpioPeripheralBase(Hub75GpioModel::Pi5) == 0u);
    CHECK(mm::platform::hub75GpioPeripheralBase(Hub75GpioModel::Unknown) == 0u);
}

TEST_CASE("Hub75GpioEngine: the predicted frame time is shift-bound at 130 ns and OE-bound at 3000 ns") {
    // The wall, 6-bit, 130 ns LSB, 262 ns columns: every plane is
    // shift-bound -- 6 planes x 32 rows x (256 cols x 262 ns + 0.5 us) = 12,973,824 ns, ~77 Hz.
    CHECK(mm::platform::hub75GpioPredictedFrameNs(32, 256, 6, 130, 262) == 12973824ull);
    // The wall at 3000 ns LSB: planes 0-4 shift-bound, plane 5
    // OE-bound (96 us) -- 32 x (5 x 67,572 + 96,000) = 13,883,520 ns, ~72 Hz.
    CHECK(mm::platform::hub75GpioPredictedFrameNs(32, 256, 6, 3000, 262) == 13883520ull);
    // One 64x64 panel, 6-bit, 130 ns: 6 x 32 x (64 x 262 + 500) = 3,315,456 ns, ~302 Hz.
    CHECK(mm::platform::hub75GpioPredictedFrameNs(32, 64, 6, 130, 262) == 3315456ull);
    // Stripe on the small panel: 2 planes x 8 rows x (128 cols x 262 ns + 0.5 us dark) = 544,576 ns -- the same
    // pixel clocks as direct, half the dark intervals.
    CHECK(mm::platform::hub75GpioPredictedFrameNs(8, 128, 2, 130, 262) == 544576ull);
    // The sign, 6-bit, 160 ns LSB, slowdown 3 (288 ns columns): every plane shift-bound --
    // 6 x 8 x (128 x 288 + 500) = 1,793,472 ns, ~558 Hz predicted -- the reconciliation test below
    // checks it against the measured 555-560 fps.
    CHECK(mm::platform::hub75GpioPredictedFrameNs(8, 128, 6, 160, 288) == 1793472ull);
    // The sign at 3000 ns LSB: planes 0-3 shift-bound, planes 4-5 OE-bound (48 us, 96 us) --
    // 8 x (4 x 37,364 + 48,000 + 96,000) = 2,347,648 ns.
    CHECK(mm::platform::hub75GpioPredictedFrameNs(8, 128, 6, 3000, 288) == 2347648ull);
    // The sign, 8-bit, 160 ns LSB: every plane shift-bound at every
    // slowdown -- the top plane's OE (160 ns x 128 = 20.48 us) stays
    // under one group's shift (128 cols x 262 ns + 0.5 us = 34.04 us
    // even at slowdown 1) -- so the frame is 8/6 the 6-bit one:
    // 8 x 8 x (128 x 262 + 500) = 2,178,304 ns at slowdown 1,
    // 2,284,800 at 2, 2,391,296 at 3 -- ~459 / 438 / 418 Hz.
    CHECK(mm::platform::hub75GpioPredictedFrameNs(8, 128, 8, 160, 262) == 2178304ull);
    CHECK(mm::platform::hub75GpioPredictedFrameNs(8, 128, 8, 160, 275) == 2284800ull);
    CHECK(mm::platform::hub75GpioPredictedFrameNs(8, 128, 8, 160, 288) == 2391296ull);
    // The sign at 8-bit and 3000 ns LSB: planes 0-3 shift-bound,
    // planes 4-7 OE-bound (48, 96, 192, 384 us) -- the per-plane max
    // is what keeps the prediction sane when the top plane outlasts
    // the shift, and the dump's pulse wait paces the frame the same
    // way on the wire: 8 x (4 x 37,364 + 48,000 + 96,000 + 192,000 +
    // 384,000) = 6,955,648 ns, ~144 Hz.
    CHECK(mm::platform::hub75GpioPredictedFrameNs(8, 128, 8, 3000, 288) == 6955648ull);
}

TEST_CASE("Hub75GpioEngine: the sign's predicted rate matches the bench's shown rate at true slowdowns 1, 2 and 3 within 3%") {
    // BENCH-V6 reconciled, in TRUE slowdowns. The card stores the
    // slowdown index (0=auto, 1="0", ..., 5="4") and the v5
    // calibration read the index as the slowdown: its 595-596 fps
    // "at slowdown 3" was true slowdown 2, and its 82 ns a slowdown
    // unit is falsified. The v6 triples, three readings each, five
    // seconds after each change (means in fps):
    //
    //   slowdown 1: 608, 611, 616 -- mean 611.67
    //   slowdown 2: 584, 585, 585 -- mean 584.67 (and 584 right after install)
    //   slowdown 3: 555, 557, 560 -- mean 557.33
    //
    // The calibrated column time (262/275/288 ns) predicts
    // 1,633,728 / 1,713,600 / 1,793,472 ns a frame -- 612.10 /
    // 583.57 / 557.58 Hz, within 0.2% of all three means -- and the
    // engine's own refreshHz agrees on the sign's options (slowdown
    // 2, as found). Tolerance +-3%: same-shape bench runs differed
    // 3% (v3 1,067 fps, v4 1,099 fps) and the within-triple spread
    // is ~0.7% -- see hub75GpioColumnTimeNs.
    struct Reading {
        int slowdown;
        uint64_t frameNs;
        double measuredHz;
    };
    static const Reading kReadings[] = {
        {1, 1633728ull, 611.67},
        {2, 1713600ull, 584.67},
        {3, 1793472ull, 557.33},
    };
    for (const Reading& r : kReadings) {
        const uint64_t frameNs = mm::platform::hub75GpioPredictedFrameNs(
            8, 128, 6, 160, mm::platform::hub75GpioColumnTimeNs(r.slowdown));
        CHECK(frameNs == r.frameNs);
        const double predictedHz = 1e9 / static_cast<double>(frameNs);
        CHECK(std::abs(predictedHz - r.measuredHz) / r.measuredHz < 0.03);
    }

    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    std::string error;
    REQUIRE(engine.open(signOptions(), error, true));
    CHECK(engine.columnTimeNs() == 275);
    CHECK(std::abs(engine.refreshHz() - 584.67) / 584.67 < 0.03);
    engine.shutdown();
}

TEST_CASE("Hub75GpioEngine: the sign's predicted rate at 8-bit is 8/6 the 6-bit frame, by true slowdown") {
    // No 8-bit bench exists yet: these are hand-computed from the
    // re-derived model (hub75GpioColumnTimeNs), pinned within the
    // +-3% the model already carries, so the lab's `shown` has a
    // number to meet -- 459 / 438 / 418 Hz at true slowdowns 1 / 2 /
    // 3, against 612 / 584 / 558 at 6-bit. The sign is shift-bound
    // at every plane and every slowdown (see the frame-time test),
    // so the 8-bit frame is exactly 8/6 the 6-bit one.
    struct Reading {
        int slowdown;
        uint64_t frameNs;
        double handHz;
    };
    static const Reading kReadings[] = {
        {1, 2178304ull, 459.07},
        {2, 2284800ull, 437.68},
        {3, 2391296ull, 418.18},
    };
    for (const Reading& r : kReadings) {
        const uint64_t frameNs = mm::platform::hub75GpioPredictedFrameNs(
            8, 128, 8, 160, mm::platform::hub75GpioColumnTimeNs(r.slowdown));
        CHECK(frameNs == r.frameNs);
        const double predictedHz = 1e9 / static_cast<double>(frameNs);
        CHECK(std::abs(predictedHz - r.handHz) / r.handHz < 0.03);
    }

    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    std::string error;
    mm::platform::Hub75GpioOptions o = signOptions();
    o.bitDepth = 8;
    REQUIRE(engine.open(o, error, true));
    CHECK(engine.columnTimeNs() == 275);
    CHECK(std::abs(engine.refreshHz() - 437.68) / 437.68 < 0.03);
    engine.shutdown();
}

TEST_CASE("Hub75GpioEngine: a red pixel at (0,0) lights the top plane's first word, and no control bit leaks into the stream") {
    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    std::string error;
    REQUIRE(engine.open(small(), error, true));

    std::vector<uint8_t> frame(64 * 32 * 3, 0);
    frame[0] = 128;   // pixel (0,0), red at half: plane 0 dark, plane 1 lit (bit 7 set, bit 6 clear)
    engine.publishRgb(frame.data());
    // Plane 1, row 0, column 0: slot 1024. R1 of chain 0 is GPIO 11.
    REQUIRE(waitForWord(1024, 11, 0));
    std::vector<uint32_t> last(2048);
    REQUIRE(mm::platform::gpioStubCopyLastFrame(last.data(), last.size()) == 2048);
    CHECK(last[1024] == 0x800u);   // only GPIO 11: the colour bit, no address, no latch, no OE
    CHECK(last[0] == 0u);          // plane 0: the half-red pixel's low bit is clear
    CHECK(last[63] == 0u);         // row 0's last word: no colour, and the latch rides the CPU, not the stream
    engine.shutdown();
}

TEST_CASE("Hub75GpioEngine: the multiplex table holds normal first and Stripe second, and an unknown type is direct") {
    using mm::platform::hub75GpioMultiplexCount;
    using mm::platform::hub75GpioMultiplexEntry;
    CHECK(hub75GpioMultiplexCount() == 2);
    // Normal first, Stripe second -- the control's order, and the
    // table's: Stripe is one mapping among hzeller's 24, not the default.
    CHECK(std::string(hub75GpioMultiplexEntry(0).name) == "normal");
    CHECK(std::string(hub75GpioMultiplexEntry(1).name) == "Stripe");
    CHECK(hub75GpioMultiplexEntry(0).stretch == 1);
    CHECK(hub75GpioMultiplexEntry(1).stretch == 2);
    CHECK(hub75GpioMultiplexEntry(0).scanDivisor == 2);
    CHECK(hub75GpioMultiplexEntry(1).scanDivisor == 4);
    // An unknown type falls back to the first entry, normal.
    CHECK(std::string(hub75GpioMultiplexEntry(2).name) == "normal");
    CHECK(hub75GpioMultiplexEntry(2).stretch == 1);
    CHECK(hub75GpioMultiplexEntry(2).scanDivisor == 2);
    // Normal is the identity, through the table.
    const auto p = mm::platform::hub75GpioMultiplexMap(0, 5, 7, 32, 64);
    CHECK(p.x == 5);
    CHECK(p.y == 7);
    const auto q = mm::platform::hub75GpioMultiplexMap(0, 63, 31, 32, 64);
    CHECK(q.x == 63);
    CHECK(q.y == 31);
}

TEST_CASE("Hub75GpioEngine: the Stripe map folds a 32x64 panel into 16 driven rows x 128 columns") {
    using mm::platform::hub75GpioMultiplexMap;
    // The Stripe entry, through the table: every row of a 32x64 panel --
    // driven row (y/16)*8 + y%8, columns +64 where (y%16) < 8 -- both
    // halves of each 16-row group, at both ends of the row.
    for (uint8_t y = 0; y < 32; y++) {
        const auto p0 = hub75GpioMultiplexMap(1, 0, y, 32, 64);
        const auto p63 = hub75GpioMultiplexMap(1, 63, y, 32, 64);
        const uint8_t expectRow = static_cast<uint8_t>((y / 16) * 8 + y % 8);
        const bool fold = (y % 16) < 8;
        CHECK(p0.y == expectRow);
        CHECK(p63.y == expectRow);
        CHECK(p0.x == (fold ? 64 : 0));
        CHECK(p63.x == (fold ? 127 : 63));
    }
    // The fold boundary: (y%16) == 7 folds, == 8 does not, in both groups.
    CHECK(hub75GpioMultiplexMap(1, 0, 7, 32, 64).x == 64);
    CHECK(hub75GpioMultiplexMap(1, 0, 8, 32, 64).x == 0);
    CHECK(hub75GpioMultiplexMap(1, 0, 23, 32, 64).x == 64);
    CHECK(hub75GpioMultiplexMap(1, 0, 24, 32, 64).x == 0);
    // The address at the group boundaries: y=15 and y=16, and y=31.
    CHECK(hub75GpioMultiplexMap(1, 0, 15, 32, 64).y == 7);
    CHECK(hub75GpioMultiplexMap(1, 0, 16, 32, 64).y == 8);
    CHECK(hub75GpioMultiplexMap(1, 0, 31, 32, 64).y == 15);
    // Two logical rows share each driven row, on opposite halves.
    CHECK(hub75GpioMultiplexMap(1, 0, 0, 32, 64).y == hub75GpioMultiplexMap(1, 0, 8, 32, 64).y);
    CHECK(hub75GpioMultiplexMap(1, 0, 0, 32, 64).x == 64);
    CHECK(hub75GpioMultiplexMap(1, 0, 8, 32, 64).x == 0);
    CHECK(hub75GpioMultiplexMap(1, 0, 16, 32, 64).y == hub75GpioMultiplexMap(1, 0, 24, 32, 64).y);
    // A 64-row panel folds the same way, in 32-row groups.
    CHECK(hub75GpioMultiplexMap(1, 0, 0, 64, 64).y == 0);
    CHECK(hub75GpioMultiplexMap(1, 0, 15, 64, 64).x == 64);
    CHECK(hub75GpioMultiplexMap(1, 0, 15, 64, 64).y == 15);
    CHECK(hub75GpioMultiplexMap(1, 0, 16, 64, 64).x == 0);
    CHECK(hub75GpioMultiplexMap(1, 0, 16, 64, 64).y == 0);
    CHECK(hub75GpioMultiplexMap(1, 0, 31, 64, 64).y == 15);
    CHECK(hub75GpioMultiplexMap(1, 0, 32, 64, 64).y == 16);
    CHECK(hub75GpioMultiplexMap(1, 0, 63, 64, 64).y == 31);
    CHECK(hub75GpioMultiplexMap(1, 0, 63, 64, 64).x == 0);
    // A panel too short to fold maps to itself.
    CHECK(hub75GpioMultiplexMap(1, 5, 1, 2, 64).x == 5);
    CHECK(hub75GpioMultiplexMap(1, 5, 1, 2, 64).y == 1);
    // The entry's own function agrees with the table: the table does
    // not reshape the mapping, it only selects it.
    for (uint8_t y = 0; y < 32; y++) {
        const auto viaTable = hub75GpioMultiplexMap(1, 0, y, 32, 64);
        const auto direct = mm::platform::hub75GpioStripeMap(0, y, 32, 64);
        CHECK(viaTable.x == direct.x);
        CHECK(viaTable.y == direct.y);
    }
}

TEST_CASE("Hub75GpioEngine: Stripe halves the scan and the rows and doubles the columns; normal is unchanged") {
    using mm::platform::hub75GpioMultiplexScanRate;
    using mm::platform::hub75GpioMultiplexDrivenRows;
    using mm::platform::hub75GpioMultiplexDrivenCols;
    CHECK(hub75GpioMultiplexScanRate(0, 32) == 16);
    CHECK(hub75GpioMultiplexScanRate(0, 64) == 32);
    CHECK(hub75GpioMultiplexScanRate(1, 32) == 8);   // the sign: 1/8 scan of the driven 16 rows
    CHECK(hub75GpioMultiplexScanRate(1, 64) == 16);
    CHECK(hub75GpioMultiplexDrivenRows(0, 32) == 32);
    CHECK(hub75GpioMultiplexDrivenRows(1, 32) == 16);
    CHECK(hub75GpioMultiplexDrivenRows(1, 64) == 32);
    CHECK(hub75GpioMultiplexDrivenCols(0, 64) == 64);
    CHECK(hub75GpioMultiplexDrivenCols(1, 64) == 128);
    CHECK(hub75GpioMultiplexDrivenCols(1, 128) == 256);
    // An unknown type is direct.
    CHECK(hub75GpioMultiplexScanRate(2, 32) == 16);
    CHECK(hub75GpioMultiplexDrivenRows(2, 32) == 32);
    CHECK(hub75GpioMultiplexDrivenCols(2, 64) == 64);
}

TEST_CASE("Hub75GpioEngine: Stripe folds four logical rows onto one address step, and normal leaves each on its own") {
    // One address step, four logical rows: rows 1, 9, 17 and 25 all ride
    // step 1 -- rows 1 and 17 folded to column 64 (R1 and R2), rows 9
    // and 25 at column 0 (R1 and R2).
    std::vector<uint8_t> frame(64 * 32 * 3, 0);
    frame[(1 * 64 + 0) * 3] = 128;    // (0,1): driven (64,1), R1, step 1
    frame[(9 * 64 + 0) * 3] = 128;    // (0,9): driven (0,1), R1, step 1
    frame[(17 * 64 + 0) * 3] = 128;   // (0,17): driven (64,9), R2, step 1
    frame[(25 * 64 + 0) * 3] = 128;   // (0,25): driven (0,9), R2, step 1
    std::vector<uint32_t> last(2048);

    mm::platform::Hub75GpioOptions o = small();
    o.multiplexing = 1;
    o.scanRate = 8;   // Stripe drives the 16 driven rows at 1/8
    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    std::string error;
    REQUIRE(engine.open(o, error, true));
    // The canvas stays the logical 64x32; the driven frame is 128 wide
    // and 16 high, the same 2,048 slots a frame.
    CHECK(engine.width() == 64);
    CHECK(engine.height() == 32);
    CHECK(engine.drivenWidth() == 128);
    CHECK(engine.drivenHeight() == 16);
    CHECK(engine.frameSlots() == 2048);
    // Predicted refresh, computed: 2 planes x 8 rows x (128 cols x 262 ns + 0.5 us dark) = 544,576 ns.
    CHECK(std::abs(engine.refreshHz() - 1e9 / 544576.0) < 1.0);
    engine.publishRgb(frame.data());
    // Plane 1, step 1: slots 1024 + 128 + x. R1 of chain 0 is GPIO 11, R2 GPIO 8.
    REQUIRE(waitForWord(1024 + 128 + 64, 11, 0));
    REQUIRE(mm::platform::gpioStubCopyLastFrame(last.data(), last.size()) == 2048);
    CHECK(last[1024 + 128 + 64] == 0x900u);   // rows 1 and 17 share the word: R1 + R2, column 64
    CHECK(last[1024 + 128 + 0] == 0x900u);    // rows 9 and 25 share the word: R1 + R2, column 0
    CHECK(last[1024 + 4 * 128] == 0u);        // step 4: nothing -- the old stripes-of-4 shape put row 1 there
    CHECK(last[128 + 64] == 0u);              // plane 0: the half-red pixels' low bit is clear
    CHECK(last[128 + 0] == 0u);
    engine.shutdown();

    // Normal: each row stays on its own step -- rows 1 and 17 share step
    // 1 (R1 and R2), rows 9 and 25 share step 9, all at column 0.
    o.multiplexing = 0;
    o.scanRate = 16;
    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine plain;
    REQUIRE(plain.open(o, error, true));
    CHECK(plain.drivenWidth() == 64);
    CHECK(plain.drivenHeight() == 32);
    plain.publishRgb(frame.data());
    REQUIRE(waitForWord(1024 + 1 * 64, 11, 0));
    REQUIRE(mm::platform::gpioStubCopyLastFrame(last.data(), last.size()) == 2048);
    CHECK(last[1024 + 1 * 64] == 0x900u);
    CHECK(last[1024 + 9 * 64] == 0x900u);
    CHECK(last[1024 + 128 + 64] == 0u);   // nothing at the folded word: step 3, column 0 is dark
    plain.shutdown();
}

TEST_CASE("Hub75GpioEngine: the sign's Stripe geometry -- 3 parallel chains, 64x96 logical, 128 driven columns") {
    mm::platform::Hub75GpioOptions o;
    o.rows = 32;
    o.cols = 64;
    o.chain = 1;
    o.parallel = 3;
    o.scanRate = 8;
    o.multiplexing = 1;
    o.bitDepth = 6;
    o.pwmLsbNs = 160;
    o.slowdown = 3;
    std::string error;
    CHECK(mm::platform::hub75GpioCheckOptions(o, error));
    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    REQUIRE(engine.open(o, error, true));
    CHECK(engine.width() == 64);
    CHECK(engine.height() == 96);
    CHECK(engine.chains() == 3);
    CHECK(engine.drivenWidth() == 128);
    CHECK(engine.drivenHeight() == 16);
    CHECK(engine.frameSlots() == 6144);   // 6 planes x 8 rows x 128 columns
    CHECK(engine.columnTimeNs() == 288);   // slowdown 3: 249 + 3*13 ns a column
    // Predicted refresh, computed: 6 planes x 8 rows x (128 cols x 288 ns + 0.5 us dark) = 1,793,472 ns, ~558 Hz.
    CHECK(std::abs(engine.refreshHz() - 1e9 / 1793472.0) < 1.0);
    std::vector<uint8_t> frame(64 * 96 * 3, 0);
    engine.publishRgb(frame.data());
    // Wait for the encode, not just a submit: the refresh thread
    // re-submits the initial frame while the published one still waits,
    // so a submit alone does not prove the published frame was encoded.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (engine.framesEncoded() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(engine.framesEncoded() > 0);
    while (engine.framesSubmitted() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(engine.framesSubmitted() > 0);
    CHECK(engine.submitErrors() == 0);
    engine.shutdown();
}

TEST_CASE("Hub75GpioEngine: the repack tables match the reference, and control bits never reach the word") {
    const mm::platform::GpioRepackTables tables = mm::platform::buildGpioRepackTables();
    // Every colour combination on each chain, the other chains dark.
    for (int chain = 0; chain < 3; chain++) {
        for (uint16_t v = 0; v < 64; v++) {
            uint16_t slot[3] = {0, 0, 0};
            slot[chain] = v;
            const uint16_t* ptrs[3] = {&slot[0], &slot[1], &slot[2]};
            uint32_t gpio = 0;
            mm::platform::repackSlotsToGpio(ptrs, 3, 1, tables, &gpio);
            CHECK(gpio == mm::platform::packGpioWordReference(slot[0], slot[1], slot[2], 3));
        }
    }
    // Every control combination on chain 0's word: the address, latch
    // and OE bits change nothing -- the dump loop and the pulser own
    // those lines, so the repack must strip them.
    for (uint16_t v = 0; v < 128; v++) {
        uint16_t slot[3] = {static_cast<uint16_t>((v << 8) | 0x3F), 0, 0};
        const uint16_t* ptrs[3] = {&slot[0], &slot[1], &slot[2]};
        uint32_t gpio = 0;
        mm::platform::repackSlotsToGpio(ptrs, 3, 1, tables, &gpio);
        CHECK(gpio == mm::platform::packGpioWordReference(0x003F, 0, 0, 3));
    }
    // One chain only: the other chains' colour never reaches the word.
    {
        uint16_t slot[3] = {0x003F, 0x003F, 0x003F};
        const uint16_t* ptrs[3] = {&slot[0], &slot[1], &slot[2]};
        uint32_t gpio = 0;
        mm::platform::repackSlotsToGpio(ptrs, 1, 1, tables, &gpio);
        CHECK(gpio == mm::platform::packGpioWordReference(slot[0], slot[1], slot[2], 1));
        CHECK(gpio == mm::platform::packGpioWordReference(0x003F, 0, 0, 3));
    }
}

TEST_CASE("Hub75GpioEngine: the setup runs dark values, then directions, then the PWM, then the mux") {
    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    std::string error;
    REQUIRE(engine.open(small(), error, true));
    // NOTE.md rule 1, pinned: values, then directions, then the mux --
    // the hardware backend runs the same four steps in the same order.
    CHECK(mm::platform::gpioStubSetupLog() == "dark-values,directions,pwm-config,oe-mux");
    engine.shutdown();
}

TEST_CASE("Hub75GpioEngine: a second open is refused, and shutdown is idempotent and leaves a blank frame last") {
    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    std::string error;
    REQUIRE(engine.open(small(), error, true));
    CHECK_FALSE(engine.open(small(), error, true));
    CHECK(error == "GPIO engine already running");

    std::vector<uint8_t> frame(64 * 32 * 3, 255);   // all white, so the blank after it is unmistakable
    engine.publishRgb(frame.data());
    REQUIRE(waitForWord(1024, 11, 0));
    engine.shutdown();
    engine.shutdown();   // a second shutdown is harmless
    CHECK_FALSE(engine.isRunning());
    std::vector<uint32_t> last(2048);
    REQUIRE(mm::platform::gpioStubCopyLastFrame(last.data(), last.size()) == 2048);
    for (uint32_t w : last) CHECK(w == 0u);   // every colour bit clear

    // The engine opens again after a shutdown, and the second open runs
    // the full dark-first setup again (NOTE.md rule 3: release and
    // re-acquire without restarting the process).
    REQUIRE(engine.open(small(), error, true));
    CHECK(mm::platform::gpioStubSetupLog() ==
          "dark-values,directions,pwm-config,oe-mux,dark-values,directions,pwm-config,oe-mux");
    engine.shutdown();
}

TEST_CASE("Hub75GpioEngine: the sign's held steps encode one row / one column exactly") {
    // BENCH-V4's lab steps, pinned as words: the probe that cleared the
    // encode path -- one canvas row lit, one canvas column lit, and the
    // even-fill control -- so a future encode, Stripe or repack change
    // that smears a held step fails here instead of on the sign.
    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    std::string error;
    REQUIRE(engine.open(signOptions(), error, true));
    REQUIRE(engine.frameSlots() == 6144);

    const uint32_t red = redMaskAllChains();
    const uint32_t greenCh0 = 1u << mm::platform::kGpioChainPins[0].g1;
    std::vector<uint32_t> last(6144);

    // Frame A: canvas row 5 (chain 0, logical row 5), all 64 cols, green 255.
    // Under Stripe the row lands in step 5 of every plane, driven columns
    // 64..127, on chain 0's G1 line only: 6 groups x 64 words.
    {
        std::vector<uint8_t> frame(64 * 96 * 3, 0);
        for (int x = 0; x < 64; x++) frame[(5 * 64 + x) * 3 + 1] = 255;
        const uint64_t e0 = engine.framesEncoded(), s0 = engine.framesSubmitted();
        engine.publishRgb(frame.data());
        REQUIRE(waitFresh(engine, e0, s0));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        REQUIRE(mm::platform::gpioStubCopyLastFrame(last.data(), last.size()) == 6144);
        size_t nz = 0;
        for (int g = 0; g < 48; g++) {
            for (int i = 0; i < 128; i++) {
                const uint32_t w = last[g * 128 + i];
                if (w != 0) nz++;
                if (g % 8 == 5 && i >= 64) CHECK(w == greenCh0);
                else CHECK(w == 0u);
            }
        }
        CHECK(nz == 384);
    }

    // Frame B: canvas column 7, all 96 rows, red 255. Every group's
    // clocks 7 and 71 carry red on all chains; every other clock is dark.
    {
        std::vector<uint8_t> frame(64 * 96 * 3, 0);
        for (int y = 0; y < 96; y++) frame[(y * 64 + 7) * 3 + 0] = 255;
        const uint64_t e0 = engine.framesEncoded(), s0 = engine.framesSubmitted();
        engine.publishRgb(frame.data());
        REQUIRE(waitFresh(engine, e0, s0));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        REQUIRE(mm::platform::gpioStubCopyLastFrame(last.data(), last.size()) == 6144);
        size_t nz = 0;
        for (int g = 0; g < 48; g++) {
            for (int i = 0; i < 128; i++) {
                const uint32_t w = last[g * 128 + i];
                if (w != 0) nz++;
                if (i == 7 || i == 71) CHECK(w == red);
                else CHECK(w == 0u);
            }
        }
        CHECK(nz == 96);
    }

    // Frame C: full red fill, the even-fill control -- every word red.
    {
        std::vector<uint8_t> frame(64 * 96 * 3, 0);
        for (size_t i = 0; i < frame.size(); i += 3) frame[i] = 255;
        const uint64_t e0 = engine.framesEncoded(), s0 = engine.framesSubmitted();
        engine.publishRgb(frame.data());
        REQUIRE(waitFresh(engine, e0, s0));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        REQUIRE(mm::platform::gpioStubCopyLastFrame(last.data(), last.size()) == 6144);
        for (uint32_t w : last) CHECK(w == red);
    }
    engine.shutdown();
}

TEST_CASE("Hub75GpioEngine: the dump walks rows outer, planes inner, and latches in the dark") {
    // BENCH-V4's fix, pinned: the dump sequence the hardware backend
    // drives, read back from the stub's recorder (the same shared loop,
    // hub75_gpio_dump.h -- not a copy). The sign's column frame (one
    // canvas column red) makes the per-clock data check crisp: clocks 7
    // and 71 carry red, every other clock is dark.
    //
    // What this proves: the loop's write order and grouping -- the
    // address changes only between rows, the data is re-presented per
    // clock, the latch follows the row's clocks, the OE wait (which
    // blanks the panel) precedes the address change and the pulse fire
    // follows the strobe. What only the sign can confirm: that this
    // sequence cures the spreading -- the VM has no panel, no slow-decay
    // rows, and no OE timing to observe.
    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    std::string error;
    REQUIRE(engine.open(signOptions(), error, true));
    REQUIRE(engine.frameSlots() == 6144);

    std::vector<uint8_t> frame(64 * 96 * 3, 0);
    for (int y = 0; y < 96; y++) frame[(y * 64 + 7) * 3 + 0] = 255;
    const uint64_t e0 = engine.framesEncoded(), s0 = engine.framesSubmitted();
    engine.publishRgb(frame.data());
    REQUIRE(waitFresh(engine, e0, s0));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // The stream: 48 groups x (128 clocks x 3 writes + colour clear +
    // pulse wait + strobe set/clear + pulse fire) + 8 address writes
    // (one per row, CLR + SET) = 18,688 events, exactly. The count pins
    // the loop's write sequence, not just its grouping.
    std::vector<mm::platform::GpioDumpEvent> stream(20000);
    REQUIRE(mm::platform::gpioStubCopyLastDumpStream(stream.data(), stream.size()) == 18688);
    stream.resize(18688);
    std::vector<uint32_t> words(6144);
    REQUIRE(mm::platform::gpioStubCopyLastFrame(words.data(), words.size()) == 6144);

    const std::vector<DecodedGroup> groups = decodeDumpStream(stream, false);
    REQUIRE(groups.size() == 48);   // 8 rows x 6 planes

    const uint32_t red = redMaskAllChains();
    const uint32_t colour = mm::platform::kGpioColourMask;
    for (int k = 0; k < 48; k++) {
        const int row = k / 6, plane = k % 6;
        const DecodedGroup& g = groups[k];
        // Row-major: the planes cycle within each row, and the strobe's
        // address is the row -- each address held for 6 groups.
        CHECK(g.plane == plane);
        CHECK(g.address == row);
        // The address is written only at the row's first plane; a row's
        // later planes share it without rewriting it.
        CHECK(g.addrChanged == (plane == 0));
        // The row's full clock count, and the data re-presented per
        // clock: the clocked data is the frame's group for this
        // (row, plane) -- the frame is plane-major, so group
        // (plane * 8 + row) -- word for word.
        REQUIRE(g.clocked.size() == 128);
        const size_t base = (static_cast<size_t>(plane) * 8 + row) * 128;
        for (int i = 0; i < 128; i++)
            CHECK(g.clocked[i] == (words[base + i] & colour));
        // The column frame's crisp pattern, in the clocked data itself:
        // red at clocks 7 and 71, dark everywhere else.
        CHECK(g.clocked[7] == red);
        CHECK(g.clocked[71] == red);
        CHECK(g.clocked[0] == 0u);
        CHECK(g.clocked[127] == 0u);
        // The dark order: the pulse wait (which disables OE) precedes
        // the address change, the address change precedes the strobe,
        // and the pulse fire follows the strobe.
        CHECK(g.waitIdx < g.strobeIdx);
        CHECK(g.strobeIdx < g.fireIdx);
        if (g.addrChanged) {
            CHECK(g.waitIdx < g.addrIdx);
            CHECK(g.addrIdx < g.strobeIdx);
        }
    }
    // The address sequence, deduped: 0..7, each held for one row.
    {
        std::vector<int> addrs;
        for (const DecodedGroup& g : groups)
            if (addrs.empty() || addrs.back() != g.address) addrs.push_back(g.address);
        CHECK(addrs.size() == 8);
        for (int r = 0; r < 8; r++) CHECK(addrs[r] == r);
    }
    engine.shutdown();
}

TEST_CASE("Hub75GpioEngine: the falling-edge dump clocks on the falling edge, in the same order") {
    // The inverted branch of the shared loop (hub75_gpio_dump.h), on the
    // small geometry: the clock edge is the CLR, the data is set with
    // the clock high, and the row-major order, the dark latch and the
    // per-clock data hold exactly as in the rising case.
    mm::platform::Hub75GpioOptions o = small();
    o.clockFallingEdge = true;
    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    std::string error;
    REQUIRE(engine.open(o, error, true));
    REQUIRE(engine.frameSlots() == 2048);   // 2 planes x 16 rows x 64 columns

    // One red pixel at (0,0): the top plane's first word carries red.
    std::vector<uint8_t> frame(64 * 32 * 3, 0);
    frame[0] = 255;
    const uint64_t e0 = engine.framesEncoded(), s0 = engine.framesSubmitted();
    engine.publishRgb(frame.data());
    REQUIRE(waitFresh(engine, e0, s0));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // 32 groups x (64 clocks x 3 writes + 5) + 16 address writes = 6,336 events.
    std::vector<mm::platform::GpioDumpEvent> stream(8000);
    REQUIRE(mm::platform::gpioStubCopyLastDumpStream(stream.data(), stream.size()) == 6336);
    stream.resize(6336);
    std::vector<uint32_t> words(2048);
    REQUIRE(mm::platform::gpioStubCopyLastFrame(words.data(), words.size()) == 2048);

    const std::vector<DecodedGroup> groups = decodeDumpStream(stream, true);
    REQUIRE(groups.size() == 32);   // 16 rows x 2 planes
    const uint32_t colour = mm::platform::kGpioColourMask;
    for (int k = 0; k < 32; k++) {
        const int row = k / 2, plane = k % 2;
        const DecodedGroup& g = groups[k];
        CHECK(g.plane == plane);
        CHECK(g.address == row);
        CHECK(g.addrChanged == (plane == 0));
        REQUIRE(g.clocked.size() == 64);
        const size_t base = (static_cast<size_t>(plane) * 16 + row) * 64;
        for (int i = 0; i < 64; i++)
            CHECK(g.clocked[i] == (words[base + i] & colour));
        CHECK(g.waitIdx < g.strobeIdx);
        CHECK(g.strobeIdx < g.fireIdx);
        if (g.addrChanged) {
            CHECK(g.waitIdx < g.addrIdx);
            CHECK(g.addrIdx < g.strobeIdx);
        }
    }
    engine.shutdown();
}

TEST_CASE("Hub75GpioEngine: the dump strides by the pass and clocks only the shift words, skipping the hold") {
    // A 16-column row at 6 bits holds 17 words: the frame's groups are 33 words, but the dump clocks only the 16 shift words of each -- the hold words exist for the encoder's OE bits, and the GPIO pulser sets the lit time, so clocking them would slow the refresh for nothing. The clocked data is the frame's shift words, word for word, past the hold.
    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    std::string error;
    mm::platform::Hub75GpioOptions o;
    o.rows = 32;
    o.cols = 16;
    o.chain = 1;
    o.parallel = 1;
    o.scanRate = 16;
    o.bitDepth = 6;
    o.pwmLsbNs = 130;
    o.slowdown = -1;   // auto: the stub resolves it to 1
    REQUIRE(engine.open(o, error, true));
    REQUIRE(engine.frameSlots() == 3168);   // 6 planes x 16 rows x a 33-word pass

    std::vector<uint8_t> frame(16 * 32 * 3, 0);
    for (int y = 0; y < 32; y++) frame[(y * 16 + 7) * 3 + 0] = 255;   // one canvas column red
    const uint64_t e0 = engine.framesEncoded(), s0 = engine.framesSubmitted();
    engine.publishRgb(frame.data());
    REQUIRE(waitFresh(engine, e0, s0));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // The stream: 96 groups x (16 clocks x 3 writes + colour clear + pulse wait + strobe set/clear + pulse fire) + 16 address writes (one per row, CLR + SET) = 5,120 events, exactly. Had the dump clocked the hold words too, each group would carry 33 clocks and the count would be 10,016.
    std::vector<mm::platform::GpioDumpEvent> stream(6000);
    REQUIRE(mm::platform::gpioStubCopyLastDumpStream(stream.data(), stream.size()) == 5120);
    stream.resize(5120);
    std::vector<uint32_t> words(3168);
    REQUIRE(mm::platform::gpioStubCopyLastFrame(words.data(), words.size()) == 3168);

    const std::vector<DecodedGroup> groups = decodeDumpStream(stream, false);
    REQUIRE(groups.size() == 96);   // 16 rows x 6 planes

    const uint32_t red = (1u << mm::platform::kGpioChainPins[0].r1) | (1u << mm::platform::kGpioChainPins[0].r2);
    const uint32_t colour = mm::platform::kGpioColourMask;
    for (int k = 0; k < 96; k++) {
        const int row = k / 6, plane = k % 6;
        const DecodedGroup& g = groups[k];
        CHECK(g.plane == plane);
        CHECK(g.address == row);
        CHECK(g.addrChanged == (plane == 0));
        // The clocked data is the frame's group for this (row, plane) -- the frame is plane-major, so group (plane * 16 + row) -- past its 17 hold words, word for word.
        REQUIRE(g.clocked.size() == 16);
        const size_t base = (static_cast<size_t>(plane) * 16 + row) * 33;
        for (int i = 0; i < 16; i++)
            CHECK(g.clocked[i] == (words[base + 17 + i] & colour));
        // The column frame's crisp pattern: red at clock 7, dark everywhere else.
        CHECK(g.clocked[7] == red);
        CHECK(g.clocked[0] == 0u);
        CHECK(g.clocked[15] == 0u);
        CHECK(g.waitIdx < g.strobeIdx);
        CHECK(g.strobeIdx < g.fireIdx);
        if (g.addrChanged) {
            CHECK(g.waitIdx < g.addrIdx);
            CHECK(g.addrIdx < g.strobeIdx);
        }
    }
    engine.shutdown();
}

TEST_CASE("Hub75GpioEngine: the sign at 8-bit lights every plane, no hold, vectors exact") {
    // The sign's driven geometry is 128 columns a row group (Stripe
    // doubles 64), so rowWords is 128 -- above the 8-bit hold trigger
    // of 64 -- and the encoder holds nothing: the frame is 8 planes x
    // 8 rows x 128 shift words, 8,192 words. An all-white frame lights
    // every bit of every byte, so every plane's every word is the full
    // colour mask -- no zero plane, no floor -- and the dump clocks
    // all eight planes row-major, word for word.
    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    std::string error;
    mm::platform::Hub75GpioOptions o = signOptions();
    o.bitDepth = 8;
    REQUIRE(engine.open(o, error, true));
    REQUIRE(engine.frameSlots() == 8192);   // 8 planes x 8 rows x 128 columns, no hold words
    CHECK(std::abs(engine.refreshHz() - 1e9 / 2284800.0) < 1.0);   // slowdown 2, ~438 Hz

    std::vector<uint8_t> frame(64 * 96 * 3, 255);   // all white: every bit of every byte set
    const uint64_t e0 = engine.framesEncoded(), s0 = engine.framesSubmitted();
    engine.publishRgb(frame.data());
    REQUIRE(waitFresh(engine, e0, s0));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    std::vector<uint32_t> words(8192);
    REQUIRE(mm::platform::gpioStubCopyLastFrame(words.data(), words.size()) == 8192);
    const uint32_t colour = mm::platform::kGpioColourMask;
    for (int p = 0; p < 8; p++) {
        for (int r = 0; r < 8; r++) {
            const size_t base = (static_cast<size_t>(p) * 8 + r) * 128;
            uint32_t any = 0;
            for (int i = 0; i < 128; i++) {
                CHECK(words[base + i] == colour);   // every word lit, every plane
                any |= words[base + i];
            }
            CHECK(any != 0u);   // no zero plane
        }
    }

    // The stream: 64 groups x (128 clocks x 3 writes + colour clear + pulse wait + strobe set/clear + pulse fire) + 8 address writes (one per row, CLR + SET) = 24,912 events, exactly.
    std::vector<mm::platform::GpioDumpEvent> stream(26000);
    REQUIRE(mm::platform::gpioStubCopyLastDumpStream(stream.data(), stream.size()) == 24912);
    stream.resize(24912);

    const std::vector<DecodedGroup> groups = decodeDumpStream(stream, false);
    REQUIRE(groups.size() == 64);   // 8 rows x 8 planes
    for (int k = 0; k < 64; k++) {
        const int row = k / 8, plane = k % 8;
        const DecodedGroup& g = groups[k];
        CHECK(g.plane == plane);
        CHECK(g.address == row);
        CHECK(g.addrChanged == (plane == 0));
        REQUIRE(g.clocked.size() == 128);
        const size_t base = (static_cast<size_t>(plane) * 8 + row) * 128;
        for (int i = 0; i < 128; i++)
            CHECK(g.clocked[i] == (words[base + i] & colour));
        CHECK(g.waitIdx < g.strobeIdx);
        CHECK(g.strobeIdx < g.fireIdx);
        if (g.addrChanged) {
            CHECK(g.waitIdx < g.addrIdx);
            CHECK(g.addrIdx < g.strobeIdx);
        }
    }
    engine.shutdown();
}

TEST_CASE("Hub75GpioEngine: the dump at 8-bit strides by the pass and clocks only the shift words, past a 113-word hold") {
    // A 16-column row at 8 bits holds 113 words: the frame's groups are 129 words, but the dump clocks only the 16 shift words of each -- the hold words exist for the encoder's OE bits, and the GPIO pulser sets the lit time, so clocking them would slow the refresh for nothing. The clocked data is the frame's shift words, word for word, past the hold, on all eight planes -- and the red column lights on every plane, so no plane is dead on a narrow row either.
    mm::platform::gpioStubResetCapture();
    mm::platform::Hub75GpioEngine engine;
    std::string error;
    mm::platform::Hub75GpioOptions o;
    o.rows = 32;
    o.cols = 16;
    o.chain = 1;
    o.parallel = 1;
    o.scanRate = 16;
    o.bitDepth = 8;
    o.pwmLsbNs = 130;
    o.slowdown = -1;   // auto: the stub resolves it to 1
    REQUIRE(engine.open(o, error, true));
    REQUIRE(engine.frameSlots() == 16512);   // 8 planes x 16 rows x a 129-word pass

    std::vector<uint8_t> frame(16 * 32 * 3, 0);
    for (int y = 0; y < 32; y++) frame[(y * 16 + 7) * 3 + 0] = 255;   // one canvas column red
    const uint64_t e0 = engine.framesEncoded(), s0 = engine.framesSubmitted();
    engine.publishRgb(frame.data());
    REQUIRE(waitFresh(engine, e0, s0));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // The stream: 128 groups x (16 clocks x 3 writes + colour clear + pulse wait + strobe set/clear + pulse fire) + 16 address writes (one per row, CLR + SET) = 6,816 events, exactly. Had the dump clocked the hold words too, each group would carry 129 clocks and the count would be 50,208.
    std::vector<mm::platform::GpioDumpEvent> stream(8000);
    REQUIRE(mm::platform::gpioStubCopyLastDumpStream(stream.data(), stream.size()) == 6816);
    stream.resize(6816);
    std::vector<uint32_t> words(16512);
    REQUIRE(mm::platform::gpioStubCopyLastFrame(words.data(), words.size()) == 16512);

    const std::vector<DecodedGroup> groups = decodeDumpStream(stream, false);
    REQUIRE(groups.size() == 128);   // 16 rows x 8 planes

    const uint32_t red = (1u << mm::platform::kGpioChainPins[0].r1) | (1u << mm::platform::kGpioChainPins[0].r2);
    const uint32_t colour = mm::platform::kGpioColourMask;
    for (int k = 0; k < 128; k++) {
        const int row = k / 8, plane = k % 8;
        const DecodedGroup& g = groups[k];
        CHECK(g.plane == plane);
        CHECK(g.address == row);
        CHECK(g.addrChanged == (plane == 0));
        // The clocked data is the frame's group for this (row, plane) -- the frame is plane-major, so group (plane * 16 + row) -- past its 113 hold words, word for word.
        REQUIRE(g.clocked.size() == 16);
        const size_t base = (static_cast<size_t>(plane) * 16 + row) * 129;
        for (int i = 0; i < 16; i++)
            CHECK(g.clocked[i] == (words[base + 113 + i] & colour));
        // The column frame's crisp pattern: red at clock 7, dark everywhere else, on every plane.
        CHECK(g.clocked[7] == red);
        CHECK(g.clocked[0] == 0u);
        CHECK(g.clocked[15] == 0u);
        CHECK(g.waitIdx < g.strobeIdx);
        CHECK(g.strobeIdx < g.fireIdx);
        if (g.addrChanged) {
            CHECK(g.waitIdx < g.addrIdx);
            CHECK(g.addrIdx < g.strobeIdx);
        }
    }
    engine.shutdown();
}
